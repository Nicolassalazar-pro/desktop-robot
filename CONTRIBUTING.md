# Contributing

Thanks for helping improve Desktop Robot. This is a physical control system, so
small, measured changes are preferred over broad rewrites.

## Before changing code

1. Read [`docs/PROJECT-HISTORY.md`](docs/PROJECT-HISTORY.md), especially the
   rejected approaches.
2. Read [`HARDWARE.md`](HARDWARE.md) before changing pins, buses, clocks, power,
   servo control, or board initialization.
3. Check [`docs/key-issues/`](docs/key-issues/) for open reliability, security,
   testing, performance, and dependency findings.

## Safety rules

- Never validate movement while USB is attached.
- Never remove or weaken the 300 ms drive lease without an independent physical
  safety mechanism and hardware evidence.
- Never block the watchdog-fed loop with animation sleeps or unbounded I/O.
- Never drive the passive piezo pin continuously high.
- Preserve the early `SYS_EN` assertion required for battery operation.

## Validation

Run the host suite before opening a pull request:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File test\run_motor_host_tests.ps1
```

Build the firmware with the pinned PlatformIO environment:

```powershell
pio run
```

For UI changes, render the real shared drawing code:

```powershell
powershell -File tools\host_preview\build.ps1
powershell -File tools\web_preview\build.ps1
```

Physical motion changes must include the untethered measurement procedure and
observed results. A successful compile is not evidence that a turn is correct.

## Pull requests

Keep each pull request focused. Explain:

- what changed
- why the current behavior is wrong
- what measurement or test proves the new behavior
- what hardware state was used for validation
- any known limitation that remains

CAD changes under `body models/` are covered by CERN-OHL-S-2.0. Software and
project-authored documentation are covered by MIT. See [`LICENSE.md`](LICENSE.md).
