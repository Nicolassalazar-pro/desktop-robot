#!/usr/bin/env python3
"""Bridge a Windows MIDI input to the desktop-robot USB serial tone command."""

from __future__ import annotations

import argparse
import importlib
import inspect
import logging
import sys
import time
from dataclasses import dataclass
from typing import Any, Callable, Iterable, Sequence


DEFAULT_MIDI_NAME = "Robot MIDI"
DEFAULT_SERIAL_NUMBER = "A0:F2:62:E4:C3:44"
ESPRESSIF_VID = 0x303A
REJECTED_SERIAL_PORT = "COM3"
SERIAL_BAUD = 115200
SERIAL_STARTUP_DRAIN_SECONDS = 0.2
HEARTBEAT_NS = 250_000_000
FIRMWARE_WATCHDOG_NS = 750_000_000
RESPONSE_TIMEOUT_NS = 200_000_000
MAX_RESPONSE_HISTORY = 64
MAX_RECONNECT_ATTEMPTS = 5
RECONNECT_DELAYS = (0.25, 0.5, 1.0, 2.0, 2.0)
LOOP_SLEEP_SECONDS = 0.005

LOG = logging.getLogger("robot-midi")


class BridgeError(RuntimeError):
    """Base error shown as a concise CLI failure."""


class DeviceSelectionError(BridgeError):
    """Raised when a MIDI or serial device cannot be selected safely."""


class DeviceUnavailableError(DeviceSelectionError):
    """Raised when an otherwise valid device is temporarily absent."""


class TransportError(BridgeError):
    """Raised when the serial command lane stops responding."""


class FirmwareRejectedError(BridgeError):
    """Raised when firmware rejects a correlated tone command."""


class MidiDisconnected(BridgeError):
    """Raised when the selected MIDI input disappears."""


@dataclass(frozen=True)
class RuntimeDependencies:
    mido: Any
    serial_module: Any | None = None
    list_ports: Callable[[], Iterable[Any]] | None = None


@dataclass(frozen=True)
class SerialUsbIdentity:
    device: str
    serial_number: str
    vid: Any
    pid: Any
    location: str
    hwid: str
    manufacturer: str
    product: str
    interface: str


@dataclass
class DeviceIdentityPins:
    midi_name: str | None = None
    midi_index: int | None = None
    midi_names: tuple[str, ...] | None = None
    serial: SerialUsbIdentity | None = None


def _channel(value: str) -> int | None:
    if value.casefold() == "all":
        return None
    try:
        channel = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("channel must be 'all' or 1..16") from exc
    if not 1 <= channel <= 16:
        raise argparse.ArgumentTypeError("channel must be 'all' or 1..16")
    return channel - 1


def _minimum_gate(value: str) -> int:
    try:
        milliseconds = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("minimum gate must be an integer") from exc
    if milliseconds < 45:
        raise argparse.ArgumentTypeError("minimum gate must be at least 45 ms")
    return milliseconds


def _octave_shift(value: str) -> int:
    try:
        octaves = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("octave shift must be an integer") from exc
    if abs(octaves) > 8:
        raise argparse.ArgumentTypeError("octave shift must be within -8..8")
    return octaves


