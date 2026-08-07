"""Derive and apply the ten drive-calibration values from a measured dead band.

The bench measurement is a single dead band in requested microseconds: the
inclusive range of pulses that produce no shaft motion. Both MG90D units are
assumed to share that band because they come from the same production batch.

Everything else is derived:

    stop        dead-band midpoint (maximum margin to both edges)
    fwd start   first pulse outside the dead band in the forward direction
    rev start   first pulse outside the dead band in the reverse direction
    fwd max     full-scale end on the forward side
    rev max     full-scale end on the reverse side

Which side of the dead band counts as "forward" depends on how each motor is
mounted, so polarity is a per-side option rather than a hardcoded constant.

This tool stages values with `cal set` only. It never runs `cal save confirm`
and never unlocks drive; that stays a deliberate human step.
"""

from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass
from typing import Iterable

# Measured on the bench rig, 2026-08-03: no motion for any requested pulse in
# 1423..1533 inclusive.
DEFAULT_DEADZONE_LOW_US = 1423
DEFAULT_DEADZONE_HIGH_US = 1533

# Mirrors src/drive_calibration.cpp.
PULSE_MIN_US = 1000
PULSE_MAX_US = 2000
STOP_MIN_US = 1450
STOP_MAX_US = 1550
SERVO_PERIOD_US = 20000
SERVO_TIMER_TICKS = 1 << 14

FIELD_ORDER = ("stop", "fwdstart", "revstart", "fwdmax", "revmax")
SIDES = ("left", "right")
SIDE_GPIO = {"left": 18, "right": 17}

PRODUCTION_SERIAL = "A0F262E4C344"
TEST_RIG_SERIAL = "DCB4D92322A0"
KNOWN_BOARDS = {
    PRODUCTION_SERIAL: "production",
    TEST_RIG_SERIAL: "testrig",
}
BOARD_TARGETS = tuple(sorted(set(KNOWN_BOARDS.values())))
REJECTED_PORTS = {"COM3"}
BAUD_RATE = 115200


def quantized_ticks(pulse_us: int) -> int:
    """14-bit / 50 Hz LEDC quantization, matching firmware exactly."""
    return pulse_us * SERVO_TIMER_TICKS // SERVO_PERIOD_US


def emitted_us(pulse_us: int) -> float:
    """Pulse the servo actually sees after quantization."""
    return quantized_ticks(pulse_us) * SERVO_PERIOD_US / SERVO_TIMER_TICKS


@dataclass(frozen=True)
class MotorCalibration:
    stop: int
    fwdstart: int
    revstart: int
    fwdmax: int
    revmax: int

    def as_fields(self) -> tuple[tuple[str, int], ...]:
        return tuple((name, getattr(self, name)) for name in FIELD_ORDER)


def derive_stop(low_us: int, high_us: int) -> int:
    """Dead-band midpoint, biased low so ties keep margin on the high edge.

    The high edge is the risky one: quantization floors toward the dead band,
    so a forward start can land back inside it.
    """
    return (low_us + high_us) // 2


def derive_motor(
    low_us: int,
    high_us: int,
    stop_us: int,
    forward_high: bool,
) -> MotorCalibration:
    """Turn a dead band plus a polarity choice into the five stored fields."""
    above = high_us + 1
    below = low_us - 1
    if forward_high:
        return MotorCalibration(
            stop=stop_us,
            fwdstart=above,
            revstart=below,
            fwdmax=PULSE_MAX_US,
            revmax=PULSE_MIN_US,
        )
    return MotorCalibration(
        stop=stop_us,
        fwdstart=below,
        revstart=above,
        fwdmax=PULSE_MIN_US,
        revmax=PULSE_MAX_US,
    )


def validate_motor(motor: MotorCalibration) -> str | None:
    """Port of DriveCalibration::validateMotorImpl. Returns an error or None."""
    for _, value in motor.as_fields():
        if not PULSE_MIN_US <= value <= PULSE_MAX_US:
            return "pulse must be 1000..2000us"
    if not STOP_MIN_US <= motor.stop <= STOP_MAX_US:
        return "stop must be 1450..1550us"

    stop_ticks = quantized_ticks(motor.stop)
    if (quantized_ticks(motor.fwdstart) == stop_ticks
            or quantized_ticks(motor.revstart) == stop_ticks):
        return "starts must differ from stop after 14-bit quantization"

    forward_high = motor.fwdstart > motor.stop
    if (motor.fwdstart == motor.stop
            or motor.revstart == motor.stop
            or forward_high == (motor.revstart > motor.stop)):
        return "starts must be on opposite sides of stop"

    if forward_high:
        if motor.fwdmax < motor.fwdstart:
            return "forward max must be at or beyond forward start"
        if motor.revmax > motor.revstart:
            return "reverse max must be at or beyond reverse start"
    else:
        if motor.fwdmax > motor.fwdstart:
            return "forward max must be at or beyond forward start"
        if motor.revmax < motor.revstart:
            return "reverse max must be at or beyond reverse start"
    return None


