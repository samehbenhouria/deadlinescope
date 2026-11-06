#!/usr/bin/env python3
"""DeadlineScope — host-side quality gate.

Reads the binary journal produced by the QNX target collector and
renders a PASS/FAIL verdict per task against a budgets file.
The host recomputes every statistic from the raw samples: the gate
never trusts the collector's own arithmetic.

Usage:
    deadlinescope.py check --journal dscope.journal --budgets budgets.json
    deadlinescope.py check --from-log boot.log     --budgets budgets.json
    ... [--html report.html]

Exit code: 0 if every budgeted task PASSes, 1 on any FAIL,
2 on usage/input errors. Standard library only.

Journal format (little-endian):
    header  64 bytes: magic "DSJ1", u64 cycles_per_sec, u64 start_cycles
    record  32 bytes: u32 type, u32 task_id, 24-byte payload
        type 1 (name):   payload = task name, NUL-padded ASCII
        type 2 (sample): payload = u64 expected_cycles,
                                   u64 actual_cycles, i64 jitter_ns
"""

import argparse
import base64
import html
import json
import struct
import sys

HEADER = struct.Struct("<4sIQQ40s")   # exactly 64 bytes, matches the C struct
RECORD = struct.Struct("<II24s")
SAMPLE = struct.Struct("<QQq")
REC_NAME, REC_SAMPLE = 1, 2

HIST_BOUNDS_NS = [10_000, 50_000, 100_000, 250_000, 500_000,
                  1_000_000, 2_000_000, 5_000_000, 10_000_000, None]


def parse_journal(data: bytes):
    if len(data) < HEADER.size:
        raise ValueError("journal too short for header")
    magic, _ver, cps, _start, _res = HEADER.unpack_from(data, 0)
    if magic != b"DSJ1":
        raise ValueError(f"bad journal magic: {magic!r}")
    names, samples = {}, {}
    off = HEADER.size
    while off + RECORD.size <= len(data):
        rtype, task_id, payload = RECORD.unpack_from(data, off)
        off += RECORD.size
        if rtype == REC_NAME:
            names[task_id] = payload.split(b"\0", 1)[0].decode("ascii", "replace")
        elif rtype == REC_SAMPLE:
            _exp, _act, jitter_ns = SAMPLE.unpack(payload)
            samples.setdefault(task_id, []).append(jitter_ns)
    tasks = []
    for task_id, js in samples.items():
        tasks.append({"id": task_id,
                      "name": names.get(task_id, f"task-{task_id}"),
                      "jitters_ns": js})
    return cps, tasks


def extract_journal_from_log(text: str) -> bytes:
    begin = text.find("===DSCOPE JOURNAL B64===")
    end = text.find("===END DSCOPE JOURNAL B64===")
    if begin == -1 or end == -1 or end <= begin:
        raise ValueError("no DSCOPE JOURNAL B64 block found in log")
    block = text[begin:end]
    lines = [ln.strip() for ln in block.splitlines()[1:] if ln.strip()]
    return base64.b64decode("".join(lines))


def stats_for(jitters_ns):
    n = len(jitters_ns)
    hist = [0] * len(HIST_BOUNDS_NS)
    for j in jitters_ns:
        for i, bound in enumerate(HIST_BOUNDS_NS):
            if bound is None or j <= bound:
                hist[i] += 1
                break
    return {
        "samples": n,
        "min_us": min(jitters_ns) / 1000.0,
        "avg_us": (sum(jitters_ns) / n) / 1000.0 if n else 0.0,
        "worst_us": max(jitters_ns) / 1000.0,
        "hist": hist,
    }