def _nonnegative_index(value: str) -> int:
    try:
        index = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("MIDI index must be an integer") from exc
    if index < 0:
        raise argparse.ArgumentTypeError("MIDI index must be zero or greater")
    return index


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--list-devices",
        action="store_true",
        help="list MIDI inputs and USB serial ports, then exit",
    )
    midi_group = parser.add_mutually_exclusive_group()
    midi_group.add_argument("--midi-port", help="MIDI input name (exact or unique partial match)")
    midi_group.add_argument(
        "--midi-index",
        type=_nonnegative_index,
        help="zero-based MIDI input index from --list-devices",
    )
    parser.add_argument("--serial-port", help="explicit COM port")
    parser.add_argument(
        "--serial-number",
        default=DEFAULT_SERIAL_NUMBER,
        help=f"preferred USB serial number (default: {DEFAULT_SERIAL_NUMBER})",
    )
    parser.add_argument(
        "--channel",
        type=_channel,
        default=None,
        metavar="all|1..16",
        help="MIDI channel (default: all)",
    )
    parser.add_argument(
        "--sustain",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="honor sustain-pedal CC messages (default: enabled)",
    )
    parser.add_argument(
        "--min-gate-ms",
        type=_minimum_gate,
        default=45,
        metavar="MS",
        help="minimum note gate in milliseconds, at least 45 (default: 45)",
    )
    parser.add_argument(
        "--octave-shift",
        type=_octave_shift,
        default=0,
        metavar="N",
        help="transpose every note by N octaves before playing (default: 0)",
    )
    parser.add_argument(
        "--fold-octaves",
        action="store_true",
        help="legacy: collapse every pitch class into A6..G#7, losing octaves",
    )
    parser.add_argument(
        "--reconnect",
        action="store_true",
        help=f"retry a lost device up to {MAX_RECONNECT_ATTEMPTS} times",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="read MIDI and log tone commands without opening serial",
    )
    parser.add_argument(
        "--log-level",
        choices=("DEBUG", "INFO", "WARNING", "ERROR"),
        default="INFO",
    )
    return parser


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    return build_parser().parse_args(argv)


def _import_dependency(module_name: str, install_name: str | None = None) -> Any:
    try:
        return importlib.import_module(module_name)
    except ImportError as exc:
        package = install_name or module_name
        raise BridgeError(
            f"missing dependency {package!r}; install tools\\requirements-midi-bridge.txt"
        ) from exc


def load_midi_dependencies() -> Any:
    _import_dependency("rtmidi", "python-rtmidi")
    mido = _import_dependency("mido")
    set_backend = getattr(mido, "set_backend", None)
    if set_backend is not None:
        set_backend("mido.backends.rtmidi")
    return mido


def load_serial_dependencies() -> tuple[Any, Callable[[], Iterable[Any]]]:
    serial_module = _import_dependency("serial", "pyserial")
    list_ports_module = _import_dependency("serial.tools.list_ports", "pyserial")
    return serial_module, list_ports_module.comports


def load_core_engine_factory() -> Callable[..., Any]:
    module_names = []
    if __package__:
        module_names.append(f"{__package__}.midi_bridge_core")
    module_names.append("midi_bridge_core")
    last_error: ImportError | None = None
    for module_name in module_names:
        try:
            module = importlib.import_module(module_name)
            return module.MidiNoteEngine
        except ImportError as exc:
            last_error = exc
    raise BridgeError("cannot import the locked midi_bridge_core.MidiNoteEngine") from last_error


def _match_midi_name(
    names: Sequence[str],
    query: str,
    *,
    unavailable_when_missing: bool = False,
) -> str:
    literal = [name for name in names if name == query]
    if len(literal) == 1:
        return literal[0]
    if len(literal) > 1:
        raise DeviceSelectionError(f"MIDI name {query!r} is duplicated; use --midi-index")

    folded_query = query.casefold()
    exact = [name for name in names if name.casefold() == folded_query]
    if len(exact) == 1:
        return exact[0]
    if len(exact) > 1:
        raise DeviceSelectionError(f"MIDI name {query!r} is ambiguous; use --midi-index")

    partial = [name for name in names if folded_query in name.casefold()]
    if len(partial) == 1:
        return partial[0]
    if len(partial) > 1:
        choices = ", ".join(repr(name) for name in partial)
        raise DeviceSelectionError(
            f"MIDI name {query!r} matches multiple inputs: {choices}; "
            "use --midi-port with an exact name or --midi-index"
        )
    available = ", ".join(repr(name) for name in names) or "(none)"
    error_type = DeviceUnavailableError if unavailable_when_missing else DeviceSelectionError
    raise error_type(f"MIDI input {query!r} not found; available: {available}")


def select_midi_port(
    names: Sequence[str],
    requested_port: str | None = None,
    requested_index: int | None = None,
) -> str:
    if requested_port is not None and requested_index is not None:
        raise DeviceSelectionError("--midi-port and --midi-index cannot be combined")
    if requested_index is not None:
        if requested_index >= len(names):
            raise DeviceUnavailableError(
                f"MIDI index {requested_index} is out of range; "
                f"found {len(names)} input(s), indexed 0..{max(len(names) - 1, 0)}"
            )
        return names[requested_index]
    return _match_midi_name(
        names,
        requested_port or DEFAULT_MIDI_NAME,
        unavailable_when_missing=True,
    )


