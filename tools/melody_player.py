"""Safe playback primitives for desktop-robot mood melodies.

The firmware gives every active tone a 750 ms lease.  A host that merely
sleeps for a long note will therefore truncate it, so this player renews the
lease every 250 ms and always attempts to leave the piezo silent.
"""

from __future__ import annotations

import time
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from typing import Any, Callable, Protocol

try:
    from melody_pitch import MIN_NOTE_MS, is_playable_hz
except ModuleNotFoundError:
    from tools.melody_pitch import MIN_NOTE_MS, is_playable_hz

try:
    from apply_calibration import (
        BAUD_RATE,
        BOARD_TARGETS,
        KNOWN_BOARDS,
        PRODUCTION_SERIAL,
        REJECTED_PORTS,
        TEST_RIG_SERIAL,
        PortIdentity,
        select_port,
    )
except ModuleNotFoundError:
    from tools.apply_calibration import (
        BAUD_RATE,
        BOARD_TARGETS,
        KNOWN_BOARDS,
        PRODUCTION_SERIAL,
        REJECTED_PORTS,
        TEST_RIG_SERIAL,
        PortIdentity,
        select_port,
    )


# src/piezo_tone.cpp kWatchdogMs. The firmware stops any active tone once this
# long has passed since the last command, so a held note must be re-sent.
PIEZO_WATCHDOG_MS = 750

TONE_REFRESH_SECONDS = 0.250
SERIAL_STARTUP_SECONDS = 0.200
SERIAL_READ_TIMEOUT_SECONDS = 0.250

# Must stay well under the firmware's 750 ms tone watchdog. A write that can
# block for longer than the watchdog would itself guarantee a truncated note,
# so the transport is not permitted to wait that long. At 115200 baud a ~12
# byte command takes about 1 ms, so this is still a ~250x margin.
SERIAL_WRITE_TIMEOUT_SECONDS = 0.250

# Refuse to keep pretending a tone is held once we are this far past the last
# command. Leaves headroom below the 750 ms firmware cutoff for the write
# itself to land.
WATCHDOG_SAFE_SECONDS = 0.600


class ToneTransport(Protocol):
    """The deliberately small command lane needed by :func:`play_notes`."""

    def send(self, command: str) -> None:
        """Send one firmware command without its line terminator."""


class MelodyPlaybackError(RuntimeError):
    """Base error suitable for concise display by the CLI."""


class ToneTimingError(MelodyPlaybackError):
    """The link is too slow to refresh a tone before the firmware drops it.

    Raised rather than silently continuing, because the audible result would
    be a note that dies partway through — a symptom that is very hard to
    attribute to serial latency by ear.
    """


class SerialTransportError(MelodyPlaybackError):
    """A serial device could not be opened or used safely."""


@dataclass(frozen=True)
class Note:
    """Transport-level note timing, independent of generation policy."""

    hz: int
    gate_ms: int
    gap_ms: int = 0


