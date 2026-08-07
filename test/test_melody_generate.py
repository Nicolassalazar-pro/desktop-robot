"""Tests for the pure mood-melody generator."""

from __future__ import annotations

import collections
import statistics
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools import melody_generate as MG  # noqa: E402
from tools import melody_pitch as MP  # noqa: E402
from tools import melody_rng as MR  # noqa: E402


MOODS = ("happy", "sad", "confused")


class GenerationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.phrases = {
            (mood, seed): MG.generate(mood, seed)
            for mood in MOODS
            for seed in range(300)
        }

    def test_same_seed_and_mood_are_identical(self) -> None:
        for mood in MOODS:
            with self.subTest(mood=mood):
                self.assertEqual(
                    MG.generate(mood, 0x123456789ABCDEF),
                    MG.generate(mood, 0x123456789ABCDEF),
                )

    def test_different_seeds_produce_many_distinct_phrases(self) -> None:
        for mood in MOODS:
            keys = {
                (
                    self.phrases[mood, seed].notes,
                    self.phrases[mood, seed].midi_notes,
                    self.phrases[mood, seed].terminal_function,
                    self.phrases[mood, seed].length_band,
                )
                for seed in range(128)
            }
            with self.subTest(mood=mood):
                self.assertGreaterEqual(len(keys), 120)

    def test_all_notes_obey_hardware_constraints(self) -> None:
        legal_hz = {
            MP.quantize_hz(midi)
            for midi in range(
                MP.BAND_WORKING.low_midi,
                MP.BAND_WORKING.high_midi + 1,
            )
        }
        for (mood, seed), phrase in self.phrases.items():
            for note in phrase.notes:
                with self.subTest(mood=mood, seed=seed, note=note):
                    self.assertIn(note.hz, legal_hz)
                    self.assertTrue(MP.is_playable_hz(note.hz))
                    self.assertGreaterEqual(note.gate_ms, MP.MIN_NOTE_MS)

    def test_repeated_pitches_have_a_real_rearticulation_gap(self) -> None:
        for (mood, seed), phrase in self.phrases.items():
            for index, (left, right) in enumerate(
                zip(phrase.midi_notes, phrase.midi_notes[1:])
            ):
                if left == right:
                    with self.subTest(mood=mood, seed=seed, index=index):
                        self.assertGreaterEqual(
                            phrase.notes[index].gap_ms,
                            MP.MIN_REARTICULATION_GAP_MS,
                        )

    def test_lengths_use_the_two_level_bands(self) -> None:
        selected_bands = {
            phrase.length_band for phrase in self.phrases.values()
        }
        self.assertEqual(selected_bands, set(MG.LENGTH_BANDS))
        # Two moods cap their length for reasons that are audible rather than
        # arbitrary: confused bodies must stay compact or they zigzag, and sad
        # notes are slow enough that a long draw drags. When a draw exceeds the
        # ceiling it is remapped, so the realised length can sit below the band
        # that was drawn — but never above it, and never outside 2..16.
        ceilings = {
            "confused": MG.CONFUSED_MAX_LENGTH,
            "sad": MG.SAD_MAX_LENGTH,
        }
        for (mood, seed), phrase in self.phrases.items():
            length = len(phrase.notes)
            assert phrase.length_band is not None
            with self.subTest(mood=mood, seed=seed):
                self.assertGreaterEqual(length, 2)
                self.assertLessEqual(length, 16)
                ceiling = ceilings.get(mood)
                if ceiling is not None:
                    self.assertLessEqual(length, ceiling)
                    if phrase.length_band[1] <= ceiling:
                        self.assertLessEqual(phrase.length_band[0], length)
                        self.assertGreaterEqual(phrase.length_band[1], length)
                else:
                    # A repeated closing pitch is merged into one held note
                    # after the length is drawn, so the realised count can sit
                    # one below the band minimum. That is the resolution
                    # sounding right, not the band being violated.
                    self.assertLessEqual(phrase.length_band[0] - 1, length)
                    self.assertGreaterEqual(phrase.length_band[1], length)

    def test_mode_tonic_and_third_are_audible_for_longer_phrases(self) -> None:
        # Only the tonal moods. Confused is deliberately atonal: a whole-tone
        # body has no leading tone and no major/minor cadence, which is the
        # point — requiring a tonic would give it the resolution the emotion
        # is supposed to withhold.
        third = {"happy": 4, "sad": 3}
        for (mood, seed), phrase in self.phrases.items():
            if mood not in third or len(phrase.notes) < 3:
                continue
            pitch_classes = {
                (midi - MP.TONIC_MIDI) % 12
                for midi in phrase.midi_notes
            }
            with self.subTest(mood=mood, seed=seed):
                self.assertIn(0, pitch_classes)
                self.assertIn(third[mood], pitch_classes)

    def test_happy_and_sad_stay_in_their_scale_masks(self) -> None:
        masks = {"happy": set(MG.MAJOR_MASK), "sad": set(MG.MINOR_MASK)}
        for mood, mask in masks.items():
            for seed in range(300):
                phrase = self.phrases[mood, seed]
                pitch_classes = {
                    (midi - MP.TONIC_MIDI) % 12
                    for midi in phrase.midi_notes
                }
                with self.subTest(mood=mood, seed=seed):
                    self.assertLessEqual(pitch_classes, mask)

    def test_reported_scale_mask_contains_every_generated_pitch(self) -> None:
        for (mood, seed), phrase in self.phrases.items():
            pitch_classes = {
                (midi - MP.TONIC_MIDI) % 12
                for midi in phrase.midi_notes
            }
            with self.subTest(mood=mood, seed=seed):
                self.assertLessEqual(pitch_classes, set(phrase.scale_mask))

    def test_happy_ioi_is_clearly_shorter_than_sad_ioi(self) -> None:
        def mean_ioi(mood: str) -> float:
            return statistics.mean(
                note.gate_ms + note.gap_ms
                for seed in range(300)
                for note in self.phrases[mood, seed].notes
            )

        happy = mean_ioi("happy")
        sad = mean_ioi("sad")
        # Sad is still the slower of the two, but both now sit in the fast
        # chirp register. Slow sustained tones were removed as a sad cue
        # because on a bare square wave they read as a theremin, not as
        # sadness — sad now carries its emotion via descending contour and
        # minor intervals instead of via tempo.
        self.assertLess(happy, sad)
        self.assertGreater(sad - happy, 50.0)
        # Neither mood may drift back into drone territory.
        self.assertLess(sad, 400.0)

    def test_terminal_note_matches_selected_function(self) -> None:
        fixed_offsets = {
            "tonic": 0,
            "fifth": 7,
        }
        for (mood, seed), phrase in self.phrases.items():
            with self.subTest(mood=mood, seed=seed):
                if mood == "confused":
                    # Confusion reports which unresolved gesture it used, not
                    # an absolute scale degree — it has no tonic to resolve to.
                    self.assertIn(
                        phrase.terminal_function,
                        {name for name, _ in MG.CONFUSED_ENDINGS},
                    )
                    self.assertEqual(
                        phrase.midi_notes[-1], phrase.terminal_midi
                    )
                    continue
                expected = (
                    {"happy": 4, "sad": 3}[mood]
                    if phrase.terminal_function == "third"
                    else fixed_offsets[phrase.terminal_function]
                )
                actual = (phrase.midi_notes[-1] - MP.TONIC_MIDI) % 12
                self.assertEqual(actual, expected)
                self.assertEqual(phrase.midi_notes[-1], phrase.terminal_midi)

    def test_confused_ends_unresolved_by_one_of_several_gestures(
        self,
    ) -> None:
        # The requirement is that the phrase does NOT resolve — not that it
        # rises. A single mandatory rising ending made confusion predictable,
        # and the rising cue is only empirical for SPEECH ("Huh?" repair
        # interjections); applying it to buzzer tones was always inference.
        # Three gestures now share the work, each unresolved by a different
        # route.
        for seed in range(300):
            phrase = self.phrases["confused", seed]
            midi = phrase.midi_notes
            gesture = phrase.terminal_function
            with self.subTest(seed=seed, gesture=gesture):
                self.assertIn(
                    gesture, {name for name, _ in MG.CONFUSED_ENDINGS}
                )
                # Never returns to where it started, and never tops the phrase.
                self.assertNotEqual(midi[-1], midi[0])
                if len(midi) >= 3:
                    self.assertLess(midi[-1], max(midi[:-1]))
                body_end = midi[phrase.body_length - 1]
                if gesture == "rise":
                    # Lifts, and is held so the question hangs.
                    self.assertGreater(midi[-1], body_end)
                    self.assertGreaterEqual(phrase.notes[-1].gate_ms, 190)
                elif gesture == "trail":
                    # Tails off downward and stops short rather than asking.
                    self.assertLess(midi[-1], body_end)
                    self.assertLessEqual(phrase.notes[-1].gate_ms, 140)
                elif gesture == "insist":
                    # Repeats the pitch, then bites off a small interval.
                    # Repetition is the insistence; the short final gate keeps
                    # it annoyed rather than resigned.
                    self.assertEqual(midi[phrase.body_length], body_end)
                    self.assertIn(body_end - midi[-1], (1, 2, 3))
                    self.assertLessEqual(phrase.notes[-1].gate_ms, 120)
                else:  # falter — dip, then lift back part-way
                    self.assertLess(midi[-2], body_end)
                    self.assertGreater(midi[-1], midi[-2])

    def test_confused_has_exactly_one_medial_hesitation(self) -> None:
        for seed in range(300):
            phrase = self.phrases["confused", seed]
            with self.subTest(seed=seed):
                hesitation_notes = [
                    n for n in phrase.notes[:-1] if n.gap_ms >= 180
                ]
                expected = 0 if len(phrase.notes) == 2 else 1
                self.assertEqual(len(hesitation_notes), expected)
                if hesitation_notes:
                    self.assertLessEqual(hesitation_notes[0].gap_ms, 260)
                    self.assertLessEqual(hesitation_notes[0].gate_ms, 110)
                for note in phrase.notes[:-1]:
                    if note.gap_ms < 180:
                        self.assertGreaterEqual(note.gap_ms, 35)
                        self.assertLessEqual(note.gap_ms, 75)

    def test_confused_forbids_tritones_and_large_leaps(self) -> None:
        # No tritones anywhere in confusion — body or ending. Nick's call:
        # sourness comes from semitone clashes instead, which read as
        # frustrated without the sinister edge a tritone brings.
        for seed in range(300):
            midi = self.phrases["confused", seed].midi_notes
            intervals = [
                midi[i + 1] - midi[i] for i in range(len(midi) - 1)
            ]
            with self.subTest(seed=seed):
                for interval in intervals:
                    self.assertNotEqual(abs(interval), 6)
                    self.assertLessEqual(abs(interval), 7)

    def test_confused_final_move_is_never_zero(self) -> None:
        for seed in range(300):
            midi = self.phrases["confused", seed].midi_notes
            with self.subTest(seed=seed):
                self.assertNotEqual(midi[-1] - midi[-2], 0)

    def test_confused_body_notes_stay_short(self) -> None:
        # Global slowness is exactly what would make confused collide with
        # sad. Confusion stays quick and uses ONE interruption instead.
        confused_gates = [
            note.gate_ms
            for seed in range(300)
            for note in self.phrases["confused", seed].notes[:-1]
        ]
        sad_gates = [
            note.gate_ms
            for seed in range(300)
            for note in self.phrases["sad", seed].notes
        ]
        mean_confused = sum(confused_gates) / len(confused_gates)
        mean_sad = sum(sad_gates) / len(sad_gates)
        # Both moods are now chirp-fast, so confusion can no longer be told
        # from sadness by tempo alone. What separates them is that confusion
        # interrupts itself once — covered by the hesitation-contrast test —
        # while its body notes stay at or below sad's pace.
        self.assertLessEqual(mean_confused, mean_sad * 1.05)
        self.assertTrue(all(90 <= gate <= 150 for gate in confused_gates))

    def test_confused_body_is_a_small_whole_tone_fragment(self) -> None:
        for seed in range(300):
            phrase = self.phrases["confused", seed]
            # The closing gesture is one or two notes depending on which was
            # chosen, and it deliberately steps outside the whole-tone body —
            # that departure is what makes the ending sound unresolved. Use the
            # recorded split rather than assuming a fixed tail length.
            body = phrase.midi_notes[: phrase.body_length]
            body_classes = {
                (midi - MP.TONIC_MIDI) % 12 for midi in body
            }
            with self.subTest(seed=seed):
                self.assertLessEqual(body_classes, set(MG.CONFUSED_MASK))
                if len(body) >= 3:
                    self.assertGreaterEqual(len(set(body)), 3)
                    self.assertLessEqual(
                        max(body) - min(body), MG._CONFUSED_MAX_BODY_SPAN
                    )

    def test_confused_rise_never_becomes_the_phrase_peak(self) -> None:
        # Bench 2026-08-04: a terminal rise that landed on the highest note of
        # the phrase was heard as "victory". Climbing past everything already
        # played is a triumph gesture; a question lifts from a settled point.
        # Tying the peak still reads conclusive, hence the strict comparison.
        for seed in range(600):
            midi = MG.generate("confused", seed=seed).midi_notes
            if len(midi) < 3:
                continue
            with self.subTest(seed=seed, notes=midi):
                self.assertLess(midi[-1], max(midi[:-1]))

    def test_confused_never_becomes_a_fanfare(self) -> None:
        # Bench finding 2026-08-04: a monotonically ascending body plus the
        # terminal rise was heard as "the Star Spangled Banner". A phrase that
        # only climbs reads as triumphant, which is the opposite of puzzled.
        for seed in range(600):
            midi = MG.generate("confused", seed=seed).midi_notes
            if len(midi) < 3:
                continue
            ascending = all(
                midi[i + 1] > midi[i] for i in range(len(midi) - 1)
            )
            with self.subTest(seed=seed, notes=midi):
                self.assertFalse(
                    ascending,
                    "confused phrase ascends throughout and will sound "
                    "like a fanfare",
                )

    def test_confused_body_turns_before_the_terminal_rise(self) -> None:
        # The rise must stand out as a question, not as the top of a ramp, so
        # the body may not hand it an ascent.
        for seed in range(600):
            midi = MG.generate("confused", seed=seed).midi_notes
            body = midi[:-1]
            if len(body) < 2:
                continue
            body_ascends = all(
                body[i + 1] > body[i] for i in range(len(body) - 1)
            )
            with self.subTest(seed=seed, body=body):
                self.assertFalse(body_ascends)

    def test_confused_uses_searching_reversals_without_zigzagging(self) -> None:
        # Measured over the BODY only. The closing gesture deliberately turns
        # (falter dips then lifts), so including it would count intentional
        # motion as zigzagging. Within the body, turns must stay well below
        # every-note alternation or the contour reads as agitation rather than
        # as looking around.
        for seed in range(300):
            phrase = self.phrases["confused", seed]
            body = phrase.midi_notes[: phrase.body_length]
            if len(body) < 4:
                continue
            directions = [
                1 if body[i + 1] > body[i] else -1
                for i in range(len(body) - 1)
            ]
            changes = sum(
                left != right
                for left, right in zip(directions, directions[1:])
            )
            available = len(directions) - 1
            with self.subTest(seed=seed, body=body):
                self.assertLessEqual(changes, max(2, available // 2))

    def test_confused_bodies_obey_their_rules_without_being_templates(
        self,
    ) -> None:
        # Bodies used to come from one or two fixed contours per length, which
        # made confusion predictable — 600 phrases gave only 57 distinct
        # shapes. They are generated now, so assert the RULES rather than the
        # templates.
        for seed in range(300):
            phrase = self.phrases["confused", seed]
            body = phrase.midi_notes[: phrase.body_length]
            with self.subTest(seed=seed, body=body):
                steps = [
                    body[i + 1] - body[i] for i in range(len(body) - 1)
                ]
                # Small chromatic motion. The semitone is the sour interval
                # that carries the frustration; nothing larger than a minor
                # third, and never a tritone.
                for step in steps:
                    self.assertIn(abs(step), (1, 2, 3))
                if len(body) >= 3:
                    # At least one semitone clash, or the line slides back to
                    # the smooth whole-tone reading that sounded merely dreamy.
                    self.assertIn(1, [abs(step) for step in steps])
                if len(body) >= 2:
                    # Must end below its own peak so the closing gesture stays
                    # a local move rather than a triumphant climb. Two-note
                    # bodies are the exception: with a single step there is no
                    # earlier peak to sit under.
                    if len(body) >= 3:
                        self.assertGreaterEqual(
                            max(body) - body[-1],
                            min(MG.CONFUSED_RISE_CHOICES),
                        )

    def test_confused_is_not_predictable(self) -> None:
        # The failure this guards against is aesthetic, not functional: every
        # phrase sounding like the same memorised sign-off.
        shapes = collections.Counter()
        endings = collections.Counter()
        for seed in range(600):
            phrase = MG.generate("confused", seed=seed)
            first = phrase.midi_notes[0]
            shapes[tuple(n - first for n in phrase.midi_notes)] += 1
            endings[phrase.terminal_function] += 1
        self.assertGreater(len(shapes), 200)
        # No single shape may dominate the way a template-driven one did.
        self.assertLess(shapes.most_common(1)[0][1], 60)
        # All three closing gestures must actually get used.
        self.assertEqual(
            set(endings), {name for name, _ in MG.CONFUSED_ENDINGS}
        )

    def test_confused_uses_the_full_length_range(self) -> None:
        # The wider two-octave band raised the ceiling from 7 to 9. It cannot
        # go higher: research caps the body at 5-8 semitones, and many notes
        # inside a compact span must oscillate.
        counts = collections.Counter(
            len(MG.generate("confused", seed=seed).notes)
            for seed in range(600)
        )
        self.assertEqual(max(counts), MG.CONFUSED_MAX_LENGTH)
        self.assertGreaterEqual(min(counts), 2)
        for length in (5, 8, 9):
            self.assertGreater(counts[length], 0)
        # No single length may dominate the way a hard collapse would.
        self.assertLess(max(counts.values()), sum(counts.values()) * 0.35)

    def test_confused_body_stays_compact(self) -> None:
        # Research: 5-8 semitones. Wider reads as agitation, not searching.
        # Measured over the body only; the closing gesture deliberately steps
        # outside it.
        for seed in range(300):
            phrase = self.phrases["confused", seed]
            body = phrase.midi_notes[: phrase.body_length]
            with self.subTest(seed=seed):
                self.assertLessEqual(
                    max(body) - min(body), MG._CONFUSED_MAX_BODY_SPAN
                )

    def test_two_note_gestures_are_deliberately_special_cased(self) -> None:
        examples = {}
        for mood in MOODS:
            examples[mood] = next(
                self.phrases[mood, seed]
                for seed in range(300)
                if len(self.phrases[mood, seed].notes) == 2
            )

        self.assertEqual(
            examples["happy"].midi_notes,
            (MP.TONIC_MIDI, MP.TONIC_MIDI + 4),
        )
        self.assertEqual(
            examples["sad"].midi_notes,
            (MP.TONIC_MIDI + 3, MP.TONIC_MIDI),
        )
        confused = examples["confused"].midi_notes
        # Two notes cannot establish a tonal centre, so confusion is reduced to
        # its closing gesture alone: an anchor note plus one unresolved move.
        # It does NOT start on the tonic — confusion has no tonic.
        self.assertTrue(
            MG.CONFUSED_ANCHOR_LOW_MIDI
            <= confused[0]
            <= MG.CONFUSED_ANCHOR_HIGH_MIDI
        )
        self.assertNotEqual(confused[1], confused[0])
        self.assertGreaterEqual(examples["confused"].notes[0].gap_ms, 35)
        self.assertLessEqual(examples["confused"].notes[0].gap_ms, 75)

        happy_mean = statistics.mean(
            note.gate_ms + note.gap_ms for note in examples["happy"].notes
        )
        sad_mean = statistics.mean(
            note.gate_ms + note.gap_ms for note in examples["sad"].notes
        )
        self.assertLess(happy_mean, sad_mean)

    def test_long_sad_gates_are_not_silently_truncated(self) -> None:
        self.assertEqual(MG.PIEZO_WATCHDOG_MS, 750)
        # The generator no longer emits gates past the watchdog now that every
        # mood is chirp-fast, but it must never silently CLAMP one either — a
        # pile-up sitting exactly on the limit would mean truncation had been
        # hidden in the generator instead of handled by the player's refresh.
        gates = [
            note.gate_ms
            for mood in MOODS
            for seed in range(300)
            for note in self.phrases[mood, seed].notes
        ]
        self.assertEqual(
            [gate for gate in gates if gate == MG.PIEZO_WATCHDOG_MS], []
        )
        # Long gates remain legal and are reported honestly if they occur;
        # melody_player covers the refresh path directly.
        self.assertTrue(all(gate >= MP.MIN_NOTE_MS for gate in gates))

    def test_invalid_mood_raises_a_clear_error(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown mood"):
            MG.generate("excited", 1)


class PhraseTests(unittest.TestCase):
    def test_signature_is_transposition_and_tempo_invariant(self) -> None:
        first_midi = (91, 95, 93)
        second_midi = tuple(note + 1 for note in first_midi)
        first_notes = (
            MG.Note(MP.quantize_hz(91), 80, 120),
            MG.Note(MP.quantize_hz(95), 100, 100),
            MG.Note(MP.quantize_hz(93), 160, 0),
        )
        second_notes = (
            MG.Note(MP.quantize_hz(92), 160, 240),
            MG.Note(MP.quantize_hz(96), 200, 200),
            MG.Note(MP.quantize_hz(94), 320, 0),
        )
        first = MG.Phrase(
            mood="happy",
            seed=1,
            notes=first_notes,
            midi_notes=first_midi,
        )
        second = MG.Phrase(
            mood="happy",
            seed=2,
            notes=second_notes,
            midi_notes=second_midi,
        )
        self.assertEqual(first.signature(), second.signature())

        quantized_rescale = MG.Phrase(
            mood="happy",
            seed=3,
            notes=(
                MG.Note(MP.quantize_hz(92), 91, 137),
                MG.Note(MP.quantize_hz(96), 114, 114),
                MG.Note(MP.quantize_hz(94), 183, 0),
            ),
            midi_notes=second_midi,
        )
        self.assertEqual(first.signature(), quantized_rescale.signature())

    def test_history_rejects_a_recent_repeat(self) -> None:
        history = MR.PhraseHistory(capacity=16)
        first = MG.generate("happy", 424242, history)
        second = MG.generate("happy", 424242, history)

        self.assertNotEqual(first.signature(), second.signature())
        self.assertNotEqual(first.seed, second.seed)
        self.assertTrue(history.seen(first.signature()))
        self.assertTrue(history.seen(second.signature()))

    def test_total_ms_includes_gates_and_gaps(self) -> None:
        phrase = MG.Phrase(
            mood="happy",
            seed=1,
            notes=(
                MG.Note(1568, 80, 120),
                MG.Note(1976, 100, 0),
            ),
        )
        self.assertEqual(phrase.total_ms(), 300)


if __name__ == "__main__":
    unittest.main()
