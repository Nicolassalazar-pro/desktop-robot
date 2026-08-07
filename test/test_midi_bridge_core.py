"""Tests for the pure MIDI-to-frequency state engine."""

from __future__ import annotations

import ast
import inspect
import unittest
from types import SimpleNamespace

from tools import midi_bridge_core
from tools.midi_bridge_core import MidiNoteEngine


FOLDED_HZ = [
    1760,
    1865,
    1976,
    2093,
    2217,
    2349,
    2489,
    2637,
    2794,
    2960,
    3136,
    3322,
]


class FakeClock:
    def __init__(self, now_ns: int = 0) -> None:
        self.now_ns = now_ns

    def __call__(self) -> int:
        return self.now_ns

    def advance_ms(self, milliseconds: float) -> None:
        self.now_ns += int(milliseconds * 1_000_000)


def message(message_type: str, **fields: int) -> SimpleNamespace:
    return SimpleNamespace(type=message_type, channel=fields.pop("channel", 0), **fields)


def note_on(note: int, velocity: int = 100, channel: int = 0) -> SimpleNamespace:
    return message("note_on", note=note, velocity=velocity, channel=channel)


def note_off(note: int, channel: int = 0) -> SimpleNamespace:
    return message("note_off", note=note, velocity=0, channel=channel)


def cc(control: int, value: int, channel: int = 0) -> SimpleNamespace:
    return message("control_change", control=control, value=value, channel=channel)


def expected_hz(note: int) -> int:
    """Frequency the default engine emits: octave-preserving, range-clamped."""
    return midi_bridge_core.playable_hz(note)


def hz(note: int) -> int:
    return expected_hz(note)


def folded_hz(note: int) -> int:
    """Legacy single-octave mapping, retained behind --fold-octaves."""
    folded_note = 93 + ((note - 93) % 12)
    return round(440.0 * (2.0 ** ((folded_note - 69) / 12.0)))