def _port_device(port: Any) -> str:
    return str(getattr(port, "device", port))


def _normalize_serial_number(value: Any) -> str:
    if value is None:
        return ""
    return "".join(character for character in str(value).upper() if character.isalnum())


def _serial_choices(ports: Sequence[Any]) -> str:
    return ", ".join(_port_device(port) for port in ports) or "(none)"


def _serial_usb_identity(port: Any) -> SerialUsbIdentity:
    return SerialUsbIdentity(
        device=_port_device(port).casefold(),
        serial_number=_normalize_serial_number(getattr(port, "serial_number", None)),
        vid=getattr(port, "vid", None),
        pid=getattr(port, "pid", None),
        location=str(getattr(port, "location", None) or ""),
        hwid=str(getattr(port, "hwid", None) or ""),
        manufacturer=str(getattr(port, "manufacturer", None) or ""),
        product=str(getattr(port, "product", None) or ""),
        interface=str(getattr(port, "interface", None) or ""),
    )


def _select_pinned_serial_port(
    ports: Sequence[Any],
    identity: SerialUsbIdentity,
) -> str:
    if not identity.serial_number:
        raise DeviceSelectionError(
            "cannot safely reconnect the original serial USB device without "
            "a non-empty USB serial number"
        )
    matches = [port for port in ports if _serial_usb_identity(port) == identity]
    if len(matches) == 1:
        return _port_device(matches[0])
    if len(matches) > 1:
        raise DeviceSelectionError(
            f"original serial USB identity is duplicated: {_serial_choices(matches)}"
        )
    raise DeviceUnavailableError(
        "original serial USB device is unavailable; refusing a replacement "
        f"(expected {identity.device}, available: {_serial_choices(ports)})"
    )


def select_serial_port(
    ports: Sequence[Any],
    explicit_port: str | None = None,
    serial_number: str | None = DEFAULT_SERIAL_NUMBER,
) -> str:
    if explicit_port and explicit_port.casefold() == REJECTED_SERIAL_PORT.casefold():
        raise DeviceSelectionError(
            f"{REJECTED_SERIAL_PORT} is reserved for Intel AMT serial-over-LAN and is rejected"
        )

    if explicit_port:
        matches = [
            port
            for port in ports
            if _port_device(port).casefold() == explicit_port.casefold()
        ]
        if len(matches) == 1:
            device = _port_device(matches[0])
            if device.casefold() == REJECTED_SERIAL_PORT.casefold():
                raise DeviceSelectionError(f"{REJECTED_SERIAL_PORT} is rejected")
            return device
        if len(matches) > 1:
            raise DeviceSelectionError(f"serial port {explicit_port!r} is ambiguous")
        raise DeviceUnavailableError(
            f"serial port {explicit_port!r} not found; available: {_serial_choices(ports)}"
        )

    safe_ports = [
        port
        for port in ports
        if _port_device(port).casefold() != REJECTED_SERIAL_PORT.casefold()
    ]
    wanted_serial = _normalize_serial_number(serial_number)
    if wanted_serial:
        serial_matches = [
            port
            for port in safe_ports
            if _normalize_serial_number(getattr(port, "serial_number", None)) == wanted_serial
        ]
        if len(serial_matches) == 1:
            return _port_device(serial_matches[0])
        if len(serial_matches) > 1:
            raise DeviceSelectionError(
                f"USB serial number {serial_number!r} matches multiple ports: "
                f"{_serial_choices(serial_matches)}"
            )

    vid_matches = [
        port for port in safe_ports if getattr(port, "vid", None) == ESPRESSIF_VID
    ]
    if len(vid_matches) == 1:
        return _port_device(vid_matches[0])
    if len(vid_matches) > 1:
        raise DeviceSelectionError(
            f"multiple VID {ESPRESSIF_VID:04X} serial devices found: "
            f"{_serial_choices(vid_matches)}; use --serial-port or --serial-number"
        )
    raise DeviceUnavailableError(
        f"desktop robot serial device not found (serial {serial_number!r}, "
        f"VID {ESPRESSIF_VID:04X}); available: {_serial_choices(safe_ports)}"
    )