def check_deadzone_escape(
    motor: MotorCalibration, low_us: int, high_us: int
) -> list[str]:
    """Warn when quantization collapses a start pulse back into the dead band.

    The dead band was measured in requested microseconds, and requests are
    floored to a 1.2207us tick, so the comparison has to happen in tick space:
    several distinct requests share one tick and therefore one shaft response.
    Starts must land on a tick strictly outside the band; stop must land inside.
    """
    warnings: list[str] = []
    low_ticks = quantized_ticks(low_us)
    high_ticks = quantized_ticks(high_us)

    stop_ticks = quantized_ticks(motor.stop)
    if not low_ticks <= stop_ticks <= high_ticks:
        warnings.append(
            f"stop {motor.stop}us quantizes to tick {stop_ticks}, outside the "
            f"dead-band ticks {low_ticks}..{high_ticks} - the motor will creep"
        )
    for name in ("fwdstart", "revstart"):
        requested = getattr(motor, name)
        ticks = quantized_ticks(requested)
        if low_ticks <= ticks <= high_ticks:
            warnings.append(
                f"{name} {requested}us quantizes to tick {ticks}, still inside "
                f"the dead-band ticks {low_ticks}..{high_ticks} - "
                f"that request cannot move the shaft"
            )
    return warnings


def build_plan(
    low_us: int,
    high_us: int,
    stop_us: int,
    left_forward_high: bool,
    right_forward_high: bool,
) -> dict[str, MotorCalibration]:
    return {
        "left": derive_motor(low_us, high_us, stop_us, left_forward_high),
        "right": derive_motor(low_us, high_us, stop_us, right_forward_high),
    }


def plan_commands(plan: dict[str, MotorCalibration]) -> list[str]:
    commands: list[str] = []
    for side in SIDES:
        for field, value in plan[side].as_fields():
            commands.append(f"cal set {side} {field} {value}")
    return commands


def format_plan(
    plan: dict[str, MotorCalibration], low_us: int, high_us: int
) -> str:
    lines = [
        f"dead band {low_us}..{high_us}us inclusive (no motion)",
        "",
        f"{'field':<10}"
        f"{'left (GPIO' + str(SIDE_GPIO['left']) + ')':>16}"
        f"{'right (GPIO' + str(SIDE_GPIO['right']) + ')':>16}",
    ]
    for field in FIELD_ORDER:
        left = getattr(plan['left'], field)
        right = getattr(plan['right'], field)
        lines.append(f"{field:<10}{left:>16}{right:>16}")
    for side in SIDES:
        motor = plan[side]
        direction = "high" if motor.fwdstart > motor.stop else "low"
        lines.append(
            f"{side} (GPIO{SIDE_GPIO[side]}) forward = {direction} pulse"
        )
    return "\n".join(lines)


@dataclass(frozen=True)
class PortIdentity:
    device: str
    serial_number: str | None


def normalize_serial(value: str | None) -> str:
    return "" if value is None else value.replace(":", "").upper()


def board_name(serial_number: str | None) -> str | None:
    return KNOWN_BOARDS.get(normalize_serial(serial_number))


def select_port(
    ports: Iterable[PortIdentity],
    requested: str | None = None,
    target: str | None = "production",
) -> str:
    if target is not None and target not in BOARD_TARGETS:
        raise RuntimeError(f"unknown target: {target}")
    candidates = list(ports)
    if requested is not None:
        requested_upper = requested.upper()
        if requested_upper in REJECTED_PORTS:
            raise RuntimeError(f"{requested_upper} is not a robot board")
        matches = [
            port for port in candidates
            if port.device.upper() == requested_upper
        ]
        if len(matches) != 1:
            raise RuntimeError(f"serial port not found: {requested_upper}")
        name = board_name(matches[0].serial_number)
        if name is None:
            raise RuntimeError(f"{requested_upper} is not a known robot board")
        if target is not None and name != target:
            raise RuntimeError(
                f"{requested_upper} is the {name} board, not {target}"
            )
        return matches[0].device

    known = [
        port for port in candidates
        if board_name(port.serial_number) is not None
        and port.device.upper() not in REJECTED_PORTS
    ]
    matches = (
        known if target is None
        else [port for port in known
              if board_name(port.serial_number) == target]
    )
    if len(matches) != 1:
        wanted = target if target is not None else "known robot board"
        found = ", ".join(
            f"{port.device}={board_name(port.serial_number)}"
            for port in known
        ) or "none"
        raise RuntimeError(
            f"expected exactly one {wanted}; found {len(matches)} ({found})"
        )
    return matches[0].device


