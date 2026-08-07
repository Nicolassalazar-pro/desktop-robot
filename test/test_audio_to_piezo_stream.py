"""Hardware-free tests for the audio-to-piezo serial streamer."""

from __future__ import annotations

import contextlib
import io
import sys
import unittest
from unittest import mock

from tools import audio_to_piezo as piezo


class FakeTime:
    def __init__(self, *, tick_seconds: float = 0.00005) -> None:
        self.now = 0.0
        self.tick_seconds = tick_seconds
        self.sleeps: list[float] = []

    def perf_counter(self) -> float:
        value = self.now
        self.now += self.tick_seconds
        return value

    def sleep(self, seconds: float) -> None:
        if seconds < 0:
            raise AssertionError(f"negative sleep: {seconds}")
        self.sleeps.append(seconds)
        self.now += seconds


class FakeSerial:
    def __init__(
        self,
        clock: FakeTime,
        *,
        incoming: bytes = b"",
        failures: dict[bytes, BaseException] | None = None,
    ) -> None:
        self.clock = clock
        self.incoming = bytearray(incoming)
        self.failures = failures or {}
        self.writes: list[bytes] = []
        self.write_times: list[float] = []
        self.read_requests: list[int] = []
        self.port: str | None = None
        self.baudrate: int | None = None
        self.timeout: float | None = None
        self.dtr = True
        self.rts = True
        self.is_open = False
        self.closed = False
        self.flush_calls = 0

    @property
    def in_waiting(self) -> int:
        return len(self.incoming)

    def open(self) -> None:
        self.is_open = True

    def read(self, size: int) -> bytes:
        self.read_requests.append(size)
        chunk = bytes(self.incoming[:size])
        del self.incoming[:size]
        return chunk

    def write(self, payload: bytes) -> int:
        self.writes.append(payload)
        self.write_times.append(self.clock.now)
        failure = self.failures.get(payload)
        if failure is not None:
            raise failure
        return len(payload)

    def flush(self) -> None:
        self.flush_calls += 1

    def close(self) -> None:
        self.is_open = False
        self.closed = True


class FakeSerialModule:
    def __init__(self, device: FakeSerial) -> None:
        self.device = device
        self.constructor_kwargs: dict[str, object] | None = None

    def Serial(
        self,
        port=None,
        baudrate=115200,
        timeout=0,
        dtr=True,
        rts=True,
        **kwargs,
    ) -> FakeSerial:
        self.constructor_kwargs = {
            "port": port,
            "baudrate": baudrate,
            "timeout": timeout,
            "dtr": dtr,
            "rts": rts,
            **kwargs,
        }
        self.device.port = port
        self.device.baudrate = baudrate
        self.device.timeout = timeout
        self.device.dtr = dtr
        self.device.rts = rts
        return self.device


def tone_value(payload: bytes) -> int:
    command, value = payload.decode("ascii").strip().split()
    if command != "tone":
        raise AssertionError(f"unexpected serial command: {payload!r}")
    return int(value)