def open_serial_device(serial_module: Any, port: str) -> Any:
    serial_device = None
    try:
        serial_device = serial_module.Serial()
        serial_device.port = port
        serial_device.baudrate = SERIAL_BAUD
        serial_device.timeout = 0
        serial_device.write_timeout = 0.5
        serial_device.dtr = False
        serial_device.rts = False
        serial_device.rtscts = False
        serial_device.dsrdtr = False
        serial_device.open()
    except BaseException as exc:
        _best_effort_serial_silence(serial_device)
        _close_quietly(serial_device)
        if not isinstance(exc, Exception):
            raise
        raise TransportError(f"failed to open {port}: {exc}") from exc
    return serial_device


class SerialToneTransport:
    """Single-command serial lane with state coalescing and active-tone heartbeat."""

    def __init__(
        self,
        serial_device: Any | None,
        *,
        dry_run: bool = False,
        heartbeat_ns: int = HEARTBEAT_NS,
        response_timeout_ns: int = RESPONSE_TIMEOUT_NS,
        logger: logging.Logger = LOG,
    ) -> None:
        self.serial = serial_device
        self.dry_run = dry_run
        self.heartbeat_ns = heartbeat_ns
        self.response_timeout_ns = response_timeout_ns
        self.logger = logger
        self.desired_hz: int | None = None
        self.in_flight_hz: int | None = None
        self.in_flight_since_ns: int | None = None
        self.last_completed_hz: int | None = None
        self.last_sent_ns: int | None = None
        self.responses: list[str] = []
        self._read_buffer = bytearray()
        self._response_sync_lost = False
        self._terminal_error: FirmwareRejectedError | None = None

    def _serial_is_closed(self) -> bool:
        return self.serial is not None and getattr(self.serial, "is_open", True) is False

    def _read_lines(self) -> list[str]:
        if self.dry_run or self.serial is None:
            return []
        if self._serial_is_closed():
            raise TransportError("serial device disconnected")
        lines: list[str] = []
        for _ in range(256):
            try:
                waiting = getattr(self.serial, "in_waiting", None)
                if waiting is not None and int(waiting) <= 0:
                    break
                raw = self.serial.readline()
            except Exception as exc:
                raise TransportError(f"serial read failed: {exc}") from exc
            if not raw:
                break
            if isinstance(raw, bytes):
                self._read_buffer.extend(raw)
            else:
                self._read_buffer.extend(str(raw).encode("utf-8", errors="replace"))
            while b"\n" in self._read_buffer:
                raw_line, _, remainder = self._read_buffer.partition(b"\n")
                self._read_buffer = bytearray(remainder)
                line = raw_line.rstrip(b"\r").decode("utf-8", errors="replace").strip()
                if line:
                    lines.append(line)
        return lines

    def drain_responses(self) -> list[str]:
        lines = self._read_lines()
        for line in lines:
            self.responses.append(line)
            if len(self.responses) > MAX_RESPONSE_HISTORY:
                del self.responses[:-MAX_RESPONSE_HISTORY]
            upper = line.upper()
            if upper == "OK" or upper.startswith("ERR"):
                if self._response_sync_lost:
                    self.logger.debug(
                        "ignored uncorrelatable response after timeout: %s", line
                    )
                    continue
                if self.in_flight_hz is not None:
                    completed_hz = self.in_flight_hz
                    self.in_flight_hz = None
                    self.in_flight_since_ns = None
                    if upper == "OK":
                        self.last_completed_hz = completed_hz
                    else:
                        self.last_completed_hz = None
                        self.logger.warning("robot rejected tone %s: %s", completed_hz, line)
                        self._terminal_error = FirmwareRejectedError(
                            f"robot rejected tone {completed_hz}: {line}"
                        )
                        raise self._terminal_error
                else:
                    self.logger.debug("drained startup/stale response: %s", line)
            else:
                self.logger.debug("drained robot output: %s", line)
        return lines

    def drain_startup(self) -> list[str]:
        return self.drain_responses()

    def _write(self, hz: int, now_ns: int) -> None:
        command = f"tone {hz}"
        if self.dry_run:
            self.logger.info("[dry-run] %s", command)
            self.last_sent_ns = now_ns
            self.last_completed_hz = hz
            return
        if self.serial is None or self._serial_is_closed():
            raise TransportError("serial device is not open")
        try:
            self.serial.write((command + "\n").encode("ascii"))
            flush = getattr(self.serial, "flush", None)
            if flush is not None:
                flush()
        except Exception as exc:
            raise TransportError(f"serial write failed for {command!r}: {exc}") from exc
        self.logger.debug("sent %s", command)
        self.last_sent_ns = now_ns
        self.in_flight_hz = hz
        self.in_flight_since_ns = now_ns

    def set_tone(self, hz: int, now_ns: int | None = None, *, force: bool = False) -> None:
        if hz < 0:
            raise ValueError("tone frequency cannot be negative")
        now_ns = time.monotonic_ns() if now_ns is None else now_ns
        self.desired_hz = int(hz)
        self._maybe_send(now_ns, force=force)

    def _maybe_send(self, now_ns: int, *, force: bool = False) -> None:
        if self.desired_hz is None:
            return
        if self._terminal_error is not None and not (
            force and self.desired_hz == 0
        ):
            raise self._terminal_error
        if self.in_flight_hz is not None:
            return
        state_changed = self.desired_hz != self.last_completed_hz
        heartbeat_due = (
            self.desired_hz > 0
            and self.last_sent_ns is not None
            and now_ns - self.last_sent_ns >= self.heartbeat_ns
        )
        if force or state_changed or heartbeat_due:
            self._write(self.desired_hz, now_ns)

    def poll(self, now_ns: int | None = None) -> None:
        if self._terminal_error is not None:
            raise self._terminal_error
        now_ns = time.monotonic_ns() if now_ns is None else now_ns
        self.drain_responses()
        if (
            self.in_flight_since_ns is not None
            and now_ns - self.in_flight_since_ns >= self.response_timeout_ns
        ):
            timed_out_hz = self.in_flight_hz
            self.in_flight_hz = None
            self.in_flight_since_ns = None
            self.last_completed_hz = None
            self._response_sync_lost = True
            self.logger.warning(
                "timeout awaiting response to tone %s; renewing newest tone state",
                timed_out_hz,
            )
            self._maybe_send(now_ns, force=True)
        self._maybe_send(now_ns)

    def startup(self, now_ns: int | None = None) -> None:
        now_ns = time.monotonic_ns() if now_ns is None else now_ns
        self.drain_startup()
        self.set_tone(0, now_ns, force=True)
        self.poll(now_ns)

    def shutdown_silence(
        self,
        *,
        clock_ns: Callable[[], int] = time.monotonic_ns,
        sleep: Callable[[float], None] = time.sleep,
        timeout_seconds: float = 0.25,
    ) -> None:
        start_ns = clock_ns()
        deadline_ns = start_ns + int(timeout_seconds * 1_000_000_000)
        try:
            self.set_tone(0, start_ns, force=True)
            while self.in_flight_hz is not None and clock_ns() < deadline_ns:
                self.poll(clock_ns())
                if self.in_flight_hz is not None:
                    sleep(0.005)
            if self.in_flight_hz is not None and self.in_flight_hz != 0:
                self.logger.warning("forcing safety tone 0 after an unresponsive command")
                self.in_flight_hz = None
                self.in_flight_since_ns = None
                self.desired_hz = 0
                self._write(0, clock_ns())
        except Exception as exc:
            self.logger.warning("could not confirm safety tone 0: %s", exc)


