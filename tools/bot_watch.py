#!/usr/bin/env python3
"""Passive listener for the two open bugs, for use while working on the robot.

WHAT IT WATCHES
---------------
Two unresolved problems, both intermittent, both previously lost because
nothing was capturing when they happened:

  1. The TASK_WDT resets (crash #9 and counting, all at "loop: touch poll").
  2. The joystick dropout -- control frames stop arriving and the 300 ms
     kControlTimeoutMs fires, hiding the stick behind "WAIT NEUTRAL".

Everything is timestamped into a rolling session log, and anything
interesting is ALSO written to a short events log, so a recurrence does not
mean grepping thousands of lines of ordinary telemetry.

IT NEVER TOUCHES DTR/RTS
------------------------
Asserting DTR and RTS together latches the ESP32-S3 into ROM download mode:
the app never runs, the panel stays dark and the board goes silent. That
happened during debugging and was mistaken for a firmware fault for some
time. This opens the port with both lines deasserted and never toggles them.

IT GETS OUT OF THE WAY FOR FLASHING
-----------------------------------
Holding COM4 makes `pio run -t upload` fail. Create the file
build/watch/PAUSE and this closes the port within a second; delete it and it
reconnects. tools/flash.ps1 does that around an upload for you.

USAGE
-----
    python tools/bot_watch.py                 # COM4, quiet unless something happens
    python tools/bot_watch.py --echo          # also mirror every line
"""

from __future__ import annotations

import argparse
import os
import re
import time
from datetime import datetime
from pathlib import Path

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    raise SystemExit("pyserial required: pip install pyserial")

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "build" / "watch"
PAUSE = OUT / "PAUSE"

# Substring matches. Cheap, and every one of these is a line the firmware only
# emits when something genuinely noteworthy happened.
FLAGS: list[tuple[str, str]] = [
    ("CRASH", "crash: #"),
    ("CRASH", "TASK_WDT"),
    ("CRASH", "Guru Meditation"),
    ("CRASH", "Backtrace"),
    ("BOOT", "boot: reset reason"),
    ("BOOT", "boot: last breadcrumb"),
    ("BOOT", "boot: stall stats"),
    ("BOOT", "boot: I2C bus before"),
    ("DROPOUT", "reason=timeout"),
    ("DROPOUT", "drive: power off (timeout)"),
    ("STALL", "STALL seg"),
    ("STALL", "STALL touch poll"),
    ("STALL", "SLOW driveReset"),
    ("STALL", "SLOW teardown"),
    ("I2C", "i2c: bus recovery"),
    ("I2C", "i2c: bus NOT idle"),
    ("I2C", "touch poll wedged"),
    ("WS", "ingress drain timed out"),
    ("WS", "queue emergency"),
    ("PAIR", "pairing failed"),
]

# The joystick dropout has a precursor worth catching in its own right: the
# control stream thinning out while a client is still connected. Browser timer
# throttling shows up as a clean collapse to ~1/s, which a threshold catches
# well before the 300 ms timeout turns it into a visible fault.
DIAG_RE = re.compile(r"ctrl_rx=(\d+)/s.*?wsClients=(\d+)")
CTRL_FLOOR = 12          # healthy is ~32/s
CTRL_REPEAT_S = 5.0      # don't spam while it stays low


def classify(line: str) -> str | None:
    for tag, needle in FLAGS:
        if needle in line:
            return tag
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--echo", action="store_true", help="mirror every line")
    args = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    session_log = OUT / f"session-{stamp}.log"
    events_log = OUT / "events.log"

    print(f"bot watch: {args.port} -> {session_log}")
    print(f"bot watch: events -> {events_log}")
    print(f"bot watch: create {PAUSE} to release the port for flashing")

    last_ctrl_warn = 0.0

    def record(tag: str, line: str, ts: str) -> None:
        msg = f"{ts} [{tag}] {line}"
        print(msg, flush=True)
        with events_log.open("a", encoding="utf-8") as fh:
            fh.write(msg + "\n")

    while True:
        if PAUSE.exists():
            time.sleep(1.0)
            continue
        try:
            # Configure the control lines BEFORE opening, not after.
            #
            # pyserial asserts DTR when it opens a port. On ESP32-S3 native USB
            # CDC that assert-then-deassert transition IS the auto-reset
            # pattern, so a watcher that opened the port and then set dtr=False
            # rebooted the board every time it attached -- and those reboots
            # were being reported as crashes. A monitor must not perturb the
            # thing it is monitoring.
            ser = serial.Serial()
            ser.port = args.port
            ser.baudrate = args.baud
            ser.timeout = 1
            ser.dtr = False
            ser.rts = False
            ser.open()
            with ser:
                print(f"bot watch: connected {args.port}", flush=True)
                with session_log.open("a", encoding="utf-8") as log:
                    while not PAUSE.exists():
                        raw = ser.readline()
                        if not raw:
                            continue
                        line = raw.decode("utf-8", "replace").rstrip()
                        if not line:
                            continue
                        ts = datetime.now().strftime("%H:%M:%S")
                        log.write(f"{ts} {line}\n")
                        log.flush()
                        if args.echo:
                            print(f"{ts} {line}", flush=True)

                        tag = classify(line)
                        if tag:
                            record(tag, line, ts)
                            continue

                        m = DIAG_RE.search(line)
                        if m:
                            ctrl, clients = int(m.group(1)), int(m.group(2))
                            now = time.time()
                            if (
                                clients >= 1
                                and ctrl < CTRL_FLOOR
                                and (now - last_ctrl_warn) > CTRL_REPEAT_S
                            ):
                                last_ctrl_warn = now
                                record(
                                    "DROPOUT",
                                    f"control stream thin: ctrl_rx={ctrl}/s "
                                    f"with {clients} client(s) -- {line}",
                                    ts,
                                )
        except serial.SerialException as exc:
            # Expected while the board resets or someone else holds the port.
            # Retry rather than exit: a watcher that quietly dies is worse than
            # no watcher, because you believe you are covered.
            print(f"bot watch: {exc}; retrying in 2s", flush=True)
            time.sleep(2.0)
        except KeyboardInterrupt:
            return 0
        else:
            time.sleep(0.5)


if __name__ == "__main__":
    raise SystemExit(main())
