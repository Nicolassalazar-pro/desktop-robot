from __future__ import annotations

import contextlib
import io
import subprocess
import sys
import unittest
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools import midi_serial_bridge as bridge


@dataclass
class FakePort:
    device: str
    serial_number: str | None = None
    vid: int | None = None
    pid: int | None = None
    description: str = ""
    location: str = ""
    hwid: str = ""
    manufacturer: str = ""
    product: str = ""
    interface: str = ""


class FakeSerial:
    def __init__(self, *, auto_ack: bool = False) -> None:
        self.auto_ack = auto_ack
        self.lines: list[bytes] = []
        self.writes: list[bytes] = []
        self.is_open = False
        self.closed = False

    @property
    def in_waiting(self) -> int:
        return sum(len(line) for line in self.lines)

    def open(self) -> None:
        self.is_open = True

    def close(self) -> None:
        self.is_open = False
        self.closed = True

    def write(self, value: bytes) -> None:
        self.writes.append(value)
        if self.auto_ack:
            self.lines.append(b"OK\n")

    def flush(self) -> None:
        pass

    def readline(self) -> bytes:
        return self.lines.pop(0) if self.lines else b""

    def queue(self, *lines: str) -> None:
        self.lines.extend((line + "\n").encode() for line in lines)

    def queue_raw(self, *chunks: bytes) -> None:
        self.lines.extend(chunks)


class FakeSerialModule:
    def __init__(self, device: FakeSerial) -> None:
        self.device = device

    def Serial(self) -> FakeSerial:
        return self.device


class FakeEngine:
    def __init__(self, **settings: object) -> None:
        self.settings = settings
        self.current_hz = 0
        self.held_count = 0
        self.panic_calls = 0
        self.messages: list[tuple[object, int | None]] = []
        self.poll_result: int | None = None

    def handle_message(self, message: object, now_ns: int | None = None) -> int | None:
        self.messages.append((message, now_ns))
        return getattr(message, "result", None)

    def poll(self) -> int | None:
        result = self.poll_result
        self.poll_result = None
        return result

    def panic(self) -> int | None:
        self.panic_calls += 1
        self.current_hz = 0
        return 0


class FakeMidiInput:
    def __init__(self, messages: list[object] | None = None) -> None:
        self.messages = messages or []
        self.closed = False

    def iter_pending(self):
        messages, self.messages = self.messages, []
        return iter(messages)

    def close(self) -> None:
        self.closed = True


class RaisingMidiInput(FakeMidiInput):
    def __init__(self, error: BaseException) -> None:
        super().__init__()
        self.error = error

    def iter_pending(self):
        raise self.error


class AdvancingClock:
    def __init__(self, now_ns: int = 0) -> None:
        self.now_ns = now_ns

    def __call__(self) -> int:
        return self.now_ns

    def sleep(self, seconds: float) -> None:
        self.now_ns += int(seconds * 1_000_000_000)


class TimestampSerial(FakeSerial):
    def __init__(self, clock: AdvancingClock) -> None:
        super().__init__()
        self.clock = clock
        self.write_times: list[int] = []

    def write(self, value: bytes) -> None:
        self.write_times.append(self.clock())
        super().write(value)


class DeviceSelectionTests(unittest.TestCase):
    def test_midi_prefers_literal_robot_midi_name(self) -> None:
        names = ["USB Robot MIDI 1", "Robot MIDI", "Robot MIDI Auxiliary"]
        self.assertEqual(bridge.select_midi_port(names), "Robot MIDI")

    def test_midi_uses_unique_partial_match(self) -> None:
        self.assertEqual(
            bridge.select_midi_port(["Keyboard", "USB Robot MIDI Input"]),
            "USB Robot MIDI Input",
        )

    def test_midi_rejects_ambiguous_partial_match(self) -> None:
        with self.assertRaisesRegex(bridge.DeviceSelectionError, "multiple inputs"):
            bridge.select_midi_port(["Robot MIDI A", "Robot MIDI B"])

    def test_midi_explicit_index_and_out_of_range(self) -> None:
        self.assertEqual(bridge.select_midi_port(["A", "B"], requested_index=1), "B")
        with self.assertRaisesRegex(bridge.DeviceSelectionError, "out of range"):
            bridge.select_midi_port(["A"], requested_index=2)

    def test_midi_explicit_port_prefers_exact_over_partial(self) -> None:
        names = ["Keys", "Keys Extended"]
        self.assertEqual(bridge.select_midi_port(names, requested_port="Keys"), "Keys")

    def test_missing_explicit_devices_are_reconnectable(self) -> None:
        with self.assertRaises(bridge.DeviceUnavailableError):
            bridge.select_midi_port([], requested_port="Robot MIDI")
        with self.assertRaises(bridge.DeviceUnavailableError):
            bridge.select_midi_port([], requested_index=0)
        with self.assertRaises(bridge.DeviceUnavailableError):
            bridge.select_serial_port([], explicit_port="COM8")

    def test_serial_selection_priority(self) -> None:
        ports = [
            FakePort("COM3", serial_number=bridge.DEFAULT_SERIAL_NUMBER, vid=0x303A),
            FakePort("COM4", serial_number="OTHER", vid=0x303A),
            FakePort("COM7", serial_number="A0F262E4C344", vid=0x9999),
        ]
        self.assertEqual(bridge.select_serial_port(ports, explicit_port="COM4"), "COM4")
        self.assertEqual(bridge.select_serial_port(ports), "COM7")

    def test_serial_falls_back_to_unique_espressif_vid(self) -> None:
        ports = [FakePort("COM8", serial_number="OTHER", vid=0x303A)]
        self.assertEqual(bridge.select_serial_port(ports), "COM8")

    def test_serial_rejects_com3_and_vid_ambiguity(self) -> None:
        with self.assertRaisesRegex(bridge.DeviceSelectionError, "Intel AMT"):
            bridge.select_serial_port([FakePort("COM3")], explicit_port="com3")
        ports = [FakePort("COM4", vid=0x303A), FakePort("COM5", vid=0x303A)]
        with self.assertRaisesRegex(bridge.DeviceSelectionError, "multiple VID"):
            bridge.select_serial_port(ports, serial_number="missing")

    def test_pinned_serial_rejects_duplicates_and_visible_metadata_changes(self) -> None:
        original = FakePort(
            "COM7",
            serial_number="ORIGINAL",
            vid=bridge.ESPRESSIF_VID,
            pid=1,
            location="1-2",
            hwid="USB original",
            manufacturer="Robot Co",
            product="Desktop Robot",
            interface="Tone",
        )
        identity = bridge._serial_usb_identity(original)

        with self.assertRaisesRegex(bridge.DeviceSelectionError, "duplicated"):
            bridge._select_pinned_serial_port([original, original], identity)

        changed_context = FakePort(
            "COM7",
            serial_number="ORIGINAL",
            vid=bridge.ESPRESSIF_VID,
            pid=1,
            location="1-3",
            hwid="USB replacement",
            manufacturer="Robot Co",
            product="Desktop Robot",
            interface="Tone",
        )
        with self.assertRaisesRegex(
            bridge.DeviceUnavailableError, "refusing a replacement"
        ):
            bridge._select_pinned_serial_port([changed_context], identity)