def _build_engine(
    engine_factory: Callable[..., Any],
    *,
    channel: int | None,
    sustain: bool,
    min_gate_ms: int,
    octave_shift: int = 0,
    fold_octaves: bool = False,
    clock_ns: Callable[[], int] | None = None,
) -> Any:
    try:
        signature = inspect.signature(engine_factory)
        parameters = signature.parameters
    except (TypeError, ValueError):
        parameters = {}
    accepts_kwargs = any(
        parameter.kind == inspect.Parameter.VAR_KEYWORD
        for parameter in parameters.values()
    )
    kwargs: dict[str, Any] = {}
    if accepts_kwargs or "channel" in parameters:
        kwargs["channel"] = channel
    if accepts_kwargs or "sustain" in parameters:
        kwargs["sustain"] = sustain
    elif "sustain_enabled" in parameters:
        kwargs["sustain_enabled"] = sustain
    if accepts_kwargs or "min_gate_ms" in parameters:
        kwargs["min_gate_ms"] = min_gate_ms
    if accepts_kwargs or "octave_shift" in parameters:
        kwargs["octave_shift"] = octave_shift
    if accepts_kwargs or "fold_octaves" in parameters:
        kwargs["fold_octaves"] = fold_octaves
    if clock_ns is not None and (accepts_kwargs or "clock_ns" in parameters):
        kwargs["clock_ns"] = clock_ns
    try:
        return engine_factory(**kwargs)
    except TypeError as exc:
        raise BridgeError(f"cannot construct MidiNoteEngine with bridge settings: {exc}") from exc