class MidiNoteEngineTests(unittest.TestCase):
    def test_default_mapping_preserves_octaves(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0)

        # C4/C5/C6/C7 must all differ, and each step up an octave must double.
        previous = None
        for note in (60, 72, 84, 96):
            emitted = engine.handle_message(note_on(note))
            self.assertEqual(engine.handle_message(note_off(note)), 0)
            if previous is not None:
                self.assertAlmostEqual(emitted / previous, 2.0, places=2)
            previous = emitted

    def test_every_note_lands_inside_the_firmware_tone_range(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0)

        for note in range(128):
            with self.subTest(note=note):
                emitted = engine.handle_message(note_on(note))
                self.assertIsNotNone(emitted)
                self.assertGreaterEqual(emitted, midi_bridge_core.MIN_TONE_HZ)
                self.assertLessEqual(emitted, midi_bridge_core.MAX_TONE_HZ)
                self.assertEqual(engine.handle_message(note_off(note)), 0)

    def test_out_of_range_notes_keep_their_pitch_class(self) -> None:
        # A0 is far below the piezo floor, so it transposes up by whole
        # octaves and must still sound as an A.
        for note in (21, 33, 45, 57, 69):
            with self.subTest(note=note):
                shifted = midi_bridge_core.transpose_into_range(note)
                self.assertEqual(shifted % 12, note % 12)
                self.assertGreaterEqual(
                    shifted, midi_bridge_core.LOWEST_PLAYABLE_NOTE
                )
                self.assertLessEqual(
                    shifted, midi_bridge_core.HIGHEST_PLAYABLE_NOTE
                )

    def test_octave_shift_transposes_the_whole_performance(self) -> None:
        base = MidiNoteEngine(min_gate_ms=0)
        shifted = MidiNoteEngine(min_gate_ms=0, octave_shift=1)

        # Notes comfortably inside range must exactly double one octave up.
        for note in (72, 79, 84):
            with self.subTest(note=note):
                self.assertAlmostEqual(
                    shifted.note_hz(note) / base.note_hz(note), 2.0, places=2
                )

    def test_octave_shift_is_validated(self) -> None:
        for bad in (9, -9, 1.5, True):
            with self.subTest(value=bad):
                with self.assertRaises(ValueError):
                    MidiNoteEngine(octave_shift=bad)

    def test_fold_octaves_restores_the_legacy_single_octave_mapping(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0, fold_octaves=True)

        for note in range(128):
            with self.subTest(note=note):
                self.assertEqual(
                    engine.handle_message(note_on(note)), folded_hz(note)
                )
                self.assertEqual(engine.handle_message(note_off(note)), 0)

    def test_folded_octave_has_exact_rounded_frequencies(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0, fold_octaves=True)

        for offset, expected in enumerate(FOLDED_HZ):
            with self.subTest(note=93 + offset):
                self.assertEqual(
                    engine.handle_message(note_on(93 + offset)), expected
                )
                self.assertEqual(engine.panic(), 0)

    def test_last_press_wins_and_releasing_it_restores_previous_note(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0)

        self.assertEqual(engine.handle_message(note_on(60)), hz(60))
        self.assertEqual(engine.handle_message(note_on(62)), hz(62))
        self.assertEqual(engine.handle_message(note_on(64)), hz(64))
        self.assertIsNone(engine.handle_message(note_off(62)))
        self.assertEqual(engine.current_hz, hz(64))
        self.assertEqual(engine.handle_message(note_off(64)), hz(60))
        self.assertEqual(engine.handle_message(note_off(60)), 0)

    def test_duplicate_presses_are_individual_fifo_matched_tokens(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0)

        self.assertEqual(engine.handle_message(note_on(60)), hz(60))
        self.assertEqual(engine.handle_message(note_on(62)), hz(62))
        self.assertEqual(engine.handle_message(note_on(60)), hz(60))
        self.assertEqual(engine.held_count, 3)
        self.assertIsNone(engine.handle_message(note_off(60)))
        self.assertEqual(engine.current_hz, hz(60))
        self.assertEqual(engine.held_count, 2)
        self.assertEqual(engine.handle_message(note_off(60)), hz(62))
        self.assertEqual(engine.held_count, 1)

    def test_note_on_velocity_zero_is_a_single_release(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0)

        self.assertEqual(engine.handle_message(note_on(69)), hz(69))
        self.assertIsNone(engine.handle_message(note_on(69)))
        self.assertEqual(engine.held_count, 2)
        self.assertIsNone(engine.handle_message(note_on(69, velocity=0)))
        self.assertEqual(engine.held_count, 1)
        self.assertEqual(engine.handle_message(note_on(69, velocity=0)), 0)

    def test_same_frequency_changes_are_coalesced(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0)

        self.assertEqual(engine.handle_message(note_on(0)), hz(60))
        self.assertIsNone(engine.handle_message(note_on(12)))
        self.assertIsNone(engine.handle_message(note_on(24)))
        self.assertEqual(engine.held_count, 3)
        self.assertIsNone(engine.handle_message(note_off(12)))
        self.assertIsNone(engine.handle_message(note_off(24)))
        self.assertEqual(engine.handle_message(note_off(0)), 0)

    def test_channels_are_not_filtered_by_the_core(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0)

        self.assertEqual(engine.handle_message(note_on(60, channel=1)), hz(60))
        self.assertEqual(engine.handle_message(note_on(62, channel=15)), hz(62))
        self.assertEqual(engine.handle_message(note_off(62, channel=7)), hz(60))

    def test_sustain_latches_released_tokens_until_pedal_up(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0)

        self.assertEqual(engine.handle_message(note_on(60)), hz(60))
        self.assertIsNone(engine.handle_message(cc(64, 127)))
        self.assertIsNone(engine.handle_message(note_off(60)))
        self.assertEqual(engine.held_count, 0)
        self.assertEqual(engine.current_hz, hz(60))
        self.assertEqual(engine.handle_message(cc(64, 63)), 0)

    def test_sustain_release_restores_latest_physically_held_note(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0)

        self.assertEqual(engine.handle_message(note_on(60)), hz(60))
        self.assertIsNone(engine.handle_message(cc(64, 64)))
        self.assertEqual(engine.handle_message(note_on(62)), hz(62))
        self.assertIsNone(engine.handle_message(note_off(62)))
        self.assertEqual(engine.handle_message(note_on(64)), hz(64))
        self.assertIsNone(engine.handle_message(note_off(64)))
        self.assertEqual(engine.held_count, 1)
        self.assertEqual(engine.handle_message(cc(64, 0)), hz(60))

    def test_sustain_disabled_ignores_cc64(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0, sustain_enabled=False)

        self.assertEqual(engine.handle_message(note_on(60)), hz(60))
        self.assertIsNone(engine.handle_message(cc(64, 127)))
        self.assertEqual(engine.handle_message(note_off(60)), 0)

    def test_panic_method_clears_notes_sustain_and_pending_gate(self) -> None:
        clock = FakeClock()
        engine = MidiNoteEngine(clock_ns=clock)

        self.assertEqual(engine.handle_message(note_on(60)), hz(60))
        self.assertIsNone(engine.handle_message(cc(64, 127)))
        self.assertIsNone(engine.handle_message(note_off(60)))
        self.assertEqual(engine.panic(), 0)
        self.assertEqual(engine.current_hz, 0)
        self.assertEqual(engine.held_count, 0)
        self.assertIsNone(engine.poll(now_ns=1_000_000_000))
        self.assertIsNone(engine.panic())

    def test_cc120_and_cc123_are_immediate_panic(self) -> None:
        for control in (120, 123):
            with self.subTest(control=control):
                engine = MidiNoteEngine()
                self.assertEqual(engine.handle_message(note_on(60)), hz(60))
                self.assertEqual(engine.handle_message(cc(control, 0)), 0)
                self.assertEqual(engine.current_hz, 0)
                self.assertEqual(engine.held_count, 0)

    def test_final_note_is_gated_until_exact_deadline(self) -> None:
        clock = FakeClock(10_000_000)
        engine = MidiNoteEngine(clock_ns=clock)

        self.assertEqual(engine.handle_message(note_on(60)), hz(60))
        clock.advance_ms(10)
        self.assertIsNone(engine.handle_message(note_off(60)))
        self.assertEqual(engine.current_hz, hz(60))
        clock.advance_ms(34.999999)
        self.assertIsNone(engine.poll())
        self.assertEqual(engine.current_hz, hz(60))
        clock.advance_ms(0.000001)
        self.assertEqual(engine.poll(), 0)
        self.assertEqual(engine.current_hz, 0)
        self.assertIsNone(engine.poll())

    def test_final_release_after_minimum_gate_stops_immediately(self) -> None:
        clock = FakeClock()
        engine = MidiNoteEngine(clock_ns=clock)

        self.assertEqual(engine.handle_message(note_on(60)), hz(60))
        clock.advance_ms(45)
        self.assertEqual(engine.handle_message(note_off(60)), 0)

    def test_explicit_now_ns_overrides_injected_clock(self) -> None:
        clock = FakeClock(999_000_000)
        engine = MidiNoteEngine(clock_ns=clock)

        self.assertEqual(engine.handle_message(note_on(60), now_ns=100), hz(60))
        self.assertIsNone(engine.handle_message(note_off(60), now_ns=101))
        self.assertIsNone(engine.poll(now_ns=45_000_099))
        self.assertEqual(engine.poll(now_ns=45_000_100), 0)

    def test_new_note_cancels_pending_stop(self) -> None:
        clock = FakeClock()
        engine = MidiNoteEngine(clock_ns=clock)

        self.assertEqual(engine.handle_message(note_on(60)), hz(60))
        clock.advance_ms(5)
        self.assertIsNone(engine.handle_message(note_off(60)))
        clock.advance_ms(5)
        self.assertEqual(engine.handle_message(note_on(62)), hz(62))
        clock.advance_ms(40)
        self.assertIsNone(engine.poll())
        self.assertEqual(engine.current_hz, hz(62))

    def test_same_frequency_repress_does_not_restart_gate_clock(self) -> None:
        clock = FakeClock()
        engine = MidiNoteEngine(clock_ns=clock)

        self.assertEqual(engine.handle_message(note_on(0)), hz(60))
        clock.advance_ms(20)
        self.assertIsNone(engine.handle_message(note_on(12)))
        clock.advance_ms(25)
        self.assertIsNone(engine.handle_message(note_off(0)))
        self.assertEqual(engine.handle_message(note_off(12)), 0)

    def test_stray_releases_invalid_notes_and_unknown_messages_are_noops(self) -> None:
        engine = MidiNoteEngine(min_gate_ms=0)

        self.assertIsNone(engine.handle_message(note_off(60)))
        self.assertIsNone(engine.handle_message(note_on(-1)))
        self.assertIsNone(engine.handle_message(note_on(128)))
        self.assertIsNone(engine.handle_message(message("pitchwheel", pitch=100)))
        self.assertEqual(engine.current_hz, 0)
        self.assertEqual(engine.held_count, 0)

    def test_negative_gate_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            MidiNoteEngine(min_gate_ms=-1)

    def test_core_has_no_midi_serial_or_hardware_imports(self) -> None:
        tree = ast.parse(inspect.getsource(midi_bridge_core))
        imported_roots = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported_roots.update(alias.name.split(".", 1)[0] for alias in node.names)
            elif isinstance(node, ast.ImportFrom) and node.module:
                imported_roots.add(node.module.split(".", 1)[0])

        self.assertTrue(imported_roots.isdisjoint({"mido", "serial", "rtmidi", "hardware"}))


if __name__ == "__main__":
    unittest.main()