class SerialTransportTests(unittest.TestCase):
    def test_open_serial_sets_control_lines_before_open(self) -> None:
        serial_device = FakeSerial()
        opened = bridge.open_serial_device(FakeSerialModule(serial_device), "COM8")
        self.assertIs(opened, serial_device)
        self.assertEqual(opened.port, "COM8")
        self.assertEqual(opened.baudrate, 115200)
        self.assertFalse(opened.dtr)
        self.assertFalse(opened.rts)
        self.assertFalse(opened.rtscts)
        self.assertFalse(opened.dsrdtr)
        self.assertTrue(opened.is_open)

    def test_serial_construction_failures_preserve_base_exception_type(self) -> None:
        for error_type in (RuntimeError, KeyboardInterrupt, SystemExit):
            with self.subTest(error=error_type.__name__):
                error = error_type("construction failed")
                serial_module = SimpleNamespace(Serial=mock.Mock(side_effect=error))
                with self.assertRaises(BaseException) as raised:
                    bridge.open_serial_device(serial_module, "COM8")
                if error_type is RuntimeError:
                    self.assertIsInstance(raised.exception, bridge.TransportError)
                    self.assertIn("failed to open COM8", str(raised.exception))
                else:
                    self.assertIs(raised.exception, error)

    def test_all_serial_configuration_failures_silence_and_close_open_object(self) -> None:
        configuration_fields = (
            "port",
            "baudrate",
            "timeout",
            "write_timeout",
            "dtr",
            "rts",
            "rtscts",
            "dsrdtr",
        )

        class FailingConfigurationSerial(FakeSerial):
            def __init__(self, field: str, error: BaseException) -> None:
                super().__init__()
                object.__setattr__(self, "failure_field", field)
                object.__setattr__(self, "failure_error", error)
                object.__setattr__(self, "configuration_active", True)
                object.__setattr__(self, "is_open", True)

            def __setattr__(self, name: str, value: object) -> None:
                if (
                    getattr(self, "configuration_active", False)
                    and name == getattr(self, "failure_field", None)
                ):
                    raise self.failure_error
                object.__setattr__(self, name, value)

        for field in configuration_fields:
            for error_type in (RuntimeError, KeyboardInterrupt, SystemExit):
                with self.subTest(field=field, error=error_type.__name__):
                    error = error_type(f"{field} failed")
                    serial_device = FailingConfigurationSerial(field, error)
                    with self.assertRaises(BaseException) as raised:
                        bridge.open_serial_device(
                            FakeSerialModule(serial_device), "COM8"
                        )
                    if error_type is RuntimeError:
                        self.assertIsInstance(raised.exception, bridge.TransportError)
                    else:
                        self.assertIs(raised.exception, error)
                    self.assertTrue(serial_device.closed)
                    self.assertEqual(serial_device.writes, [b"tone 0\n"])

    def test_startup_drains_banner_ok_err_and_sends_silence(self) -> None:
        serial_device = FakeSerial(auto_ack=True)
        serial_device.open()
        serial_device.queue("booting", "OK", "ERR stale")
        transport = bridge.SerialToneTransport(serial_device)
        transport.startup(100)
        self.assertEqual(transport.responses[:3], ["booting", "OK", "ERR stale"])
        self.assertEqual(serial_device.writes, [b"tone 0\n"])
        self.assertIsNone(transport.in_flight_hz)

    def test_coalesces_to_newest_state_while_response_pending(self) -> None:
        serial_device = FakeSerial()
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        transport.set_tone(440, 0)
        transport.set_tone(660, 1)
        transport.set_tone(880, 2)
        self.assertEqual(serial_device.writes, [b"tone 440\n"])
        serial_device.queue("OK")
        transport.poll(3)
        self.assertEqual(serial_device.writes, [b"tone 440\n", b"tone 880\n"])
        self.assertEqual(transport.in_flight_hz, 880)

    def test_persistent_err_is_terminal_and_non_reconnectable(self) -> None:
        class RejectingSerial(FakeSerial):
            def write(self, value: bytes) -> None:
                super().write(value)
                self.queue("ERR rejected")

        serial_device = RejectingSerial()
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        message = SimpleNamespace(channel=0, result=440)
        midi_input = FakeMidiInput([message])
        sleeps: list[float] = []

        with mock.patch.object(
            bridge,
            "_open_session_resources",
            return_value=(FakeEngine(), midi_input, transport),
        ) as open_resources:
            with self.assertRaisesRegex(
                bridge.FirmwareRejectedError, "robot rejected tone 440"
            ):
                bridge.run_bridge(
                    bridge.parse_args(["--reconnect", "--dry-run"]),
                    bridge.RuntimeDependencies(mido=SimpleNamespace()),
                    FakeEngine,
                    clock_ns=lambda: 0,
                    sleep=sleeps.append,
                )

        open_resources.assert_called_once()
        self.assertEqual(sleeps, [])
        self.assertEqual(serial_device.writes.count(b"tone 440\n"), 1)
        self.assertEqual(serial_device.writes, [b"tone 440\n", b"tone 0\n"])
        self.assertLessEqual(len(transport.responses), bridge.MAX_RESPONSE_HISTORY)

    def test_fragmented_ack_is_buffered_until_newline(self) -> None:
        serial_device = FakeSerial()
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        transport.set_tone(440, 0)
        serial_device.queue_raw(b"O")
        transport.poll(1)
        self.assertEqual(transport.in_flight_hz, 440)
        serial_device.queue_raw(b"K\r", b"\n")
        transport.poll(2)
        self.assertIsNone(transport.in_flight_hz)
        self.assertEqual(transport.last_completed_hz, 440)

    def test_active_tone_heartbeat_resends_every_250ms(self) -> None:
        serial_device = FakeSerial()
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        transport.set_tone(440, 0)
        serial_device.queue("OK")
        transport.poll(1)
        transport.poll(bridge.HEARTBEAT_NS - 1)
        self.assertEqual(len(serial_device.writes), 1)
        transport.poll(bridge.HEARTBEAT_NS)
        self.assertEqual(serial_device.writes, [b"tone 440\n", b"tone 440\n"])

    def test_silence_has_no_heartbeat(self) -> None:
        serial_device = FakeSerial()
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        transport.set_tone(0, 0)
        serial_device.queue("OK")
        transport.poll(1)
        transport.poll(bridge.HEARTBEAT_NS * 2)
        self.assertEqual(serial_device.writes, [b"tone 0\n"])

    def test_lost_ack_renews_active_tone_before_firmware_watchdog(self) -> None:
        serial_device = FakeSerial()
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        transport.set_tone(440, 0)
        self.assertLess(bridge.RESPONSE_TIMEOUT_NS, bridge.FIRMWARE_WATCHDOG_NS)
        transport.poll(bridge.RESPONSE_TIMEOUT_NS - 1)
        self.assertEqual(serial_device.writes, [b"tone 440\n"])
        transport.poll(bridge.RESPONSE_TIMEOUT_NS)
        self.assertEqual(serial_device.writes, [b"tone 440\n", b"tone 440\n"])
        self.assertEqual(transport.in_flight_since_ns, bridge.RESPONSE_TIMEOUT_NS)

    def test_lost_ack_recovery_sends_only_newest_coalesced_state(self) -> None:
        serial_device = FakeSerial()
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        transport.set_tone(440, 0)
        transport.set_tone(660, 1)
        transport.set_tone(880, 2)
        transport.poll(bridge.RESPONSE_TIMEOUT_NS)
        self.assertEqual(serial_device.writes, [b"tone 440\n", b"tone 880\n"])
        self.assertEqual(transport.in_flight_hz, 880)
        serial_device.queue("OK")
        transport.poll(bridge.RESPONSE_TIMEOUT_NS + 1)
        self.assertIsNone(transport.last_completed_hz)
        self.assertEqual(transport.in_flight_hz, 880)
        transport.set_tone(990, bridge.RESPONSE_TIMEOUT_NS + 2)
        transport.poll(bridge.RESPONSE_TIMEOUT_NS * 2)
        self.assertEqual(
            serial_device.writes,
            [b"tone 440\n", b"tone 880\n", b"tone 990\n"],
        )
        self.assertEqual(transport.in_flight_hz, 990)

    def test_lost_heartbeat_ack_recovers_with_watchdog_margin(self) -> None:
        serial_device = FakeSerial()
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        transport.set_tone(440, 0)
        serial_device.queue("OK")
        transport.poll(1)
        transport.poll(bridge.HEARTBEAT_NS)
        recovery_ns = bridge.HEARTBEAT_NS + bridge.RESPONSE_TIMEOUT_NS
        self.assertLess(recovery_ns, bridge.FIRMWARE_WATCHDOG_NS)
        transport.poll(recovery_ns - 1)
        self.assertEqual(serial_device.writes, [b"tone 440\n", b"tone 440\n"])
        transport.poll(recovery_ns)
        self.assertEqual(
            serial_device.writes,
            [b"tone 440\n", b"tone 440\n", b"tone 440\n"],
        )

    def test_response_diagnostics_are_bounded(self) -> None:
        serial_device = FakeSerial()
        serial_device.open()
        serial_device.queue(*(f"line {index}" for index in range(100)))
        transport = bridge.SerialToneTransport(serial_device)
        transport.drain_responses()
        self.assertEqual(len(transport.responses), bridge.MAX_RESPONSE_HISTORY)
        self.assertEqual(transport.responses[0], "line 36")

    def test_shutdown_sends_tone_zero(self) -> None:
        serial_device = FakeSerial(auto_ack=True)
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        transport.set_tone(440, 0)
        transport.poll(1)
        clock = AdvancingClock(2)
        transport.shutdown_silence(clock_ns=clock, sleep=clock.sleep)
        self.assertEqual(serial_device.writes[-1], b"tone 0\n")