def _pending_messages(midi_input: Any) -> Iterable[Any]:
    if getattr(midi_input, "closed", False):
        raise MidiDisconnected("MIDI input closed")
    try:
        if hasattr(midi_input, "iter_pending"):
            yield from midi_input.iter_pending()
            return
        if hasattr(midi_input, "poll"):
            while True:
                message = midi_input.poll()
                if message is None:
                    return
                yield message
        raise MidiDisconnected("MIDI input does not support non-blocking reads")
    except MidiDisconnected:
        raise
    except Exception as exc:
        raise MidiDisconnected(f"MIDI input failed: {exc}") from exc


class MidiSerialBridge:
    def __init__(
        self,
        engine: Any,
        midi_input: Any,
        transport: SerialToneTransport,
        *,
        channel: int | None = None,
        clock_ns: Callable[[], int] = time.monotonic_ns,
        sleep: Callable[[float], None] = time.sleep,
    ) -> None:
        self.engine = engine
        self.midi_input = midi_input
        self.transport = transport
        self.channel = channel
        self.clock_ns = clock_ns
        self.sleep = sleep

    def _accept_message(self, message: Any) -> bool:
        message_channel = getattr(message, "channel", None)
        return self.channel is None or message_channel is None or message_channel == self.channel

    def step(self) -> None:
        for message in _pending_messages(self.midi_input):
            now_ns = self.clock_ns()
            self.transport.poll(now_ns)
            if self._accept_message(message):
                next_hz = self.engine.handle_message(message, now_ns=now_ns)
                if next_hz is not None:
                    self.transport.set_tone(next_hz, self.clock_ns())
            self.transport.poll(self.clock_ns())
        next_hz = self.engine.poll()
        if next_hz is not None:
            self.transport.set_tone(next_hz, self.clock_ns())
        self.transport.poll(self.clock_ns())

    def panic(self) -> None:
        try:
            self.engine.panic()
        finally:
            self.transport.set_tone(0, self.clock_ns(), force=True)

    def run(self) -> None:
        while True:
            self.step()
            self.sleep(LOOP_SLEEP_SECONDS)


def _close_quietly(resource: Any) -> None:
    if resource is None:
        return
    try:
        resource.close()
    except BaseException as exc:
        LOG.debug("close failed: %s", exc)


def _best_effort_serial_silence(serial_device: Any) -> None:
    if serial_device is None:
        return
    try:
        if getattr(serial_device, "is_open", True) is False:
            return
        serial_device.write(b"tone 0\n")
        flush = getattr(serial_device, "flush", None)
        if flush is not None:
            flush()
    except BaseException as exc:
        LOG.debug("startup safety tone 0 failed: %s", exc)


def run_connected_session(
    engine: Any,
    midi_input: Any,
    transport: SerialToneTransport,
    *,
    channel: int | None,
    clock_ns: Callable[[], int] = time.monotonic_ns,
    sleep: Callable[[float], None] = time.sleep,
) -> None:
    bridge = MidiSerialBridge(
        engine,
        midi_input,
        transport,
        channel=channel,
        clock_ns=clock_ns,
        sleep=sleep,
    )
    try:
        bridge.run()
    finally:
        try:
            bridge.panic()
        except Exception as exc:
            LOG.warning("panic failed: %s", exc)
        transport.shutdown_silence(clock_ns=clock_ns, sleep=sleep)


