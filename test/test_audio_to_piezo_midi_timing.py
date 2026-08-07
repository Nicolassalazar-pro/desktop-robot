"""Timing tests for MIDI-to-piezo conversion."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools import audio_to_piezo as piezo


def message(message_type: str, time: float, **values: object) -> SimpleNamespace:
    return SimpleNamespace(type=message_type, time=time, **values)


class AudioToPiezoMidiTimingTests(unittest.TestCase):
    def convert(
        self, messages: list[SimpleNamespace], strategy: str = "loudest"
    ) -> list[piezo.Note]:
        fake_mido = SimpleNamespace(MidiFile=lambda _path: messages)
        with mock.patch.dict(sys.modules, {"mido": fake_mido}):
            return piezo.midi_to_events(Path("synthetic.mid"), strategy)

    def assert_notes(
        self,
        actual: list[piezo.Note],
        expected: list[tuple[int, int, str]],
    ) -> None:
        self.assertEqual(
            [(note.hz, note.ms, note.name) for note in actual],
            expected,
        )
        self.assertEqual(
            sum(note.ms for note in actual),
            sum(duration for _, duration, _ in expected),
        )

    def test_control_and_meta_events_preserve_three_second_duration(self) -> None:
        notes = self.convert(
            [
                message("track_name", 0.0),
                message("note_on", 0.0, note=69, velocity=100),
                message("control_change", 0.75, control=1, value=64),
                message("marker", 0.25, text="middle"),
                message("control_change", 1.0, control=64, value=127),
                message("note_off", 1.0, note=69, velocity=0),
            ]
        )

        self.assert_notes(notes, [(440, 3000, "A4")])

    def test_polyphonic_selection_changes_do_not_lose_nonselected_time(self) -> None:
        notes = self.convert(
            [
                message("note_on", 0.0, note=60, velocity=80),
                message("control_change", 0.25, control=1, value=32),
                message("note_on", 0.25, note=72, velocity=70),
                message("note_on", 0.25, note=48, velocity=127),
                message("note_off", 0.25, note=60, velocity=0),
                message("note_on", 0.5, note=84, velocity=60),
                message("note_off", 0.25, note=48, velocity=0),
                message("note_off", 0.25, note=84, velocity=0),
                message("note_off", 0.25, note=60, velocity=0),
                message("note_off", 0.25, note=72, velocity=0),
                message("end_of_track", 0.5),
            ],
            strategy="highest",
        )

        self.assert_notes(
            notes,
            [
                (262, 500, "C4"),
                (523, 1000, "C5"),
                (1047, 500, "C6"),
                (523, 500, "C5"),
                (0, 500, "rest"),
            ],
        )

    def test_leading_and_trailing_rests_keep_their_full_duration(self) -> None:
        notes = self.convert(
            [
                message("marker", 0.25, text="count-in"),
                message("note_on", 0.0, note=69, velocity=100),
                message("control_change", 0.25, control=1, value=64),
                message("note_off", 0.25, note=69, velocity=0),
                message("end_of_track", 0.25),
            ]
        )

        self.assert_notes(
            notes,
            [
                (0, 250, "rest"),
                (440, 500, "A4"),
                (0, 250, "rest"),
            ],
        )

    def test_tempo_meta_event_deltas_are_retained_after_mido_conversion(self) -> None:
        notes = self.convert(
            [
                message("note_on", 0.0, note=69, velocity=100),
                message("set_tempo", 0.5, tempo=500_000),
                message("control_change", 1.0, control=1, value=64),
                message("set_tempo", 0.0, tempo=1_000_000),
                message("note_off", 1.0, note=69, velocity=0),
                message("end_of_track", 0.5),
            ]
        )

        self.assert_notes(
            notes,
            [
                (440, 2500, "A4"),
                (0, 500, "rest"),
            ],
        )

    def test_duplicate_same_pitch_note_off_releases_only_one_voice(self) -> None:
        notes = self.convert(
            [
                message("note_on", 0.0, channel=0, note=69, velocity=100),
                message("note_on", 0.25, channel=0, note=69, velocity=80),
                message("note_off", 0.25, channel=0, note=69, velocity=0),
                message("note_off", 0.25, channel=0, note=69, velocity=0),
            ]
        )

        self.assert_notes(notes, [(440, 750, "A4")])

    def test_same_pitch_on_separate_channels_stays_active(self) -> None:
        notes = self.convert(
            [
                message("note_on", 0.0, channel=0, note=69, velocity=100),
                message("note_on", 0.25, channel=1, note=69, velocity=80),
                message("note_off", 0.25, channel=0, note=69, velocity=0),
                message("note_off", 0.25, channel=1, note=69, velocity=0),
            ]
        )

        self.assert_notes(notes, [(440, 750, "A4")])


if __name__ == "__main__":
    unittest.main()