class LifecycleAndCliTests(unittest.TestCase):
    def test_bridge_delegates_messages_and_poll_to_core(self) -> None:
        engine = FakeEngine()
        engine.poll_result = 660
        message = SimpleNamespace(channel=0, result=440)
        midi_input = FakeMidiInput([message])
        serial_device = FakeSerial(auto_ack=True)
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        runner = bridge.MidiSerialBridge(
            engine, midi_input, transport, channel=0, clock_ns=lambda: 123
        )
        runner.step()
        self.assertEqual(engine.messages, [(message, 123)])
        self.assertEqual(serial_device.writes, [b"tone 440\n", b"tone 660\n"])

    def test_large_slow_midi_batch_cannot_starve_active_tone_recovery(self) -> None:
        clock = AdvancingClock()
        serial_device = TimestampSerial(clock)
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        transport.set_tone(440, clock())

        class SlowEngine(FakeEngine):
            def handle_message(
                self, message: object, now_ns: int | None = None
            ) -> int | None:
                super().handle_message(message, now_ns=now_ns)
                clock.sleep(0.1)
                return getattr(message, "result", None)

        messages = [
            SimpleNamespace(channel=0, result=500 + index)
            for index in range(20)
        ]
        engine = SlowEngine()
        runner = bridge.MidiSerialBridge(
            engine,
            FakeMidiInput(messages),
            transport,
            channel=0,
            clock_ns=clock,
        )

        runner.step()

        self.assertGreater(clock(), bridge.FIRMWARE_WATCHDOG_NS)
        self.assertEqual(serial_device.writes[1], b"tone 501\n")
        self.assertLess(serial_device.write_times[1], bridge.FIRMWARE_WATCHDOG_NS)
        self.assertTrue(
            all(
                later - earlier <= bridge.RESPONSE_TIMEOUT_NS
                for earlier, later in zip(
                    serial_device.write_times, serial_device.write_times[1:]
                )
            )
        )
        self.assertEqual(
            [now_ns for _, now_ns in engine.messages],
            [index * 100_000_000 for index in range(20)],
        )

    def test_channel_filter_does_not_reimplement_note_semantics(self) -> None:
        engine = FakeEngine()
        messages = [
            SimpleNamespace(channel=0, result=440),
            SimpleNamespace(channel=1, result=660),
        ]
        midi_input = FakeMidiInput(messages)
        transport = bridge.SerialToneTransport(None, dry_run=True)
        runner = bridge.MidiSerialBridge(
            engine, midi_input, transport, channel=1, clock_ns=lambda: 5
        )
        runner.step()
        self.assertEqual([entry[0] for entry in engine.messages], [messages[1]])

    def test_midi_loss_panics_and_sends_safety_tone(self) -> None:
        engine = FakeEngine()
        midi_input = FakeMidiInput()
        midi_input.closed = True
        serial_device = FakeSerial(auto_ack=True)
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        clock = AdvancingClock()
        with self.assertRaises(bridge.MidiDisconnected):
            bridge.run_connected_session(
                engine,
                midi_input,
                transport,
                channel=None,
                clock_ns=clock,
                sleep=clock.sleep,
            )
        self.assertEqual(engine.panic_calls, 1)
        self.assertIn(b"tone 0\n", serial_device.writes)

    def test_ctrl_c_panics_and_sends_safety_tone(self) -> None:
        engine = FakeEngine()
        midi_input = RaisingMidiInput(KeyboardInterrupt())
        serial_device = FakeSerial(auto_ack=True)
        serial_device.open()
        transport = bridge.SerialToneTransport(serial_device)
        clock = AdvancingClock()
        with self.assertRaises(KeyboardInterrupt):
            bridge.run_connected_session(
                engine,
                midi_input,
                transport,
                channel=None,
                clock_ns=clock,
                sleep=clock.sleep,
            )
        self.assertEqual(engine.panic_calls, 1)
        self.assertEqual(serial_device.writes[-1], b"tone 0\n")

    def test_engine_settings_are_injected_without_midi_semantics(self) -> None:
        captured: dict[str, object] = {}

        def factory(channel=None, sustain=True, min_gate_ms=45):
            captured.update(
                channel=channel, sustain=sustain, min_gate_ms=min_gate_ms
            )
            return FakeEngine()

        bridge._build_engine(factory, channel=15, sustain=False, min_gate_ms=60)
        self.assertEqual(
            captured, {"channel": 15, "sustain": False, "min_gate_ms": 60}
        )

    def test_locked_core_sustain_enabled_constructor_is_supported(self) -> None:
        from tools.midi_bridge_core import MidiNoteEngine

        clock = AdvancingClock()
        engine = bridge._build_engine(
            MidiNoteEngine,
            channel=None,
            sustain=False,
            min_gate_ms=45,
            clock_ns=clock,
        )
        note_on = SimpleNamespace(type="note_on", note=60, velocity=100, channel=0)
        sustain = SimpleNamespace(type="control_change", control=64, value=127, channel=0)
        note_off = SimpleNamespace(type="note_off", note=60, velocity=0, channel=0)
        expected = MidiNoteEngine(min_gate_ms=0).note_hz(60)
        self.assertEqual(engine.handle_message(note_on, now_ns=0), expected)
        self.assertIsNone(engine.handle_message(sustain, now_ns=1))
        self.assertIsNone(engine.handle_message(note_off, now_ns=2))
        self.assertEqual(engine.current_hz, expected)
        self.assertEqual(engine.held_count, 0)
        clock.sleep(0.045)
        self.assertEqual(engine.poll(), 0)

    def test_cli_validation(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                bridge.parse_args(["--min-gate-ms", "44"])
            with self.assertRaises(SystemExit):
                bridge.parse_args(["--channel", "17"])
            with self.assertRaises(SystemExit):
                bridge.parse_args(["--midi-index", "-1"])
            with self.assertRaises(SystemExit):
                bridge.parse_args(["--midi-port", "A", "--midi-index", "0"])
        args = bridge.parse_args(["--channel", "16", "--no-sustain", "--min-gate-ms", "60"])
        self.assertEqual(args.channel, 15)
        self.assertFalse(args.sustain)

    def test_help_runs_without_optional_packages_or_core(self) -> None:
        script = Path(bridge.__file__)
        result = subprocess.run(
            [sys.executable, str(script), "--help"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--list-devices", result.stdout)
        self.assertIn("--no-sustain", result.stdout)

    def test_list_devices_does_not_load_core(self) -> None:
        fake_mido = SimpleNamespace(get_input_names=lambda: ["Robot MIDI"])
        dependencies = bridge.RuntimeDependencies(
            mido=fake_mido,
            serial_module=object(),
            list_ports=lambda: [FakePort("COM4", vid=0x303A, pid=1)],
        )
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = bridge.main(
                ["--list-devices"],
                dependencies=dependencies,
                engine_factory=lambda: self.fail("core should not be loaded"),
            )
        self.assertEqual(result, 0)
        self.assertIn("[0] Robot MIDI", output.getvalue())
        self.assertIn("COM4", output.getvalue())

    def test_optional_dependencies_are_imported_only_by_loaders(self) -> None:
        with mock.patch.object(bridge.importlib, "import_module") as importer:
            bridge.parse_args(["--dry-run"])
        importer.assert_not_called()

    def test_dry_run_does_not_load_serial_dependency(self) -> None:
        fake_mido = SimpleNamespace()
        with (
            mock.patch.object(bridge, "load_midi_dependencies", return_value=fake_mido),
            mock.patch.object(
                bridge,
                "load_serial_dependencies",
                side_effect=AssertionError("serial must stay lazy"),
            ),
            mock.patch.object(bridge, "load_core_engine_factory", return_value=FakeEngine),
            mock.patch.object(bridge, "run_bridge") as run_bridge,
        ):
            self.assertEqual(bridge.main(["--dry-run"]), 0)
        dependencies = run_bridge.call_args.args[1]
        self.assertIs(dependencies.mido, fake_mido)
        self.assertIsNone(dependencies.serial_module)
        self.assertIsNone(dependencies.list_ports)

    def test_dry_run_ctrl_c_closes_midi_without_serial_hardware(self) -> None:
        midi_input = RaisingMidiInput(KeyboardInterrupt())
        fake_mido = SimpleNamespace(
            get_input_names=lambda: ["Robot MIDI"],
            open_input=lambda name: midi_input,
        )

        result = bridge.main(
            ["--dry-run"],
            dependencies=bridge.RuntimeDependencies(mido=fake_mido),
            engine_factory=FakeEngine,
        )

        self.assertEqual(result, 130)
        self.assertTrue(midi_input.closed)

    def test_ctrl_c_closes_midi_and_serial_resources(self) -> None:
        midi_input = RaisingMidiInput(KeyboardInterrupt())
        serial_device = FakeSerial(auto_ack=True)
        dependencies = bridge.RuntimeDependencies(
            mido=SimpleNamespace(
                get_input_names=lambda: ["Robot MIDI"],
                open_input=lambda name: midi_input,
            ),
            serial_module=FakeSerialModule(serial_device),
            list_ports=lambda: [
                FakePort("COM7", serial_number=bridge.DEFAULT_SERIAL_NUMBER)
            ],
        )

        result = bridge.main([], dependencies=dependencies, engine_factory=FakeEngine)

        self.assertEqual(result, 130)
        self.assertTrue(midi_input.closed)
        self.assertTrue(serial_device.closed)
        self.assertEqual(serial_device.writes[-1], b"tone 0\n")

    def test_reconnect_requires_original_midi_identity_not_replacement_at_index(self) -> None:
        args = bridge.parse_args(["--dry-run", "--midi-index", "0"])
        names = ["Original MIDI"]
        opened_inputs: list[FakeMidiInput] = []

        def open_input(name: str) -> FakeMidiInput:
            midi_input = FakeMidiInput()
            opened_inputs.append(midi_input)
            return midi_input

        dependencies = bridge.RuntimeDependencies(
            mido=SimpleNamespace(
                get_input_names=lambda: list(names),
                open_input=open_input,
            )
        )
        identity_pins = bridge.DeviceIdentityPins()
        _, midi_input, _ = bridge._open_session_resources(
            args,
            dependencies,
            FakeEngine,
            clock_ns=lambda: 0,
            sleep=lambda seconds: None,
            identity_pins=identity_pins,
        )
        midi_input.close()
        names[:] = ["Replacement MIDI"]

        with self.assertRaisesRegex(
            bridge.DeviceUnavailableError, "refusing a replacement"
        ):
            bridge._open_session_resources(
                args,
                dependencies,
                FakeEngine,
                clock_ns=lambda: 0,
                sleep=lambda seconds: None,
                identity_pins=identity_pins,
            )

        self.assertEqual(identity_pins.midi_name, "Original MIDI")
        self.assertEqual(len(opened_inputs), 1)

    def test_reconnect_rejects_midi_reordering_and_context_replacement(self) -> None:
        cases = (
            (
                ["Other MIDI", "Original MIDI"],
                ["Original MIDI", "Other MIDI"],
                1,
                "moved from index",
            ),
            (
                ["Original MIDI", "Original Context"],
                ["Original MIDI", "Replacement Context"],
                0,
                "enumeration context changed",
            ),
            (
                ["Original MIDI", "Other MIDI"],
                ["Original MIDI", "Original MIDI"],
                0,
                "duplicated",
            ),
        )

        for original_names, replacement_names, selected_index, error_text in cases:
            with self.subTest(error=error_text):
                names = list(original_names)
                opened_inputs: list[FakeMidiInput] = []

                def open_input(name: str) -> FakeMidiInput:
                    midi_input = FakeMidiInput()
                    opened_inputs.append(midi_input)
                    return midi_input

                dependencies = bridge.RuntimeDependencies(
                    mido=SimpleNamespace(
                        get_input_names=lambda: list(names),
                        open_input=open_input,
                    )
                )
                pins = bridge.DeviceIdentityPins()
                args = bridge.parse_args(
                    ["--dry-run", "--midi-index", str(selected_index)]
                )
                _, midi_input, _ = bridge._open_session_resources(
                    args,
                    dependencies,
                    FakeEngine,
                    clock_ns=lambda: 0,
                    sleep=lambda seconds: None,
                    identity_pins=pins,
                )
                midi_input.close()
                names[:] = replacement_names

                with self.assertRaisesRegex(
                    bridge.DeviceSelectionError, error_text
                ):
                    bridge._open_session_resources(
                        args,
                        dependencies,
                        FakeEngine,
                        clock_ns=lambda: 0,
                        sleep=lambda seconds: None,
                        identity_pins=pins,
                    )

                self.assertEqual(len(opened_inputs), 1)

    def test_reconnect_requires_original_serial_usb_identity_not_same_port(self) -> None:
        args = bridge.parse_args([])
        ports = [
            FakePort(
                "COM7",
                serial_number="ORIGINAL",
                vid=bridge.ESPRESSIF_VID,
                pid=1,
            )
        ]
        opened_inputs: list[FakeMidiInput] = []
        serial_device = FakeSerial(auto_ack=True)

        def open_input(name: str) -> FakeMidiInput:
            midi_input = FakeMidiInput()
            opened_inputs.append(midi_input)
            return midi_input

        dependencies = bridge.RuntimeDependencies(
            mido=SimpleNamespace(
                get_input_names=lambda: ["Robot MIDI"],
                open_input=open_input,
            ),
            serial_module=FakeSerialModule(serial_device),
            list_ports=lambda: list(ports),
        )
        identity_pins = bridge.DeviceIdentityPins()
        _, midi_input, transport = bridge._open_session_resources(
            args,
            dependencies,
            FakeEngine,
            clock_ns=lambda: 0,
            sleep=lambda seconds: None,
            identity_pins=identity_pins,
        )
        midi_input.close()
        transport.serial.close()
        ports[:] = [
            FakePort(
                "COM7",
                serial_number="REPLACEMENT",
                vid=bridge.ESPRESSIF_VID,
                pid=1,
            )
        ]

        with self.assertRaisesRegex(
            bridge.DeviceUnavailableError, "refusing a replacement"
        ):
            bridge._open_session_resources(
                args,
                dependencies,
                FakeEngine,
                clock_ns=lambda: 0,
                sleep=lambda seconds: None,
                identity_pins=identity_pins,
            )

        self.assertEqual(identity_pins.serial.serial_number, "ORIGINAL")
        self.assertEqual(len(opened_inputs), 2)
        self.assertTrue(opened_inputs[-1].closed)

        ports[:] = [
            FakePort(
                "COM8",
                serial_number="ORIGINAL",
                vid=bridge.ESPRESSIF_VID,
                pid=1,
            )
        ]
        with self.assertRaisesRegex(
            bridge.DeviceUnavailableError, "refusing a replacement"
        ):
            bridge._open_session_resources(
                args,
                dependencies,
                FakeEngine,
                clock_ns=lambda: 0,
                sleep=lambda seconds: None,
                identity_pins=identity_pins,
            )
        self.assertEqual(len(opened_inputs), 3)
        self.assertTrue(opened_inputs[-1].closed)

    def test_reconnect_refuses_serial_identity_without_usb_serial_number(self) -> None:
        args = bridge.parse_args([])
        midi_inputs: list[FakeMidiInput] = []
        serial_devices: list[FakeSerial] = []

        def open_input(name: str) -> FakeMidiInput:
            midi_input = FakeMidiInput()
            midi_inputs.append(midi_input)
            return midi_input

        class SerialFactory:
            def Serial(self) -> FakeSerial:
                serial_device = FakeSerial(auto_ack=True)
                serial_devices.append(serial_device)
                return serial_device

        dependencies = bridge.RuntimeDependencies(
            mido=SimpleNamespace(
                get_input_names=lambda: ["Robot MIDI"],
                open_input=open_input,
            ),
            serial_module=SerialFactory(),
            list_ports=lambda: [
                FakePort("COM7", serial_number=None, vid=bridge.ESPRESSIF_VID, pid=1)
            ],
        )
        pins = bridge.DeviceIdentityPins()
        _, midi_input, transport = bridge._open_session_resources(
            args,
            dependencies,
            FakeEngine,
            clock_ns=lambda: 0,
            sleep=lambda seconds: None,
            identity_pins=pins,
        )
        midi_input.close()
        transport.serial.close()

        with self.assertRaisesRegex(
            bridge.DeviceSelectionError, "non-empty USB serial number"
        ):
            bridge._open_session_resources(
                args,
                dependencies,
                FakeEngine,
                clock_ns=lambda: 0,
                sleep=lambda seconds: None,
                identity_pins=pins,
            )

        self.assertEqual(len(serial_devices), 1)
        self.assertEqual(len(midi_inputs), 2)
        self.assertTrue(midi_inputs[-1].closed)

    def test_all_startup_failure_phases_cleanup_acquired_resources(self) -> None:
        phases = (
            "midi-enumeration",
            "midi-selection",
            "midi-open",
            "serial-enumeration",
            "serial-selection",
            "serial-open",
            "startup-drain",
            "transport-creation",
            "startup-silence",
            "engine-build",
        )
        error_types = (RuntimeError, KeyboardInterrupt, SystemExit)

        for phase in phases:
            for error_type in error_types:
                with self.subTest(phase=phase, error=error_type.__name__):
                    error = error_type(f"{phase} failed")
                    midi_input = FakeMidiInput()
                    serial_device = FakeSerial(auto_ack=True)

                    if phase == "serial-open":
                        def interrupted_open() -> None:
                            serial_device.is_open = True
                            raise error

                        serial_device.open = interrupted_open

                    def list_ports():
                        if phase == "serial-enumeration":
                            raise error
                        return [
                            FakePort(
                                "COM7",
                                serial_number=bridge.DEFAULT_SERIAL_NUMBER,
                                vid=bridge.ESPRESSIF_VID,
                                pid=1,
                            )
                        ]

                    def startup_sleep(seconds: float) -> None:
                        if phase == "startup-drain":
                            raise error

                    def engine_factory(**settings: object) -> FakeEngine:
                        if phase == "engine-build":
                            raise error
                        return FakeEngine(**settings)

                    def get_input_names():
                        if phase == "midi-enumeration":
                            raise error
                        return ["Robot MIDI"]

                    def open_input(name: str) -> FakeMidiInput:
                        if phase == "midi-open":
                            raise error
                        return midi_input

                    dependencies = bridge.RuntimeDependencies(
                        mido=SimpleNamespace(
                            get_input_names=get_input_names,
                            open_input=open_input,
                        ),
                        serial_module=FakeSerialModule(serial_device),
                        list_ports=list_ports,
                    )

                    with contextlib.ExitStack() as stack:
                        if phase == "midi-selection":
                            stack.enter_context(
                                mock.patch.object(
                                    bridge, "select_midi_port", side_effect=error
                                )
                            )
                        if phase == "serial-selection":
                            stack.enter_context(
                                mock.patch.object(
                                    bridge, "select_serial_port", side_effect=error
                                )
                            )
                        if phase == "transport-creation":
                            stack.enter_context(
                                mock.patch.object(
                                    bridge, "SerialToneTransport", side_effect=error
                                )
                            )
                        if phase == "startup-silence":
                            stack.enter_context(
                                mock.patch.object(
                                    bridge.SerialToneTransport,
                                    "startup",
                                    side_effect=error,
                                )
                            )

                        with self.assertRaises(BaseException) as raised:
                            bridge._open_session_resources(
                                bridge.parse_args([]),
                                dependencies,
                                engine_factory,
                                clock_ns=lambda: 0,
                                sleep=startup_sleep,
                            )

                    if error_type in (KeyboardInterrupt, SystemExit):
                        self.assertIs(raised.exception, error)
                    midi_was_opened = phase not in {
                        "midi-enumeration",
                        "midi-selection",
                        "midi-open",
                    }
                    self.assertEqual(midi_input.closed, midi_was_opened)
                    serial_was_opened = phase in {
                        "serial-open",
                        "startup-drain",
                        "transport-creation",
                        "startup-silence",
                        "engine-build",
                    }
                    if serial_was_opened:
                        self.assertTrue(serial_device.closed)
                        self.assertIn(b"tone 0\n", serial_device.writes)
                    else:
                        self.assertFalse(serial_device.closed)
                        self.assertEqual(serial_device.writes, [])

    def test_startup_serial_acquisition_failures_cleanup_midi_and_serial(self) -> None:
        class FailingConfigurationSerial(FakeSerial):
            def __init__(self, error: BaseException) -> None:
                super().__init__()
                self.error = error
                self.is_open = True

            def __setattr__(self, name: str, value: object) -> None:
                if name == "baudrate" and hasattr(self, "error"):
                    raise self.error
                object.__setattr__(self, name, value)

        for phase in ("construction", "configuration"):
            for error_type in (RuntimeError, KeyboardInterrupt, SystemExit):
                with self.subTest(phase=phase, error=error_type.__name__):
                    error = error_type(f"{phase} failed")
                    midi_input = FakeMidiInput()
                    serial_device = (
                        None
                        if phase == "construction"
                        else FailingConfigurationSerial(error)
                    )

                    def acquire_serial() -> FakeSerial:
                        if serial_device is None:
                            raise error
                        return serial_device

                    dependencies = bridge.RuntimeDependencies(
                        mido=SimpleNamespace(
                            get_input_names=lambda: ["Robot MIDI"],
                            open_input=lambda name: midi_input,
                        ),
                        serial_module=SimpleNamespace(Serial=acquire_serial),
                        list_ports=lambda: [
                            FakePort(
                                "COM7",
                                serial_number=bridge.DEFAULT_SERIAL_NUMBER,
                                vid=bridge.ESPRESSIF_VID,
                                pid=1,
                            )
                        ],
                    )

                    with self.assertRaises(BaseException) as raised:
                        bridge._open_session_resources(
                            bridge.parse_args([]),
                            dependencies,
                            FakeEngine,
                            clock_ns=lambda: 0,
                            sleep=lambda seconds: None,
                        )

                    if error_type is RuntimeError:
                        self.assertIsInstance(raised.exception, bridge.TransportError)
                    else:
                        self.assertIs(raised.exception, error)
                    self.assertTrue(midi_input.closed)
                    if serial_device is not None:
                        self.assertTrue(serial_device.closed)
                        self.assertEqual(serial_device.writes, [b"tone 0\n"])

    def test_reconnect_attempts_are_bounded(self) -> None:
        args = bridge.parse_args(["--reconnect", "--dry-run"])
        dependencies = bridge.RuntimeDependencies(mido=SimpleNamespace())
        sleeps: list[float] = []
        with mock.patch.object(
            bridge,
            "_open_session_resources",
            side_effect=bridge.MidiDisconnected("gone"),
        ) as open_resources:
            with self.assertRaisesRegex(bridge.MidiDisconnected, "gone"):
                bridge.run_bridge(
                    args,
                    dependencies,
                    FakeEngine,
                    clock_ns=lambda: 0,
                    sleep=sleeps.append,
                )
        self.assertEqual(open_resources.call_count, bridge.MAX_RECONNECT_ATTEMPTS + 1)
        self.assertEqual(sleeps, list(bridge.RECONNECT_DELAYS))

    def test_reconnect_recovers_after_transient_disconnect(self) -> None:
        args = bridge.parse_args(["--reconnect", "--dry-run"])
        dependencies = bridge.RuntimeDependencies(mido=SimpleNamespace())
        midi_input = FakeMidiInput()
        transport = bridge.SerialToneTransport(None, dry_run=True)
        sleeps: list[float] = []
        with (
            mock.patch.object(
                bridge,
                "_open_session_resources",
                side_effect=[
                    bridge.MidiDisconnected("gone"),
                    (FakeEngine(), midi_input, transport),
                ],
            ) as open_resources,
            mock.patch.object(bridge, "run_connected_session") as connected_session,
        ):
            bridge.run_bridge(
                args,
                dependencies,
                FakeEngine,
                clock_ns=lambda: 0,
                sleep=sleeps.append,
            )

        self.assertEqual(open_resources.call_count, 2)
        connected_session.assert_called_once()
        self.assertEqual(sleeps, [bridge.RECONNECT_DELAYS[0]])
        self.assertTrue(midi_input.closed)

    def test_reconnect_does_not_retry_ambiguous_configuration(self) -> None:
        args = bridge.parse_args(["--reconnect", "--dry-run"])
        dependencies = bridge.RuntimeDependencies(mido=SimpleNamespace())
        sleeps: list[float] = []
        with mock.patch.object(
            bridge,
            "_open_session_resources",
            side_effect=bridge.DeviceSelectionError("ambiguous"),
        ) as open_resources:
            with self.assertRaisesRegex(bridge.DeviceSelectionError, "ambiguous"):
                bridge.run_bridge(
                    args,
                    dependencies,
                    FakeEngine,
                    clock_ns=lambda: 0,
                    sleep=sleeps.append,
                )
        self.assertEqual(open_resources.call_count, 1)
        self.assertEqual(sleeps, [])


if __name__ == "__main__":
    unittest.main()