def _format_serial_port(port: Any) -> str:
    device = _port_device(port)
    vid = getattr(port, "vid", None)
    pid = getattr(port, "pid", None)
    serial_number = getattr(port, "serial_number", None) or "-"
    description = getattr(port, "description", None) or ""
    vid_pid = (
        f"{vid:04X}:{pid:04X}"
        if isinstance(vid, int) and isinstance(pid, int)
        else "----:----"
    )
    rejected = " [REJECTED]" if device.casefold() == "com3" else ""
    return f"  {device}: VID:PID={vid_pid} S/N={serial_number} {description}{rejected}".rstrip()


def print_devices(dependencies: RuntimeDependencies) -> None:
    names = list(dependencies.mido.get_input_names())
    print("MIDI inputs:")
    if names:
        for index, name in enumerate(names):
            print(f"  [{index}] {name}")
    else:
        print("  (none)")
    print("Serial ports:")
    ports = list(dependencies.list_ports()) if dependencies.list_ports else []
    if ports:
        for port in ports:
            print(_format_serial_port(port))
    else:
        print("  (none)")


def _open_session_resources(
    args: argparse.Namespace,
    dependencies: RuntimeDependencies,
    engine_factory: Callable[..., Any],
    *,
    clock_ns: Callable[[], int],
    sleep: Callable[[float], None],
    identity_pins: DeviceIdentityPins | None = None,
) -> tuple[Any, Any, SerialToneTransport]:
    identity_pins = identity_pins or DeviceIdentityPins()
    midi_input = None
    serial_device = None
    try:
        midi_names = list(dependencies.mido.get_input_names())
        if identity_pins.midi_name is None:
            midi_name = select_midi_port(
                midi_names, args.midi_port, args.midi_index
            )
            identity_pins.midi_name = midi_name
            identity_pins.midi_index = (
                args.midi_index
                if args.midi_index is not None
                else midi_names.index(midi_name)
            )
            identity_pins.midi_names = tuple(midi_names)
        else:
            if identity_pins.midi_index is None or identity_pins.midi_names is None:
                raise DeviceSelectionError(
                    "cannot safely reconnect without the original MIDI "
                    "selection position and enumeration context"
                )
            midi_matches = [
                index
                for index, name in enumerate(midi_names)
                if name == identity_pins.midi_name
            ]
            if len(midi_matches) > 1:
                raise DeviceSelectionError(
                    f"original MIDI identity {identity_pins.midi_name!r} is duplicated"
                )
            if not midi_matches:
                raise DeviceUnavailableError(
                    f"original MIDI input {identity_pins.midi_name!r} is unavailable; "
                    "refusing a replacement"
                )
            if midi_matches[0] != identity_pins.midi_index:
                raise DeviceUnavailableError(
                    f"original MIDI input {identity_pins.midi_name!r} moved from "
                    f"index {identity_pins.midi_index} to {midi_matches[0]}; "
                    "refusing a replacement"
                )
            if tuple(midi_names) != identity_pins.midi_names:
                raise DeviceUnavailableError(
                    f"MIDI enumeration context changed around original input "
                    f"{identity_pins.midi_name!r}; refusing a replacement"
                )
            midi_name = midi_names[identity_pins.midi_index]
        LOG.info("MIDI input: %s", midi_name)
        try:
            midi_input = dependencies.mido.open_input(midi_name)
        except Exception as exc:
            raise MidiDisconnected(
                f"failed to open MIDI input {midi_name!r}: {exc}"
            ) from exc

        if args.dry_run:
            transport = SerialToneTransport(None, dry_run=True)
        else:
            if dependencies.serial_module is None or dependencies.list_ports is None:
                raise BridgeError("serial dependencies were not loaded")
            ports = list(dependencies.list_ports())
            if identity_pins.serial is None:
                serial_port = select_serial_port(
                    ports, args.serial_port, args.serial_number
                )
                selected_ports = [
                    port
                    for port in ports
                    if _port_device(port).casefold() == serial_port.casefold()
                ]
                if len(selected_ports) != 1:
                    raise DeviceSelectionError(
                        f"cannot establish exact USB identity for {serial_port!r}"
                    )
                identity_pins.serial = _serial_usb_identity(selected_ports[0])
            else:
                serial_port = _select_pinned_serial_port(
                    ports, identity_pins.serial
                )
            LOG.info("serial output: %s @ %d", serial_port, SERIAL_BAUD)
            serial_device = open_serial_device(dependencies.serial_module, serial_port)
            transport = SerialToneTransport(serial_device)
            sleep(SERIAL_STARTUP_DRAIN_SECONDS)
        transport.startup(clock_ns())
        engine = _build_engine(
            engine_factory,
            channel=args.channel,
            sustain=args.sustain,
            min_gate_ms=args.min_gate_ms,
            octave_shift=args.octave_shift,
            fold_octaves=args.fold_octaves,
            clock_ns=clock_ns,
        )
        LOG.info(
            "ready: channel=%s sustain=%s min_gate_ms=%d octave_shift=%+d mode=%s",
            "all" if args.channel is None else args.channel + 1,
            args.sustain,
            args.min_gate_ms,
            args.octave_shift,
            "fold" if args.fold_octaves else "octaves",
        )
        return engine, midi_input, transport
    except BaseException:
        _best_effort_serial_silence(serial_device)
        _close_quietly(midi_input)
        _close_quietly(serial_device)
        raise