def send_commands(
    commands: list[str],
    requested_port: str | None,
    target: str | None,
) -> int:
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print("ERR pyserial is required", file=sys.stderr)
        return 2

    identities = [
        PortIdentity(port.device, port.serial_number)
        for port in list_ports.comports()
    ]
    try:
        device = select_port(identities, requested_port, target)
    except RuntimeError as error:
        print(f"ERR {error}", file=sys.stderr)
        return 2

    port = serial.Serial()
    port.port = device
    port.baudrate = BAUD_RATE
    port.timeout = 0.2
    port.write_timeout = 1.0
    port.dtr = False
    port.rts = False

    failures = 0
    try:
        port.open()
        time.sleep(0.2)
        port.reset_input_buffer()
        for command in commands:
            print(f"> {command}")
            port.write((command + "\n").encode("ascii"))
            port.flush()
            deadline = time.monotonic() + 3.0
            settled = False
            while time.monotonic() < deadline:
                raw = port.readline()
                if not raw:
                    continue
                line = raw.decode("ascii", errors="replace").strip()
                if not line:
                    continue
                print(f"  {line}")
                if line == "OK":
                    settled = True
                    break
                if line.startswith("ERR"):
                    failures += 1
                    settled = True
                    break
            if not settled:
                print("  ERR timed out", file=sys.stderr)
                failures += 1
    except (OSError, serial.SerialException) as error:
        print(f"ERR serial failure: {error}", file=sys.stderr)
        return 2
    finally:
        if port.is_open:
            port.close()
    return 1 if failures else 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Stage drive calibration derived from a measured dead band"
    )
    parser.add_argument(
        "--deadzone-low", type=int, default=DEFAULT_DEADZONE_LOW_US,
        help="lowest requested pulse with no motion (inclusive)",
    )
    parser.add_argument(
        "--deadzone-high", type=int, default=DEFAULT_DEADZONE_HIGH_US,
        help="highest requested pulse with no motion (inclusive)",
    )
    parser.add_argument(
        "--stop", type=int, default=None,
        help="override the stop pulse; defaults to the dead-band midpoint",
    )
    parser.add_argument(
        "--left-forward", choices=("high", "low"), default="high",
        help="pulse side that drives the robot's LEFT wheel forward (GPIO18)",
    )
    parser.add_argument(
        "--right-forward", choices=("high", "low"), default="low",
        help="pulse side that drives the robot's RIGHT wheel forward (GPIO17)",
    )
    parser.add_argument("--port")
    parser.add_argument(
        "--target", choices=BOARD_TARGETS, default="production"
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="print the plan and the cal commands without opening the port",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    low, high = args.deadzone_low, args.deadzone_high
    if low > high:
        print("ERR dead-band low must not exceed high", file=sys.stderr)
        return 2
    if not PULSE_MIN_US <= low <= PULSE_MAX_US or \
            not PULSE_MIN_US <= high <= PULSE_MAX_US:
        print("ERR dead band must lie within 1000..2000us", file=sys.stderr)
        return 2

    stop = args.stop if args.stop is not None else derive_stop(low, high)
    plan = build_plan(
        low, high, stop,
        args.left_forward == "high",
        args.right_forward == "high",
    )

    print(format_plan(plan, low, high))
    print()

    failed = False
    for side in SIDES:
        error = validate_motor(plan[side])
        if error is not None:
            print(f"ERR {side}: {error}", file=sys.stderr)
            failed = True
        for warning in check_deadzone_escape(plan[side], low, high):
            print(f"WARN {side}: {warning}", file=sys.stderr)
            failed = True
    if failed:
        return 2

    commands = plan_commands(plan)
    if args.dry_run:
        print("\n".join(commands))
        return 0

    print()
    status = send_commands(commands, args.port, args.target)
    if status == 0:
        print()
        print("Staged. Verify with 'cal show', then persist with:")
        print("  cal save confirm")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
