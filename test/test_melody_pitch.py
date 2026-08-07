"""Tests for the piezo pitch primitives."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path


MODULE_PATH = (
    Path(__file__).resolve().parents[1] / "tools" / "melody_pitch.py"
)
SPEC = importlib.util.spec_from_file_location("melody_pitch", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MP = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MP
SPEC.loader.exec_module(MP)


class FrequencyMathTests(unittest.TestCase):
    def test_reference_pitches(self) -> None:
        self.assertAlmostEqual(MP.midi_to_hz(69), 440.0, places=6)
        self.assertAlmostEqual(MP.midi_to_hz(60), 261.6255, places=3)
        self.assertAlmostEqual(MP.midi_to_hz(81), 880.0, places=6)

    def test_round_trip(self) -> None:
        for note in range(40, 120):
            with self.subTest(note=note):
                self.assertAlmostEqual(
                    MP.hz_to_midi(MP.midi_to_hz(note)), note, places=6
                )

    def test_an_octave_doubles_frequency(self) -> None:
        for note in (60, 72, 84, 96):
            self.assertAlmostEqual(
                MP.midi_to_hz(note + 12) / MP.midi_to_hz(note), 2.0, places=9
            )

    def test_hz_to_midi_rejects_non_positive(self) -> None:
        for bad in (0.0, -1.0):
            with self.subTest(value=bad):
                with self.assertRaises(ValueError):
                    MP.hz_to_midi(bad)

    def test_boundary_helpers_stay_inside_the_requested_hz(self) -> None:
        for hz in (500, 1500, 2000, 3500, 4000):
            with self.subTest(hz=hz):
                low = MP.lowest_midi_at_or_above(hz)
                high = MP.highest_midi_at_or_below(hz)
                self.assertGreaterEqual(MP.midi_to_hz(low), hz - 1e-6)
                self.assertLessEqual(MP.midi_to_hz(high), hz + 1e-6)
                # Strictly inside: one step further would cross the bound.
                self.assertLess(MP.midi_to_hz(low - 1), hz)
                self.assertGreater(MP.midi_to_hz(high + 1), hz)


class PitchBandTests(unittest.TestCase):
    def test_candidate_bands_match_the_measured_envelope(self) -> None:
        # Locks in the trade-off the range-policy research must resolve:
        # the loud band buys audibility at the cost of melodic room.
        self.assertEqual(
            (MP.BAND_LOUD.low_midi, MP.BAND_LOUD.high_midi), (91, 104)
        )
        self.assertEqual(MP.BAND_LOUD.semitone_span, 13)
        self.assertEqual(
            (MP.BAND_WIDE.low_midi, MP.BAND_WIDE.high_midi), (72, 107)
        )
        self.assertEqual(MP.BAND_WIDE.semitone_span, 35)
        self.assertGreater(
            MP.BAND_WIDE.semitone_span, MP.BAND_LOUD.semitone_span
        )

    def test_every_note_in_a_band_is_a_legal_tone_argument(self) -> None:
        for band in (MP.BAND_LOUD, MP.BAND_WIDE):
            for note in range(band.low_midi, band.high_midi + 1):
                with self.subTest(band=band.name, note=note):
                    hz = MP.quantize_hz(note)
                    self.assertTrue(MP.is_playable_hz(hz))
                    self.assertGreaterEqual(hz, MP.TONE_MIN_HZ)
                    self.assertLessEqual(hz, MP.TONE_MAX_HZ)

    def test_loud_band_stays_inside_the_resonant_window(self) -> None:
        low_hz = MP.midi_to_hz(MP.BAND_LOUD.low_midi)
        high_hz = MP.midi_to_hz(MP.BAND_LOUD.high_midi)
        self.assertGreaterEqual(low_hz, MP.RESONANT_LOW_HZ)
        self.assertLessEqual(high_hz, MP.RESONANT_HIGH_HZ)

    def test_wide_band_never_dips_below_the_audible_floor(self) -> None:
        self.assertGreaterEqual(
            MP.midi_to_hz(MP.BAND_WIDE.low_midi), MP.AUDIBLE_FLOOR_HZ
        )

    def test_clamp_and_contains(self) -> None:
        band = MP.BAND_LOUD
        self.assertTrue(band.contains(band.low_midi))
        self.assertTrue(band.contains(band.high_midi))
        self.assertFalse(band.contains(band.low_midi - 1))
        self.assertFalse(band.contains(band.high_midi + 1))
        self.assertEqual(band.clamp(0), band.low_midi)
        self.assertEqual(band.clamp(127), band.high_midi)
        self.assertEqual(band.clamp(band.low_midi + 2), band.low_midi + 2)

    def test_band_construction_rejects_impossible_ranges(self) -> None:
        with self.assertRaises(ValueError):
            MP.PitchBand.from_hz("inverted", 3000, 1000)
        with self.assertRaises(ValueError):
            MP.PitchBand.from_hz("below-firmware", 100, 1000)
        with self.assertRaises(ValueError):
            MP.PitchBand.from_hz("above-firmware", 1000, 7000)
        with self.assertRaises(ValueError):
            # Too narrow to contain a whole semitone.
            MP.PitchBand.from_hz("hairline", 2000, 2001)

    def test_rearticulation_gap_matches_the_measured_finding(self) -> None:
        # notes.h: 30-50 ms gap needed or two notes sound like one.
        self.assertGreaterEqual(MP.MIN_REARTICULATION_GAP_MS, 30)
        self.assertLessEqual(MP.MIN_REARTICULATION_GAP_MS, 50)


if __name__ == "__main__":
    unittest.main()
