# DeadlineScope

**A real-time quality gate for QNX 8.0.** DeadlineScope turns *"it looks
fast enough"* into *"the deadline held — here is the measured proof"*,
and gives your CI a verdict it can fail on.

QNX's System Profiler *shows* traces but renders no verdict: no budgets,
no thresholds, no CI gate. The High Availability Manager supervises and
restarts processes but never measures whether their deadlines held.
DeadlineScope is the missing link between the two.

## How it works

**On the target** (`target/dscope.c`, native C, no dependencies beyond
the QNX 8.0 SDP):

- Periodic tasks **register** with the collector over native QNX
  **message passing** (`ChannelCreate` / `MsgSend`), declaring their
  period and jitter budget.
- The collector drives activations with **timer pulses** on its own
  channel and timestamps every activation with **`ClockCycles()`**,
  computing per-task statistics (count, min / average / worst jitter,
  overruns, jitter histogram).
- Live verdicts are exposed through a **resource manager** at
  `/dev/deadlinescope` — `cat /dev/deadlinescope` prints the current
  table from any shell on the target.
- Every raw sample is appended to a compact **binary journal**
  (64-byte header + 32-byte records) for host-side verification.

**On the host** (`host/deadlinescope.py`, Python 3, standard library
only):

```console
$ deadlinescope.py check --journal dscope.journal --budgets budgets.json --html report.html
task              samples  budget_us    min_us    avg_us   worst_us   over  verdict
control-loop          600       3500    1029.8    1994.9     8511.3      2  PASS
telemetry             120      10000     626.5    1619.1     2909.3      0  PASS
OVERALL: PASS        (exit code 0 — 1 on FAIL, so CI can gate on it)
```

The host **recomputes every statistic from the raw samples** — the
gate never trusts the collector's own arithmetic. In the runs above,
target-side and host-side figures agree exactly.

**Gate rule** (per task, from `budgets.json`): a task FAILs when the
share of samples beyond its jitter budget exceeds
`max_overrun_ratio` (default 1 %), **or** any single sample breaches
`hard_cap_us` (default 3× budget). One isolated transient therefore
cannot fail a build; a pattern of lateness always does.

## The demo (PASS → FAIL in one boot)

The collector ships a self-contained demo used for the hackathon
video: two periodic tasks (`control-loop` 20 ms, `telemetry` 100 ms)
run healthy, then a runaway CPU hog at priority 40 is injected on every
core. Measured on the reference rig (QEMU, see *Environment notes*):

| Phase | control-loop | telemetry | Host verdict |
|---|---|---|---|
| Healthy (12 s, 600 samples) | avg 1.99 ms, worst 8.51 ms, 2 over (0.3 %) | PASS | **PASS** (exit 0) |
| Fault injected (20 s, 1001 samples) | avg 2.70 ms, worst 8.02 ms, **202 over (20 %)** | PASS | **FAIL** (exit 1) |

## Build & run (QNX 8.0, QEMU x86_64)

You need the QNX Software Development Platform 8.0 (free
non-commercial licence via myQNX) and QEMU.

```bat
:: On the host, with the SDP environment loaded (qnxsdp-env.bat):
qcc -V gcc_ntox86_64 -Wall target\dscope.c -o dscope

:: Generate a QEMU target image with mkqnximage, add to the image:
::   local\snippets\data_files.custom:
::     [perms=555] dscope = <path to the dscope binary>
::   local\snippets\post_start.custom:
::     /data/dscope --demo            (or run it from a shell)
mkqnximage --type=qemu --arch=x86_64 --noprompt --build=all

:: Boot it (x86_64 non-UEFI images load the IFS via -kernel):
qemu-system-x86_64 -m 1024 -smp 2 -cpu max -accel tcg ^
  -kernel output\ifs.bin -drive file=output\disk-qemu.vmdk,if=ide,id=drv0 ^
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 -display none
```

The demo prints the verdict table on the console and dumps the journal
base64-encoded (markers `===DSCOPE JOURNAL B64===`), so a serial log
capture is all the host needs:

```console
$ deadlinescope.py check --from-log boot.log --budgets budgets.json
```

In production you would copy `/data/dscope.journal` off the target
instead (scp once sshd is configured in the image).

## Environment notes (measured, 2026-11-06)

- The reference rig is QEMU on Windows (Dell Latitude): `-accel tcg`
  is the canonical demo path; `-accel whpx,kernel-irqchip=off` also
  boots but showed a spikier jitter tail.
- The stock QEMU target image runs a fixed 1 ms clock tick;
  `ClockPeriod()` changes are refused by this kernel configuration
  ("Not supported"). Together with virtualization, that sets a
  jitter floor around 1–2 ms average under QEMU — the demo budgets
  (3.5 ms / 10 ms) are chosen for this rig. On real QNX hardware the
  same collector measures at microsecond scale and budgets should be
  set from the workload's actual requirements.
- QNX 8 API notes for porters: `TimerCreate` returns the timer id
  directly, and `TimerSettime` takes `struct _itimer`
  (`nsec` / `interval_nsec`), not POSIX `struct itimerspec`.

## Layout

```
target/dscope.c        collector, resource manager, demo (single file)
host/deadlinescope.py  host gate: journal parser, verdicts, HTML report
budgets.json           demo budgets (period, jitter budget, tolerance, cap)
```

## Licence

MIT — see `LICENSE`.
