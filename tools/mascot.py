#!/usr/bin/env python3
"""mascot — DeskNotif host CLI.

Pipe commands to a DeskNotif device over USB serial.

Modes:
  mascot <subcommand> [args...]            one-shot
  echo "cmd" | mascot                      stdin
  mascot batch <file>                      batch from file
  mascot watch                             stream serial output

Default port: COM5 (override with --port or MASCOT_PORT env var).
"""

import argparse
import os
import re
import sys
import time

DEFAULT_PORT = os.environ.get("MASCOT_PORT", "COM6")
DEFAULT_BAUD = 921600
DEFAULT_TIMEOUT = 2.0
STREAM_TIMEOUT = 8.0
RETRY_ATTEMPTS = 12
RETRY_DELAY = 0.25

STREAM_LOCAL_QUIT = {"exit", "quit", ".exit", ".quit"}

DEMO_MOODS = [
    "default", "bored", "annoyed", "tired", "angry", "happy", "sad",
    "think", "error", "surprised", "scared", "wink", "sleeping", "love",
    "confused", "curious",
]
DEMO_LOOKS = ["center", "n", "ne", "e", "se", "s", "sw", "w", "nw"]

# Subcommands and their argspecs (used to build argparse + serialize to wire format).
# Every entry: (name, nargs, help)
SUBCOMMANDS = [
    ("ping", 0, "Liveness check"),
    ("status", 0, "Print device state as JSON"),
    ("help", 0, "List device commands"),
    ("demo", 0, "Run cycle of all moods/positions"),
    ("sleep", 0, "Backlight off + pause"),
    ("wake", 0, "Backlight on + resume"),
    ("blink", 0, "One-shot blink"),
    ("mood", 1, "<default|tired|angry|happy|notify|think|thinking|error>"),
    ("look", 1, "<default|center|n|ne|e|se|s|sw|w|nw>"),
    ("wander", 1, "<on|off>"),
    ("dims", 3, "<w> <h> <r>"),
    ("motion", 2, "<x> <y>"),
    ("blinkrate", 2, "<sec> <jitter>"),
    ("color", 1, "<RRGGBB|green|cyan|red|blue|yellow|white|magenta|orange>"),
    ("headgain", 2, "<xDiv> <yDiv>"),
    ("raw", -1, "Send arbitrary text as a line (-1 = all remaining args joined by spaces)"),
]

TERMINATOR_RE = re.compile(r"^(OK|ERR\b.*)$")


def open_serial(port, baud, timeout, allow_retry=True):
    """Open the serial port with auto-retry on busy/permission errors.

    Sets dtr=False AND rts=False before open so the CH343P auto-boot
    circuit does not pulse-reset the ESP32-S3 when we connect. Then waits
    briefly and drains buffers so a stale boot banner or leftover bytes
    from a previous session don't get mixed into our first command.
    """
    import serial

    attempts = RETRY_ATTEMPTS if allow_retry else 1
    last_err = None
    for i in range(attempts):
        try:
            s = serial.Serial()
            s.port = port
            s.baudrate = baud
            s.timeout = timeout
            s.dtr = False
            s.rts = False
            s.rtscts = False
            s.open()
            time.sleep(0.2)
            try:
                s.reset_input_buffer()
                s.reset_output_buffer()
            except Exception:
                pass
            return s
        except (serial.SerialException, PermissionError, OSError) as e:
            last_err = e
            if i == 0 and attempts > 1:
                print(f"waiting for {port}...", file=sys.stderr)
            if i < attempts - 1:
                time.sleep(RETRY_DELAY)
    raise SystemExit(f"failed to open {port}: {last_err}")


def send_command(ser, cmd_line, timeout, verbose=False):
    """Send one line, read until terminator/timeout. Returns (exit_code, content_lines, terminator_or_none)."""
    # drain any pending bytes so async log lines from prior commands don't confuse us
    try:
        ser.reset_input_buffer()
    except Exception:
        pass
    ser.write((cmd_line + "\n").encode("utf-8"))
    ser.flush()
    content = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = max(0.05, deadline - time.monotonic())
        ser.timeout = remaining
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if not line:
            continue
        if TERMINATOR_RE.match(line):
            return (0 if line == "OK" else 1, content, line)
        content.append(line)
    return (2, content, None)  # timeout


def emit_response(content, terminator, verbose):
    for c in content:
        print(c)
    if verbose:
        print(terminator if terminator else "TIMEOUT")
    else:
        print(terminator if terminator else "TIMEOUT", file=sys.stderr)