def run_bridge(
    args: argparse.Namespace,
    dependencies: RuntimeDependencies,
    engine_factory: Callable[..., Any],
    *,
    clock_ns: Callable[[], int] = time.monotonic_ns,
    sleep: Callable[[float], None] = time.sleep,
) -> None:
    failures = 0
    identity_pins = DeviceIdentityPins()
    while True:
        midi_input = None
        serial_device = None
        transport = None
        try:
            engine, midi_input, transport = _open_session_resources(
                args,
                dependencies,
                engine_factory,
                clock_ns=clock_ns,
                sleep=sleep,
                identity_pins=identity_pins,
            )
            serial_device = transport.serial
            run_connected_session(
                engine,
                midi_input,
                transport,
                channel=args.channel,
                clock_ns=clock_ns,
                sleep=sleep,
            )
            return
        except KeyboardInterrupt:
            raise
        except Exception as exc:
            if transport is not None:
                transport.shutdown_silence(clock_ns=clock_ns, sleep=sleep)
            reconnectable = isinstance(
                exc, (DeviceUnavailableError, MidiDisconnected, TransportError)
            )
            if (
                not args.reconnect
                or not reconnectable
                or failures >= MAX_RECONNECT_ATTEMPTS
            ):
                if isinstance(exc, BridgeError):
                    raise
                raise BridgeError(str(exc)) from exc
            delay = RECONNECT_DELAYS[min(failures, len(RECONNECT_DELAYS) - 1)]
            failures += 1
            LOG.warning(
                "%s; reconnect %d/%d in %.2fs",
                exc,
                failures,
                MAX_RECONNECT_ATTEMPTS,
                delay,
            )
            sleep(delay)
        finally:
            _close_quietly(midi_input)
            _close_quietly(serial_device)


def main(
    argv: Sequence[str] | None = None,
    *,
    dependencies: RuntimeDependencies | None = None,
    engine_factory: Callable[..., Any] | None = None,
) -> int:
    args = parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(levelname)s %(message)s",
    )
    try:
        if dependencies is None:
            mido = load_midi_dependencies()
            if args.dry_run and not args.list_devices:
                dependencies = RuntimeDependencies(mido=mido)
            else:
                serial_module, list_ports = load_serial_dependencies()
                dependencies = RuntimeDependencies(mido, serial_module, list_ports)
        if args.list_devices:
            print_devices(dependencies)
            return 0
        if engine_factory is None:
            engine_factory = load_core_engine_factory()
        run_bridge(args, dependencies, engine_factory)
        return 0
    except KeyboardInterrupt:
        LOG.info("stopped")
        return 130
    except BridgeError as exc:
        LOG.error("%s", exc)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