class AudioToPiezoStreamTests(unittest.TestCase):
    def run_stream(
        self,
        notes: list[piezo.Note],
        *,
        device: FakeSerial | None = None,
        clock: FakeTime | None = None,
    ) -> tuple[FakeSerial, FakeTime, str]:
        clock = clock or FakeTime()
        device = device or FakeSerial(clock)
        serial_module = FakeSerialModule(device)
        stderr = io.StringIO()
        with (
            mock.patch.dict(sys.modules, {"serial": serial_module}),
            mock.patch.object(piezo.time, "perf_counter", clock.perf_counter),
            mock.patch.object(piezo.time, "sleep", clock.sleep),
            contextlib.redirect_stderr(stderr),
        ):
            piezo.emit_stream(notes, "COM_TEST")
        return device, clock, stderr.getvalue()

    def test_forbidden_sub_200_hz_frequencies_are_never_commanded(self) -> None:
        notes = [
            piezo.Note(hz=1, ms=10),
            piezo.Note(hz=55, ms=10),
            piezo.Note(hz=199, ms=10),
            piezo.Note(hz=200, ms=10),
            piezo.Note(hz=440, ms=10),
        ]

        device, _, _ = self.run_stream(notes)
        values = [tone_value(payload) for payload in device.writes]

        self.assertTrue(all(value == 0 or value >= 200 for value in values), values)
        self.assertIn(200, values)
        self.assertIn(440, values)

    def test_long_active_note_is_refreshed_at_most_250_ms_apart(self) -> None:
        device, _, _ = self.run_stream([piezo.Note(hz=440, ms=1000)])
        active_times = [
            timestamp
            for payload, timestamp in zip(device.writes, device.write_times)
            if tone_value(payload) == 440
        ]

        self.assertGreaterEqual(len(active_times), 4, active_times)
        for previous, current in zip(active_times, active_times[1:]):
            self.assertLessEqual(current - previous, 0.251)
        self.assertLessEqual(active_times[0] + 1.0 - active_times[-1], 0.251)

    def test_refreshes_do_not_extend_total_target_duration(self) -> None:
        notes = [
            piezo.Note(hz=440, ms=1000),
            piezo.Note(hz=0, ms=300, name="rest"),
            piezo.Note(hz=660, ms=500),
        ]

        device, _, _ = self.run_stream(notes)

        self.assertEqual(device.writes[-1], b"tone 0\n")
        elapsed = device.write_times[-1] - device.write_times[0]
        self.assertAlmostEqual(elapsed, 1.8, delta=0.003)

    def test_rests_are_sent_as_tone_zero(self) -> None:
        notes = [
            piezo.Note(hz=0, ms=125, name="rest"),
            piezo.Note(hz=440, ms=125, name="A4"),
        ]

        device, _, _ = self.run_stream(notes)

        self.assertEqual(device.writes[0], b"tone 0\n")
        self.assertIn(b"tone 440\n", device.writes)

    def test_serial_reply_drain_obeys_byte_and_time_bounds(self) -> None:
        with self.subTest(bound="bytes"):
            clock = FakeTime()
            device = FakeSerial(clock, incoming=b"x" * 5000)
            with mock.patch.object(piezo.time, "perf_counter", clock.perf_counter):
                drained = piezo._drain_serial_input(
                    device, max_bytes=1025, max_seconds=10.0
                )
            self.assertEqual(drained, 1025)
            self.assertEqual(device.read_requests, [512, 512, 1])
            self.assertEqual(len(device.incoming), 3975)

        with self.subTest(bound="time"):
            clock = FakeTime(tick_seconds=0.01)
            device = FakeSerial(clock, incoming=b"x" * 5000)
            with mock.patch.object(piezo.time, "perf_counter", clock.perf_counter):
                drained = piezo._drain_serial_input(
                    device, max_bytes=5000, max_seconds=0.025
                )
            self.assertGreater(drained, 0)
            self.assertLess(drained, 5000)
            self.assertLessEqual(max(device.read_requests), 512)
            self.assertGreater(len(device.incoming), 0)

    def test_finally_sends_tone_zero_and_closes_after_stream_error(self) -> None:
        clock = FakeTime()
        device = FakeSerial(
            clock, failures={b"tone 440\n": OSError("active write failed")}
        )

        with self.assertRaisesRegex(OSError, "active write failed"):
            self.run_stream([piezo.Note(hz=440, ms=100)], device=device, clock=clock)

        self.assertEqual(device.writes[-1], b"tone 0\n")
        self.assertTrue(device.closed)

    def test_cleanup_error_is_reported_to_stderr_and_port_still_closes(self) -> None:
        clock = FakeTime()
        device = FakeSerial(
            clock, failures={b"tone 0\n": OSError("silence write failed")}
        )

        device, _, stderr = self.run_stream(
            [piezo.Note(hz=440, ms=100)], device=device, clock=clock
        )

        self.assertIn("cleanup tone 0 failed: silence write failed", stderr)
        self.assertTrue(device.closed)

    def test_firmware_error_reply_aborts_without_success_message(self) -> None:
        clock = FakeTime()

        class ErrorReplySerial(FakeSerial):
            def write(self, payload: bytes) -> int:
                written = super().write(payload)
                if payload == b"tone 440\n":
                    self.incoming.extend(b"OK\r\nERR tone rejected\r\n")
                return written

        device = ErrorReplySerial(clock)
        serial_module = FakeSerialModule(device)
        stderr = io.StringIO()

        with (
            mock.patch.dict(sys.modules, {"serial": serial_module}),
            mock.patch.object(piezo.time, "perf_counter", clock.perf_counter),
            mock.patch.object(piezo.time, "sleep", clock.sleep),
            contextlib.redirect_stderr(stderr),
        ):
            with self.assertRaisesRegex(
                piezo.FirmwareResponseError, "ERR tone rejected"
            ):
                piezo.emit_stream([piezo.Note(hz=440, ms=100)], "COM_TEST")

        self.assertNotIn("[stream] done", stderr.getvalue())
        self.assertEqual(device.writes[-1], b"tone 0\n")
        self.assertTrue(device.closed)


if __name__ == "__main__":
    unittest.main()