def _integer_field(value: Any, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an integer")
    return value


def _note_fields(note: Any) -> tuple[Any, Any, Any]:
    if isinstance(note, Mapping):
        try:
            return note["hz"], note["gate_ms"], note["gap_ms"]
        except KeyError as exc:
            raise TypeError(
                "notes must provide hz, gate_ms, and gap_ms"
            ) from exc
    if all(hasattr(note, name) for name in ("hz", "gate_ms", "gap_ms")):
        return note.hz, note.gate_ms, note.gap_ms
    if (
        isinstance(note, Sequence)
        and not isinstance(note, (str, bytes, bytearray))
        and len(note) == 3
    ):
        return note[0], note[1], note[2]
    raise TypeError("notes must provide hz, gate_ms, and gap_ms")


def validate_note(note: Any) -> Note:
    """Return a normalized note or fail before an unsafe command is sent."""

    raw_hz, raw_gate_ms, raw_gap_ms = _note_fields(note)
    hz = _integer_field(raw_hz, "hz")
    gate_ms = _integer_field(raw_gate_ms, "gate_ms")
    gap_ms = _integer_field(raw_gap_ms, "gap_ms")
    if not is_playable_hz(hz):
        raise ValueError(f"unplayable tone frequency: {hz} Hz")
    if gate_ms < MIN_NOTE_MS:
        raise ValueError(
            f"gate_ms must be at least {MIN_NOTE_MS} ms, got {gate_ms}"
        )
    if gap_ms < 0:
        raise ValueError(f"gap_ms must be non-negative, got {gap_ms}")
    return Note(hz=hz, gate_ms=gate_ms, gap_ms=gap_ms)


def _wait_until(
    deadline: float,
    *,
    sleep: Callable[[float], None],
    now: Callable[[], float],
) -> None:
    remaining = deadline - now()
    if remaining > 0.0:
        sleep(remaining)


def play_notes(
    notes: Iterable[Any],
    transport: ToneTransport,
    *,
    sleep: Callable[[float], None] = time.sleep,
    now: Callable[[], float] = time.monotonic,
) -> list[str]:
    """Play a phrase while renewing the firmware's active-tone watchdog.

    All notes are validated before the first active command.  The final safety
    stop lives in ``finally`` because a transport or clock failure must never
    strand the robot in an audible state.
    """

    commands: list[str] = []
    final_stop_sent = False
    tone_may_be_active = False
    primary_error: BaseException | None = None
    normalized: list[Note] = []

    def send(command: str) -> None:
        nonlocal final_stop_sent, tone_may_be_active
        if command != "tone 0":
            # A write can reach the UART and still raise while flushing, so
            # cleanup must assume the piezo may have heard an active command.
            tone_may_be_active = True
        transport.send(command)
        commands.append(command)
        if command == "tone 0":
            tone_may_be_active = False
            final_stop_sent = True

    try:
        normalized = [validate_note(note) for note in notes]
        for note in normalized:
            active_command = f"tone {note.hz}"
            # Two different anchors, deliberately kept apart:
            #  * command_started drives watchdog safety. Measuring from before
            #    the write is conservative — the firmware's lease begins when
            #    the byte lands, which is never earlier than this.
            #  * gate_started drives musical duration, and begins when the
            #    tone is actually sounding, i.e. after the write completes.
            command_started = now()
            send(active_command)
            gate_started = now()
            gate_deadline = gate_started + note.gate_ms / 1000.0
            # Anchor each refresh to when the PREVIOUS send began, not to a
            # fixed grid. A slow serial write has to shorten the next interval
            # rather than push it into the future: skipping an overdue slot is
            # exactly how a long note slips past the firmware's 750 ms
            # watchdog and dies mid-sound.
            next_refresh = command_started + TONE_REFRESH_SECONDS

            while note.hz != 0 and next_refresh < gate_deadline:
                _wait_until(next_refresh, sleep=sleep, now=now)
                if now() >= gate_deadline:
                    break
                elapsed = now() - command_started
                if elapsed >= WATCHDOG_SAFE_SECONDS:
                    raise ToneTimingError(
                        "serial is too slow to hold a tone: "
                        f"{elapsed * 1000:.0f} ms since the last command, but "
                        f"the firmware stops the tone at "
                        f"{PIEZO_WATCHDOG_MS} ms"
                    )
                command_started = now()
                send(active_command)
                next_refresh = command_started + TONE_REFRESH_SECONDS

            _wait_until(gate_deadline, sleep=sleep, now=now)
            send("tone 0")
            if note.gap_ms:
                gap_deadline = now() + note.gap_ms / 1000.0
                _wait_until(gap_deadline, sleep=sleep, now=now)
    except BaseException as exc:
        primary_error = exc
        raise
    finally:
        if tone_may_be_active or not final_stop_sent:
            try:
                send("tone 0")
            except Exception:
                if primary_error is None:
                    raise
    return commands


class SerialToneTransport:
    """A pyserial command lane pinned to a known robot USB identity."""

    # A peer that never terminates a line must not grow the buffer forever.
    _MAX_BUFFERED_CHARS = 4096

    def __init__(self, serial_port: Any, device: str) -> None:
        self.serial = serial_port
        self.device = device
        self._read_buffer = ""

    @classmethod
    def connect(
        cls,
        requested_port: str | None = None,
        target: str | None = "production",
        *,
        serial_module: Any | None = None,
        list_ports_provider: Callable[[], Iterable[Any]] | None = None,
        startup_sleep: Callable[[float], None] = time.sleep,
    ) -> "SerialToneTransport":
        """Select and open one known board without falling back by VID."""

        if serial_module is None or list_ports_provider is None:
            try:
                import serial
                from serial.tools import list_ports
            except ImportError as exc:
                raise SerialTransportError(
                    "pyserial is required; install it with "
                    "'py -3.12 -m pip install pyserial'"
                ) from exc
            serial_module = serial if serial_module is None else serial_module
            if list_ports_provider is None:
                list_ports_provider = list_ports.comports

        identities = [
            PortIdentity(port.device, getattr(port, "serial_number", None))
            for port in list_ports_provider()
        ]
        try:
            device = select_port(identities, requested_port, target)
        except RuntimeError as exc:
            raise SerialTransportError(str(exc)) from exc

        serial_port = None
        try:
            serial_port = serial_module.Serial()
            serial_port.port = device
            serial_port.baudrate = BAUD_RATE
            serial_port.timeout = SERIAL_READ_TIMEOUT_SECONDS
            serial_port.write_timeout = SERIAL_WRITE_TIMEOUT_SECONDS
            serial_port.dtr = False
            serial_port.rts = False
            if hasattr(serial_port, "rtscts"):
                serial_port.rtscts = False
            if hasattr(serial_port, "dsrdtr"):
                serial_port.dsrdtr = False
            serial_port.open()
            startup_sleep(SERIAL_STARTUP_SECONDS)
        except BaseException as exc:
            if serial_port is not None:
                try:
                    serial_port.close()
                except Exception:
                    pass
            if not isinstance(exc, Exception):
                raise
            raise SerialTransportError(
                f"could not open {device}; another process may own the serial "
                "port. Stop the MIDI bridge, then retry. "
                f"Underlying error: {exc}"
            ) from exc
        return cls(serial_port, device)

    @property
    def is_open(self) -> bool:
        return bool(getattr(self.serial, "is_open", True))

    def send(self, command: str) -> None:
        if "\r" in command or "\n" in command:
            raise ValueError("serial commands must be a single line")
        if not self.is_open:
            raise SerialTransportError(f"serial port {self.device} is closed")
        payload = (command + "\n").encode("ascii")
        try:
            written = self.serial.write(payload)
            if written is not None and written != len(payload):
                raise OSError(
                    f"short serial write: wrote {written}/{len(payload)} bytes"
                )
            flush = getattr(self.serial, "flush", None)
            if flush is not None:
                flush()
        except Exception as exc:
            raise SerialTransportError(
                f"serial write failed on {self.device} for {command!r}: {exc}"
            ) from exc

    def readline(self) -> str:
        """Return one COMPLETE line, buffering fragments across reads.

        A timed-out ``readline`` returns whatever bytes have arrived so far,
        with no trailing newline. Treating that fragment as a finished line
        would split a message like ``demo step 7/13 mood sad`` across two
        reads and lose the mood event entirely, so partial data is held back
        until its terminator arrives.
        """
        if not self.is_open:
            raise SerialTransportError(f"serial port {self.device} is closed")
        try:
            raw = self.serial.readline()
        except Exception as exc:
            raise SerialTransportError(
                f"serial read failed on {self.device}: {exc}"
            ) from exc
        if not raw:
            return ""
        if isinstance(raw, bytes):
            text = raw.decode("utf-8", errors="replace")
        else:
            text = str(raw)

        self._read_buffer += text
        if not self._read_buffer.endswith(("\n", "\r")):
            # Still mid-line; wait for the terminator rather than emitting a
            # fragment that would never match.
            if len(self._read_buffer) > self._MAX_BUFFERED_CHARS:
                # A peer that never sends a newline must not grow this without
                # bound. Drop the runaway prefix rather than leaking memory.
                self._read_buffer = self._read_buffer[
                    -self._MAX_BUFFERED_CHARS :
                ]
            return ""

        line = self._read_buffer.strip()
        self._read_buffer = ""
        return line

    def read_available_lines(self, max_lines: int = 256) -> list[str]:
        """Drain complete serial backlog so watch mode can follow the latest mood."""

        lines: list[str] = []
        while len(lines) < max_lines:
            try:
                waiting = int(getattr(self.serial, "in_waiting", 0))
            except Exception as exc:
                raise SerialTransportError(
                    f"serial status read failed on {self.device}: {exc}"
                ) from exc
            if waiting <= 0:
                break
            line = self.readline()
            if line:
                lines.append(line)
        return lines

    def close(self) -> None:
        if self.is_open:
            try:
                self.serial.close()
            except Exception as exc:
                raise SerialTransportError(
                    f"failed to close {self.device}: {exc}"
                ) from exc

    def __enter__(self) -> "SerialToneTransport":
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> bool:
        cleanup_error: Exception | None = None
        if self.is_open:
            try:
                self.send("tone 0")
            except Exception as error:
                cleanup_error = error
            try:
                self.close()
            except Exception as error:
                if cleanup_error is None:
                    cleanup_error = error
        if exc_type is None and cleanup_error is not None:
            raise cleanup_error
        return False