def cmd_check(args) -> int:
    if args.journal:
        with open(args.journal, "rb") as f:
            data = f.read()
    else:
        with open(args.from_log, "r", errors="replace") as f:
            data = extract_journal_from_log(f.read())
    _cps, tasks = parse_journal(data)
    with open(args.budgets) as f:
        budgets = json.load(f)["tasks"]

    rows, all_pass = [], True
    for t in sorted(tasks, key=lambda x: x["name"]):
        st = stats_for(t["jitters_ns"])
        b = budgets.get(t["name"])
        if b is None:
            verdict, budget_us, over = "NO-BUDGET", None, 0
        else:
            budget_us = float(b["jitter_budget_us"])
            # Gate rule: FAIL when the overrun ratio exceeds the
            # budget's tolerance (default 1% of samples) or any single
            # sample breaches the hard cap (default 3x budget). One
            # isolated transient on a virtualized rig must not fail a
            # build; a pattern of lateness must.
            max_ratio = float(b.get("max_overrun_ratio", 0.01))
            hard_cap_us = float(b.get("hard_cap_us", 3 * budget_us))
            over = sum(1 for j in t["jitters_ns"] if j / 1000.0 > budget_us)
            ratio = over / st["samples"] if st["samples"] else 0.0
            verdict = ("FAIL" if (ratio > max_ratio
                                  or st["worst_us"] > hard_cap_us)
                       else "PASS")
            if verdict == "FAIL":
                all_pass = False
        rows.append({**t, **st, "budget_us": budget_us,
                     "overruns": over, "verdict": verdict})

    print(f"{'task':<16} {'samples':>8} {'budget_us':>10} {'min_us':>9} "
          f"{'avg_us':>9} {'worst_us':>10} {'over':>6}  verdict")
    for r in rows:
        b = f"{r['budget_us']:.0f}" if r["budget_us"] is not None else "-"
        print(f"{r['name']:<16} {r['samples']:>8} {b:>10} {r['min_us']:>9.1f} "
              f"{r['avg_us']:>9.1f} {r['worst_us']:>10.1f} {r['overruns']:>6}  "
              f"{r['verdict']}")
    print("OVERALL:", "PASS" if all_pass else "FAIL")

    if args.html:
        with open(args.html, "w") as f:
            f.write(render_html(rows, all_pass))
        print("HTML report:", args.html)
    return 0 if all_pass else 1


def render_html(rows, all_pass) -> str:
    trs = []
    for r in rows:
        cls = "pass" if r["verdict"] == "PASS" else "fail"
        bars = "".join(
            f'<div class="bar" style="width:{(c / max(1, r["samples"])) * 100:.1f}%"></div>'
            for c in r["hist"])
        trs.append(
            f"<tr><td>{html.escape(r['name'])}</td><td>{r['samples']}</td>"
            f"<td>{r['min_us']:.1f}</td><td>{r['avg_us']:.1f}</td>"
            f"<td>{r['worst_us']:.1f}</td><td>{r['overruns']}</td>"
            f'<td class="{cls}">{r["verdict"]}</td>'
            f'<td class="hist">{bars}</td></tr>')
    return f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<title>DeadlineScope report</title>
<style>
body{{font-family:system-ui,sans-serif;margin:2em;color:#111}}
table{{border-collapse:collapse}}td,th{{border:1px solid #bbb;padding:.4em .7em}}
.pass{{color:#0a7d2c;font-weight:700}}.fail{{color:#b00020;font-weight:700}}
.hist{{width:220px}}.bar{{height:5px;background:#4477aa;margin:1px 0}}
h1 .pass,h1 .fail{{font-size:1em}}
</style></head><body>
<h1>DeadlineScope report — overall <span class="{'pass' if all_pass else 'fail'}">
{'PASS' if all_pass else 'FAIL'}</span></h1>
<table><tr><th>task</th><th>samples</th><th>min µs</th><th>avg µs</th>
<th>worst µs</th><th>overruns</th><th>verdict</th><th>jitter histogram</th></tr>
{''.join(trs)}</table>
<p>Jitter = activation lateness measured on the target with ClockCycles().
Verdicts recomputed on the host from raw journal samples.</p>
</body></html>"""


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="deadlinescope")
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check", help="verify a journal against budgets")
    src = c.add_mutually_exclusive_group(required=True)
    src.add_argument("--journal", help="binary journal file from the target")
    src.add_argument("--from-log", help="text log containing the "
                                        "DSCOPE JOURNAL B64 block")
    c.add_argument("--budgets", required=True, help="budgets JSON file")
    c.add_argument("--html", help="also write an HTML report here")
    args = p.parse_args(argv)
    try:
        if args.cmd == "check":
            return cmd_check(args)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as e:
        print(f"deadlinescope: error: {e}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    sys.exit(main())