def serialize_subcommand(name, args):
    if name == "raw":
        return " ".join(str(a) for a in args)
    if not args:
        return name
    return name + " " + " ".join(str(a) for a in args)


def cmd_one_shot(ser, name, args, timeout, verbose):
    line = serialize_subcommand(name, args)
    code, content, term = send_command(ser, line, timeout, verbose=verbose)
    emit_response(content, term, verbose)
    return code


def cmd_stdin(ser, timeout, verbose):
    """Read commands from stdin one per line, send each, aggregate exit code."""
    worst = 0
    for raw in sys.stdin:
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        print(">>>", line, file=sys.stderr)
        code, content, term = send_command(ser, line, timeout, verbose=verbose)
        emit_response(content, term, verbose)
        if code > worst:
            worst = code
    return worst


def cmd_batch(ser, path, timeout, verbose):
    worst = 0
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            print(">>>", line, file=sys.stderr)
            code, content, term = send_command(ser, line, timeout, verbose=verbose)
            emit_response(content, term, verbose)
            if code > worst:
                worst = code
    return worst


def cmd_watch(ser):
    """Read-only stream of serial output until ^C."""
    print(f"watching {ser.port}; press Ctrl-C to stop", file=sys.stderr)
    ser.timeout = 0.1
    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue
            sys.stdout.write(raw.decode("utf-8", errors="replace"))
            sys.stdout.flush()
    except KeyboardInterrupt:
        print("\n(watch stopped)", file=sys.stderr)
        return 0


def cmd_stream(ser, timeout, verbose):
    """Persistent-port line-mode REPL.

    Reads commands from stdin one per line, sends each to the device, prints
    responses. The port stays open across commands so we never DTR-pulse the
    ESP32 between sends. Type 'exit' / 'quit' or send EOF (Ctrl-Z + Enter on
    Windows, Ctrl-D on Unix) to leave cleanly.

    This is the primary entry point for agent-driven (write_powershell) demos
    where each mood swap is a single stdin line.
    """
    print(
        f"streaming on {ser.port} @ {ser.baudrate}; type 'exit' to quit",
        file=sys.stderr, flush=True,
    )
    print("commands echoed to stderr as '>>>', responses to stdout",
          file=sys.stderr, flush=True)
    worst = 0
    try:
        for raw in sys.stdin:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.lower() in STREAM_LOCAL_QUIT:
                print("(stream exit)", file=sys.stderr, flush=True)
                break
            print(">>>", line, file=sys.stderr, flush=True)
            code, content, term = send_command(ser, line, timeout, verbose=verbose)
            emit_response(content, term, verbose)
            sys.stdout.flush()
            if code > worst:
                worst = code
    except KeyboardInterrupt:
        print("\n(stream interrupted)", file=sys.stderr, flush=True)
    return worst


def cmd_tui(ser, timeout):
    """Hands-on TUI picker for live demos. Single-key mood swaps.

    Windows-only (uses msvcrt). Layout: header with current mood + port,
    numbered list of moods (press 1-9, 0, then a/b/c/... for the rest),
    extras at the bottom (b=blink, d=demo, z=sleep, w=wake, arrows=look,
    c=center, q=quit). Responses scroll in the lower pane.
    """
    try:
        import msvcrt
    except ImportError:
        print("--tui mode requires Windows (msvcrt module).", file=sys.stderr)
        return 1

    moods = DEMO_MOODS
    hotkeys = "1234567890abcdefghijklmnopqrstuv"[: len(moods)]
    mood_by_key = {k: m for k, m in zip(hotkeys, moods)}
    current_mood = "default"
    log = []
    LOG_LINES = 8

    CSI = "\x1b["

    def redraw():
        sys.stdout.write(CSI + "2J" + CSI + "H")
        sys.stdout.write(f"=== mascot stream  port={ser.port}  mood={current_mood} ===\n\n")
        sys.stdout.write("MOODS:\n")
        for i, m in enumerate(moods):
            marker = " * " if m == current_mood else "   "
            sys.stdout.write(f"  [{hotkeys[i]}]{marker}{m}\n")
        sys.stdout.write("\nEXTRAS: [B]link  [D]emo  [Z]sleep  [W]ake  [C]enter  arrows=look  [Q]uit\n")
        sys.stdout.write("\nresponses:\n")
        for entry in log[-LOG_LINES:]:
            sys.stdout.write(f"  {entry}\n")
        sys.stdout.flush()

    def send(line):
        nonlocal current_mood
        code, content, term = send_command(ser, line, timeout, verbose=False)
        tag = term if term else "TIMEOUT"
        log.append(f"{line} -> {tag} {' | '.join(content)}".rstrip())
        if line.startswith("mood ") and code == 0:
            current_mood = line.split(" ", 1)[1]

    redraw()
    while True:
        ch = msvcrt.getwch()
        if ch in ("q", "Q", "\x03"):
            break
        elif ch in ("\x00", "\xe0"):
            arrow = msvcrt.getwch()
            mapping = {"H": "n", "P": "s", "K": "w", "M": "e"}
            d = mapping.get(arrow)
            if d:
                send(f"look {d}")
        elif ch in mood_by_key:
            send(f"mood {mood_by_key[ch]}")
        elif ch in ("b", "B"):
            send("blink")
        elif ch in ("d", "D"):
            send("demo")
        elif ch in ("z", "Z"):
            send("sleep")
        elif ch in ("w", "W"):
            send("wake")
        elif ch in ("c", "C"):
            send("look center")
        else:
            log.append(f"(unbound key: {ch!r})")
        redraw()

    sys.stdout.write(CSI + "2J" + CSI + "H")
    sys.stdout.write("(tui exit)\n")
    sys.stdout.flush()
    return 0


