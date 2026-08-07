"""Hardware-free tests for mood-melody playback and CLI behavior."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import types
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


PLAYER = _load("melody_player", ROOT / "tools" / "melody_player.py")
CLI = _load("mood_melody", ROOT / "tools" / "mood_melody.py")


class FakeClock:
    def __init__(self) -> None:
        self.value = 0.0
        self.sleeps: list[float] = []

    def now(self) -> float:
        return self.value

    def sleep(self, seconds: float) -> None:
        if seconds < 0:
            raise AssertionError(f"negative sleep: {seconds}")
        self.sleeps.append(seconds)
        self.value += seconds


class RecordingTransport:
    def __init__(
        self,
        clock: FakeClock,
        *,
        fail_once_on: str | None = None,
    ) -> None:
        self.clock = clock
        self.fail_once_on = fail_once_on
        self.attempts: list[tuple[str, float]] = []

    def send(self, command: str) -> None:
        self.attempts.append((command, self.clock.now()))
        if command == self.fail_once_on:
            self.fail_once_on = None
            raise OSError(f"injected failure for {command}")


class LatencyTransport(RecordingTransport):
    def __init__(self, clock: FakeClock, latency_seconds: float) -> None:
        super().__init__(clock)
        self.latency_seconds = latency_seconds

    def send(self, command: str) -> None:
        super().send(command)
        self.clock.value += self.latency_seconds


class PlaybackTests(unittest.TestCase):
    def test_slow_writes_never_let_a_tone_lapse(self) -> None:
        # Regression: refresh slots used to be skipped when a write ran long,
        # pushing the next command onto a future grid slot. At 500 ms write
        # latency that produced a 750 ms gap, exactly the firmware cutoff, so
        # a long note died mid-sound while every test still passed.
        for latency in (0.0, 0.1, 0.3, 0.5):
            with self.subTest(latency=latency):
                clock = FakeClock()
                transport = LatencyTransport(clock, latency)
                PLAYER.play_notes(
                    (PLAYER.Note(hz=2093, gate_ms=2000, gap_ms=0),),
                    transport,
                    sleep=clock.sleep,
                    now=clock.now,
                )
                holds = [
                    when
                    for command, when in transport.attempts
                    if command != "tone 0"
                ]
                gaps = [
                    holds[i + 1] - holds[i] for i in range(len(holds) - 1)
                ]
                self.assertTrue(gaps, "a 2 s note must be refreshed")
                self.assertLess(
                    max(gaps), PLAYER.PIEZO_WATCHDOG_MS / 1000.0
                )

    def test_a_link_too_slow_to_hold_a_tone_fails_loudly(self) -> None:
        # Silently continuing would produce notes that die partway through —
        # nearly impossible to attribute to serial latency by ear.
        clock = FakeClock()
        transport = LatencyTransport(clock, 0.9)
        with self.assertRaises(PLAYER.ToneTimingError):
            PLAYER.play_notes(
                (PLAYER.Note(hz=2093, gate_ms=3000, gap_ms=0),),
                transport,
                sleep=clock.sleep,
                now=clock.now,
            )
        self.assertEqual(transport.attempts[-1][0], "tone 0")

    def test_write_timeout_cannot_outlast_the_watchdog(self) -> None:
        # A write allowed to block longer than the watchdog would guarantee a
        # truncated note no matter how the refresh is scheduled.
        self.assertLess(
            PLAYER.SERIAL_WRITE_TIMEOUT_SECONDS,
            PLAYER.PIEZO_WATCHDOG_MS / 1000.0,
        )
        self.assertLess(
            PLAYER.WATCHDOG_SAFE_SECONDS, PLAYER.PIEZO_WATCHDOG_MS / 1000.0
        )

    def play(self, notes, transport=None):
        clock = FakeClock()
        transport = transport or RecordingTransport(clock)
        commands = PLAYER.play_notes(
            notes,
            transport,
            sleep=clock.sleep,
            now=clock.now,
        )
        return commands, transport, clock

    def test_790_ms_gate_refreshes_under_the_750_ms_watchdog(self) -> None:
        commands, transport, _ = self.play(
            [PLAYER.Note(hz=1568, gate_ms=790, gap_ms=0)]
        )

        active_times = [
            timestamp
            for command, timestamp in transport.attempts
            if command == "tone 1568"
        ]
        self.assertGreaterEqual(len(active_times), 2)
        for previous, current in zip(active_times, active_times[1:]):
            self.assertLess(current - previous, 0.750)
            self.assertLessEqual(current - previous, 0.250_001)
        self.assertEqual(commands[-1], "tone 0")
        self.assertAlmostEqual(transport.attempts[-1][1], 0.790, places=6)

    def test_refresh_schedule_does_not_add_serial_write_latency(self) -> None:
        clock = FakeClock()
        transport = LatencyTransport(clock, latency_seconds=0.020)
        PLAYER.play_notes(
            [PLAYER.Note(hz=1568, gate_ms=790, gap_ms=0)],
            transport,
            sleep=clock.sleep,
            now=clock.now,
        )
        active_times = [
            timestamp
            for command, timestamp in transport.attempts
            if command == "tone 1568"
        ]
        for previous, current in zip(active_times, active_times[1:]):
            self.assertLessEqual(current - previous, 0.250_001)
        stop_time = next(
            timestamp
            for command, timestamp in reversed(transport.attempts)
            if command == "tone 0"
        )
        self.assertAlmostEqual(stop_time - 0.020, 0.790, places=6)

    def test_short_note_has_one_active_command_and_one_stop(self) -> None:
        commands, _, clock = self.play(
            [PLAYER.Note(hz=440, gate_ms=100, gap_ms=0)]
        )
        self.assertEqual(commands, ["tone 440", "tone 0"])
        self.assertAlmostEqual(clock.now(), 0.100, places=6)

    def test_every_note_is_followed_by_tone_zero(self) -> None:
        commands, _, _ = self.play(
            [
                PLAYER.Note(hz=440, gate_ms=100, gap_ms=20),
                PLAYER.Note(hz=660, gate_ms=100, gap_ms=0),
            ]
        )
        self.assertEqual(
            commands,
            ["tone 440", "tone 0", "tone 660", "tone 0"],
        )

    def test_exception_still_attempts_a_final_tone_zero(self) -> None:
        clock = FakeClock()
        transport = RecordingTransport(clock, fail_once_on="tone 660")
        with self.assertRaisesRegex(OSError, "injected failure"):
            PLAYER.play_notes(
                [
                    PLAYER.Note(hz=440, gate_ms=100, gap_ms=0),
                    PLAYER.Note(hz=660, gate_ms=100, gap_ms=0),
                ],
                transport,
                sleep=clock.sleep,
                now=clock.now,
            )
        self.assertEqual(transport.attempts[-1][0], "tone 0")

    def test_illegal_frequency_raises_before_any_active_tone(self) -> None:
        for hz in (199, 6001):
            with self.subTest(hz=hz):
                clock = FakeClock()
                transport = RecordingTransport(clock)
                with self.assertRaises(ValueError):
                    PLAYER.play_notes(
                        [PLAYER.Note(hz=hz, gate_ms=100, gap_ms=0)],
                        transport,
                        sleep=clock.sleep,
                        now=clock.now,
                    )
                self.assertEqual(transport.attempts, [("tone 0", 0.0)])

    def test_gate_below_45_ms_raises(self) -> None:
        clock = FakeClock()
        transport = RecordingTransport(clock)
        with self.assertRaises(ValueError):
            PLAYER.play_notes(
                [PLAYER.Note(hz=440, gate_ms=44, gap_ms=0)],
                transport,
                sleep=clock.sleep,
                now=clock.now,
            )
        self.assertEqual(transport.attempts, [("tone 0", 0.0)])


class PortSelectionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.ports = [
            PLAYER.PortIdentity("COM3", None),
            PLAYER.PortIdentity("COM4", "A0:F2:62:E4:C3:44"),
            PLAYER.PortIdentity("COM9", "DC:B4:D9:23:22:A0"),
        ]

    def test_selects_production_and_test_rig_by_usb_serial(self) -> None:
        self.assertEqual(PLAYER.select_port(self.ports), "COM4")
        self.assertEqual(
            PLAYER.select_port(self.ports, target="testrig"),
            "COM9",
        )

    def test_com3_is_always_rejected(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "not a robot board"):
            PLAYER.select_port(self.ports, requested="COM3")

    def test_unknown_usb_serial_is_rejected(self) -> None:
        unknown = [PLAYER.PortIdentity("COM12", "0123456789AB")]
        with self.assertRaises(RuntimeError):
            PLAYER.select_port(unknown)
        with self.assertRaises(RuntimeError):
            PLAYER.select_port(unknown, requested="COM12")

    def test_both_known_boards_are_ambiguous_without_a_target(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "found 2"):
            PLAYER.select_port(self.ports, target=None)

    def test_open_contention_names_the_midi_bridge(self) -> None:
        class BusySerial:
            def __init__(self) -> None:
                self.is_open = False

            def open(self) -> None:
                raise OSError("Access is denied")

            def close(self) -> None:
                pass

        serial_module = types.SimpleNamespace(Serial=BusySerial)
        listed = [
            types.SimpleNamespace(
                device="COM4",
                serial_number=PLAYER.PRODUCTION_SERIAL,
            )
        ]
        with self.assertRaisesRegex(
            PLAYER.SerialTransportError, "Stop the MIDI bridge"
        ):
            PLAYER.SerialToneTransport.connect(
                serial_module=serial_module,
                list_ports_provider=lambda: listed,
            )

    def test_serial_transport_configures_and_writes_the_firmware_protocol(
        self,
    ) -> None:
        class FakeSerial:
            def __init__(self) -> None:
                self.is_open = False
                self.writes: list[bytes] = []
                self.flushes = 0

            def open(self) -> None:
                self.is_open = True

            def write(self, payload: bytes) -> int:
                self.writes.append(payload)
                return len(payload)

            def flush(self) -> None:
                self.flushes += 1

            def close(self) -> None:
                self.is_open = False

        device = FakeSerial()
        delays: list[float] = []
        transport = PLAYER.SerialToneTransport.connect(
            serial_module=types.SimpleNamespace(Serial=lambda: device),
            list_ports_provider=lambda: [
                types.SimpleNamespace(
                    device="COM4",
                    serial_number=PLAYER.PRODUCTION_SERIAL,
                )
            ],
            startup_sleep=delays.append,
        )
        transport.send("tone 440")

        self.assertEqual(delays, [PLAYER.SERIAL_STARTUP_SECONDS])
        self.assertEqual(device.port, "COM4")
        self.assertEqual(device.baudrate, PLAYER.BAUD_RATE)
        self.assertFalse(device.dtr)
        self.assertFalse(device.rts)
        self.assertEqual(device.writes, [b"tone 440\n"])
        self.assertEqual(device.flushes, 1)
        transport.close()


class FakeGenerator:
    def __init__(self) -> None:
        self.calls: list[tuple[str, int | None]] = []

    def generate(self, mood: str, seed: int | None = None):
        actual_seed = 0x1234 if seed is None else seed
        self.calls.append((mood, seed))
        return types.SimpleNamespace(
            mood=mood,
            seed=actual_seed,
            notes=(
                PLAYER.Note(hz=1568, gate_ms=100, gap_ms=40),
                PLAYER.Note(hz=1760, gate_ms=120, gap_ms=0),
            ),
        )


class CliTests(unittest.TestCase):
    def test_valid_moods_are_accepted_and_invalid_mood_is_rejected(self) -> None:
        for mood in CLI.MOODS:
            with self.subTest(mood=mood):
                self.assertEqual(CLI.parse_args([mood, "--dry-run"]).mood, mood)
        with (
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit),
        ):
            CLI.parse_args(["angry", "--dry-run"])

    def test_seed_round_trips_through_cli_parsing(self) -> None:
        seed_text = "0123456789abcdef0123456789abcdef"
        args = CLI.parse_args(["sad", "--seed", "0x" + seed_text, "--dry-run"])
        self.assertEqual(CLI.format_seed(args.seed), seed_text)

    def test_dry_run_prints_notes_without_opening_a_port(self) -> None:
        generator = FakeGenerator()
        output = io.StringIO()

        def forbidden_transport(**_kwargs):
            raise AssertionError("dry-run opened a serial port")

        result = CLI.main(
            ["happy", "--seed", "1", "--dry-run"],
            generator_loader=lambda: generator,
            transport_factory=forbidden_transport,
            output=output,
            error=io.StringIO(),
        )

        self.assertEqual(result, 0)
        self.assertIn("seed=0x00000000000000000000000000000001", output.getvalue())
        self.assertIn("1568 Hz", output.getvalue())
        self.assertIn("total=260 ms", output.getvalue())

    def test_watch_does_not_load_generator_before_a_mood_event(self) -> None:
        class InterruptingTransport:
            device = "COM_TEST"

            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc, traceback):
                return False

            def readline(self) -> str:
                raise KeyboardInterrupt

        def forbidden_loader():
            raise AssertionError("watch loaded generator before a mood event")

        result = CLI.main(
            ["--watch"],
            generator_loader=forbidden_loader,
            transport_factory=lambda **_kwargs: InterruptingTransport(),
            output=io.StringIO(),
            error=io.StringIO(),
        )
        self.assertEqual(result, 130)


class WatchParsingTests(unittest.TestCase):
    IGNORED_MOODS = (
        "default",
        "bored",
        "annoyed",
        "tired",
        "angry",
        "error",
        "surprised",
        "scared",
        "wink",
        "sleeping",
    )

    def test_extracts_supported_mood_from_firmware_line(self) -> None:
        self.assertEqual(
            CLI.parse_mood_line("demo step 7/13 mood sad"),
            "sad",
        )

    def test_ignores_non_mood_lines_and_other_ten_moods(self) -> None:
        for line in ("OK", "demo step 7/13 look sad", "random output"):
            with self.subTest(line=line):
                self.assertIsNone(CLI.parse_mood_line(line))
        for index, mood in enumerate(self.IGNORED_MOODS, start=1):
            with self.subTest(mood=mood):
                self.assertIsNone(
                    CLI.parse_mood_line(
                        f"demo step {index}/13 mood {mood}"
                    )
                )

    def test_debounces_repeats_and_rapid_flicker(self) -> None:
        debouncer = CLI.MoodChangeDebouncer(debounce_seconds=0.75)
        self.assertEqual(
            debouncer.observe("demo step 7/13 mood sad", 0.0),
            "sad",
        )
        self.assertIsNone(
            debouncer.observe("demo step 7/13 mood sad", 0.1)
        )
        self.assertIsNone(
            debouncer.observe("demo step 6/13 mood happy", 0.2)
        )
        # The pending happy change may fire once it has remained current for
        # the debounce window; further repeats do not replay it.
        self.assertEqual(
            debouncer.observe("demo step 6/13 mood happy", 0.8),
            "happy",
        )
        self.assertIsNone(
            debouncer.observe("demo step 6/13 mood happy", 1.8)
        )

    def test_backlog_uses_latest_mood_and_drops_stale_supported_one(self) -> None:
        debouncer = CLI.MoodChangeDebouncer(debounce_seconds=0.0)
        latest = CLI.latest_mood_change(
            [
                "demo step 7/13 mood sad",
                "OK",
                "demo step 8/13 mood error",
            ],
            debouncer,
            now=lambda: 1.0,
        )
        self.assertIsNone(latest)
        self.assertEqual(
            CLI.latest_mood_change(
                [
                    "demo step 8/13 mood error",
                    "demo step 12/13 mood confused",
                ],
                debouncer,
                now=lambda: 2.0,
            ),
            "confused",
        )

    def test_backlog_detects_a_return_to_the_same_supported_mood(self) -> None:
        debouncer = CLI.MoodChangeDebouncer(debounce_seconds=0.0)
        self.assertEqual(
            debouncer.observe("demo step 7/13 mood sad", 0.0),
            "sad",
        )
        self.assertEqual(
            CLI.latest_mood_change(
                [
                    "demo step 8/13 mood error",
                    "demo step 7/13 mood sad",
                ],
                debouncer,
                now=lambda: 2.0,
            ),
            "sad",
        )


if __name__ == "__main__":
    unittest.main()
