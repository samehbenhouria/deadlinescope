# Codelab — Measure a deadline overrun on QNX 8.0 in 30 minutes

**Goal.** By the end of this codelab you will have a QNX 8.0 target
that *proves* whether a periodic task meets its timing budget — and a
host command that fails your CI when it doesn't.

**Time.** ~30 minutes of hands-on work (plus the one-time QNX SDP
download). **Level.** Beginner-friendly; basic C and command-line
comfort assumed.

**You will build:**

1. A native QNX collector that measures activation jitter with
   `ClockCycles()`, driven by timer pulses over message passing.
2. A live verdict table served by a QNX resource manager at
   `/dev/deadlinescope`.
3. A host-side gate that recomputes the verdict from raw samples and
   exits 0 (PASS) or 1 (FAIL).

---

## Step 0 — Prerequisites (one-time, ~10 min)

1. Create a free myQNX account and accept the **QNX Software
   Development Platform 8.0** non-commercial licence:
   <https://www.qnx.com/getqnx> (QNX Everywhere / "QNX in Every
   University" flows also work).
2. Install the **QNX Software Center**, sign in, and install the
   baseline `com.qnx.sdp/8.0`. Add the QEMU target packages:
   `com.qnx.qnx800.quickstart.qemu.1` and
   `com.qnx.qnx800.target.qemuvirt`.
   *Tip from the field:* if `-installBaseline` complains about a
   missing `com.qnx.qnx800.target` requirement, run it together with
   `-mirror` so the full repository is present.
3. Install QEMU for your host OS (on Windows, the builds at
   <https://qemu.weilnetz.de/w64/> work well).
4. Clone this repository.

## Step 1 — Build the collector (2 min)

With the SDP environment loaded (`qnxsdp-env.bat` on Windows,
`qnxsdp-env.sh` on Linux):

```console
qcc -V gcc_ntox86_64 -Wall target/dscope.c -o dscope
```

One file, no dependencies beyond the SDP. If it builds silently, you
have a QNX 8.0 binary.

## Step 2 — Put it in a QEMU target image (5 min)

Generate a target image once:

```console
mkdir work && cd work
mkqnximage --type=qemu --arch=x86_64 --noprompt
```

Then tell the image builder about DeadlineScope. Append to
`local/snippets/data_files.custom`:

```
[perms=555] dscope = /absolute/path/to/dscope
```

and to `local/snippets/post_start.custom`:

```
/data/dscope --demo --seconds=20
```

Rebuild (the incremental builder ignores snippet edits, force it):

```console
mkqnximage --type=qemu --arch=x86_64 --noprompt --build=all
```

## Step 3 — Boot and watch it measure (3 min)

x86_64 non-UEFI images load the IFS directly (booting the disk image
through its IPL hangs — use `-kernel`):

```console
qemu-system-x86_64 -m 1024 -smp 2 -cpu max -accel tcg ^
  -kernel output\ifs.bin -drive file=output\disk-qemu.vmdk,if=ide,id=drv0 ^
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 -display none ^
  -serial file:boot.log
```

The demo registers two tasks — `control-loop` (20 ms period, 3.5 ms
jitter budget) and `telemetry` (100 ms, 10 ms) — and, halfway through,
injects a runaway CPU hog at priority 40 on every core. In
`boot.log` you will find the collector's verdict table:

```
task              samples    budget       min       avg     worst  overrun  verdict
control-loop         1001      3500       996      2704      8019      202  FAIL
telemetry             200     10000       978      2628      7292        0  PASS
```

*(Numbers from the reference run, 2026-11-06; yours will differ in
detail — the shape is the lesson: the healthy task stays inside its
budget, the interfered one blows through it 202 times.)*

While a target is running (service mode), the same table is available
live from any shell on the target — that is the resource manager at
work:

```console
# cat /dev/deadlinescope
```

## Step 4 — Let the host judge (2 min)

The demo also dumps its binary journal, base64-encoded, into the
serial log. The host gate extracts it, **recomputes every statistic
from the raw samples**, and renders the verdict your CI consumes:

```console
$ python3 host/deadlinescope.py check --from-log boot.log \
      --budgets budgets.json --html report.html
task              samples  budget_us    min_us    avg_us   worst_us   over  verdict
control-loop         1001       3500     997.0    2704.3     8019.7    202  FAIL
telemetry             200      10000     978.1    2628.4     7292.5      0  PASS
OVERALL: FAIL
$ echo $?
1
```

Run the demo with `--no-fault` and the same command prints
`OVERALL: PASS` and exits 0. That exit code is the whole product:
**a build can now fail because a deadline did.**

**The gate rule.** A task FAILs when the share of over-budget samples
exceeds `max_overrun_ratio` (default 1 %) *or* one sample breaches
`hard_cap_us` (default 3× budget) — one isolated transient can't fail
you; a pattern always does.

## Step 5 — Exercises (8 min)

1. **Tighten the budget.** Set `control-loop`'s `jitter_budget_us` to
   `1500` in `budgets.json` and re-run Step 4 on the *healthy* log.
   How many samples overrun now? What does that tell you about the
   jitter floor of a virtualized target (see the README's
   *Environment notes*)?
2. **Slow the victim.** Change the demo's `control-loop` period from
   20 ms to 50 ms, rebuild, reboot. Does the overrun *ratio* under
   the hog go up or down? Why?
3. **Register your own task.** Copy the `dscope_register()` call and
   add a third task with a period and budget from a workload you
   care about. Watch it in `cat /dev/deadlinescope`.

## What you used (QNX-specific technologies)

- **Message passing** — task registration (`ChannelCreate`,
  `MsgSend`/`MsgReceive`, `MsgReply`)
- **Pulses** — timer activations delivered to the collector's channel
  (remember: a pulse event needs a *connection* id, not the channel id)
- **`ClockCycles()`** — cycle-accurate activation timestamps
- **Resource manager** — `/dev/deadlinescope`, a filesystem face on a
  measurement engine
- **QNX 8 API notes** — `TimerCreate` returns the timer id;
  `TimerSettime` takes `struct _itimer`, not `itimerspec`

## Next steps

- Run the collector in service mode on a real workload and ship the
  journal (`/data/dscope.journal`) to your CI.
- Read `target/dscope.c` — it is deliberately a single commented
  file; the measurement core is ~100 lines.