def build_parser():
    p = argparse.ArgumentParser(
        prog="mascot",
        description="DeskNotif host CLI (USB serial)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
Examples:
  mascot ping
  mascot mood happy
  mascot dims 12 36 5
  echo "mood angry" | mascot
  mascot batch tests/demo.script
  mascot watch
  mascot stream                  # persistent REPL — type commands, 'exit' to quit
  mascot stream --tui            # single-key full-screen picker (Windows)
""",
    )
    p.add_argument("--port", default=DEFAULT_PORT, help=f"serial port (default: {DEFAULT_PORT} or $MASCOT_PORT)")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD, help=f"baud rate (default: {DEFAULT_BAUD})")
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help=f"response timeout in seconds (default: {DEFAULT_TIMEOUT})")
    p.add_argument("--no-retry", action="store_true", help="don't retry if port is busy")
    p.add_argument("--verbose", "-v", action="store_true", help="print OK/ERR terminator to stdout (not stderr)")
    sub = p.add_subparsers(dest="cmd")

    for name, nargs, helptext in SUBCOMMANDS:
        sp = sub.add_parser(name, help=helptext)
        if nargs == 0:
            pass
        elif nargs > 0:
            sp.add_argument("args", nargs=nargs)
        else:  # raw: -1 means all remaining
            sp.add_argument("args", nargs=argparse.REMAINDER)

    # special CLI-only subcommands
    sp = sub.add_parser("watch", help="stream serial output until Ctrl-C")
    sp = sub.add_parser("batch", help="send commands from a file (one per line)")
    sp.add_argument("file", help="path to a file with one command per line")
    sp = sub.add_parser("stream", help="persistent-port REPL (line per stdin); 'exit' to quit")
    sp.add_argument("--tui", action="store_true",
                    help="full-screen single-key picker (Windows only)")
    sp.add_argument("--stream-timeout", type=float, default=STREAM_TIMEOUT,
                    help=f"per-command timeout in seconds (default: {STREAM_TIMEOUT})")

    return p


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.cmd == "watch":
        ser = open_serial(args.port, args.baud, args.timeout, allow_retry=not args.no_retry)
        try:
            return cmd_watch(ser)
        finally:
            ser.close()

    if args.cmd == "batch":
        ser = open_serial(args.port, args.baud, args.timeout, allow_retry=not args.no_retry)
        try:
            return cmd_batch(ser, args.file, args.timeout, args.verbose)
        finally:
            ser.close()

    if args.cmd == "stream":
        ser = open_serial(args.port, args.baud, args.timeout, allow_retry=not args.no_retry)
        try:
            if args.tui:
                return cmd_tui(ser, args.stream_timeout)
            return cmd_stream(ser, args.stream_timeout, args.verbose)
        finally:
            ser.close()

    if args.cmd is None:
        # stdin mode if not a tty
        if sys.stdin.isatty():
            parser.print_help()
            return 0
        ser = open_serial(args.port, args.baud, args.timeout, allow_retry=not args.no_retry)
        try:
            return cmd_stdin(ser, args.timeout, args.verbose)
        finally:
            ser.close()

    # one-shot
    sub_args = getattr(args, "args", [])
    ser = open_serial(args.port, args.baud, args.timeout, allow_retry=not args.no_retry)
    try:
        return cmd_one_shot(ser, args.cmd, sub_args, args.timeout, args.verbose)
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
