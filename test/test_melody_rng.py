"""Tests for the seeded randomness layer."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from collections import Counter
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "melody_rng.py"
SPEC = importlib.util.spec_from_file_location("melody_rng", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
RNG = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RNG
SPEC.loader.exec_module(RNG)


class SeedTests(unittest.TestCase):
    def test_new_seed_is_in_range_and_varies(self) -> None:
        seeds = {RNG.new_seed() for _ in range(64)}
        # Collisions across 64 draws from 128 bits would indicate a broken
        # entropy source, not bad luck.
        self.assertEqual(len(seeds), 64)
        for seed in seeds:
            self.assertGreaterEqual(seed, 0)
            self.assertLess(seed, 1 << RNG.SEED_BITS)

    def test_seed_round_trips_through_hex(self) -> None:
        for seed in (0, 1, 2**127, RNG.new_seed(), RNG.new_seed()):
            with self.subTest(seed=seed):
                text = RNG.format_seed(seed)
                self.assertEqual(len(text), RNG.SEED_BITS // 4)
                self.assertEqual(RNG.parse_seed(text), seed)
                self.assertEqual(RNG.parse_seed("0x" + text), seed)

    def test_parse_seed_rejects_junk(self) -> None:
        for bad in ("", "   ", "zz", "12g4", "0x", "-1"):
            with self.subTest(value=bad):
                with self.assertRaises(ValueError):
                    RNG.parse_seed(bad)

    def test_parse_seed_rejects_oversized(self) -> None:
        with self.assertRaises(ValueError):
            RNG.parse_seed("f" * (RNG.SEED_BITS // 4 + 1))

    def test_same_seed_gives_identical_streams(self) -> None:
        seed = RNG.new_seed()
        a = [RNG.rng_for(seed).random() for _ in range(1)]
        first = RNG.rng_for(seed)
        second = RNG.rng_for(seed)
        self.assertEqual(
            [first.random() for _ in range(50)],
            [second.random() for _ in range(50)],
        )
        self.assertTrue(a)

    def test_different_seeds_diverge(self) -> None:
        first = RNG.rng_for(1).random()
        second = RNG.rng_for(2).random()
        self.assertNotEqual(first, second)


class WeightedChoiceTests(unittest.TestCase):
    def test_respects_weights_statistically(self) -> None:
        rng = RNG.rng_for(12345)
        counts = Counter(
            RNG.weighted_choice(rng, [10, 20], [0.9, 0.1])
            for _ in range(4000)
        )
        # Fixed seed, so this is deterministic and cannot flake. Generous
        # bounds because the point is "weights are honoured", not exactness.
        self.assertGreater(counts[10], counts[20] * 4)

    def test_zero_weight_candidate_is_never_chosen(self) -> None:
        rng = RNG.rng_for(7)
        picks = {
            RNG.weighted_choice(rng, [1, 2, 3], [1.0, 0.0, 1.0])
            for _ in range(500)
        }
        self.assertNotIn(2, picks)

    def test_rejects_degenerate_input(self) -> None:
        rng = RNG.rng_for(1)
        with self.assertRaises(ValueError):
            RNG.weighted_choice(rng, [], [])
        with self.assertRaises(ValueError):
            RNG.weighted_choice(rng, [1, 2], [1.0])
        with self.assertRaises(ValueError):
            RNG.weighted_choice(rng, [1, 2], [0.0, 0.0])
        with self.assertRaises(ValueError):
            RNG.weighted_choice(rng, [1, 2], [1.0, -1.0])


class TriangularWeightTests(unittest.TestCase):
    def test_peaks_at_centre_and_decays(self) -> None:
        candidates = [-3, -2, -1, 0, 1, 2, 3]
        weights = RNG.triangular_weights(candidates, centre=0.0, half_width=4.0)
        peak = weights[candidates.index(0)]
        self.assertEqual(peak, max(weights))
        for offset in (1, 2, 3):
            nearer = weights[candidates.index(offset - 1)]
            further = weights[candidates.index(offset)]
            self.assertGreater(nearer, further)

    def test_support_is_bounded_so_no_extra_clamp_is_needed(self) -> None:
        candidates = list(range(-10, 11))
        weights = RNG.triangular_weights(candidates, centre=0.0, half_width=4.0)
        for candidate, weight in zip(candidates, weights):
            if abs(candidate) >= 4:
                self.assertEqual(weight, 0.0)
            else:
                self.assertGreater(weight, 0.0)

    def test_small_moves_dominate_large_ones(self) -> None:
        candidates = list(range(-6, 7))
        weights = RNG.triangular_weights(candidates, centre=0.0, half_width=6.0)
        rng = RNG.rng_for(99)
        picks = [
            RNG.weighted_choice(rng, candidates, weights) for _ in range(3000)
        ]
        small = sum(1 for p in picks if abs(p) <= 2)
        large = sum(1 for p in picks if abs(p) >= 4)
        self.assertGreater(small, large)

    def test_rejects_non_positive_width(self) -> None:
        for bad in (0.0, -1.0):
            with self.subTest(value=bad):
                with self.assertRaises(ValueError):
                    RNG.triangular_weights([1, 2], centre=1.0, half_width=bad)


class SuppressionTests(unittest.TestCase):
    def test_banned_candidate_is_zeroed_by_default(self) -> None:
        damped = RNG.suppress([1, 2, 3], [1.0, 1.0, 1.0], banned=[2])
        self.assertEqual(damped, [1.0, 0.0, 1.0])

    def test_partial_factor_discourages_without_forbidding(self) -> None:
        damped = RNG.suppress([1, 2], [1.0, 1.0], banned=[2], factor=0.25)
        self.assertEqual(damped, [1.0, 0.25])

    def test_falls_back_when_everything_would_be_banned(self) -> None:
        # Rejection sampling would loop forever here; damping must not.
        original = [1.0, 1.0]
        damped = RNG.suppress([1, 2], original, banned=[1, 2])
        self.assertEqual(damped, original)

    def test_rejects_out_of_range_factor(self) -> None:
        for bad in (-0.1, 1.1):
            with self.subTest(value=bad):
                with self.assertRaises(ValueError):
                    RNG.suppress([1], [1.0], banned=[], factor=bad)


class PhraseHistoryTests(unittest.TestCase):
    def test_remembers_and_evicts_oldest(self) -> None:
        history = RNG.PhraseHistory(capacity=3)
        for key in ("a", "b", "c"):
            history.remember(key)
        self.assertTrue(all(history.seen(k) for k in "abc"))
        history.remember("d")
        self.assertFalse(history.seen("a"))
        self.assertTrue(history.seen("d"))
        self.assertEqual(len(history), 3)

    def test_reseeing_a_key_refreshes_its_position(self) -> None:
        history = RNG.PhraseHistory(capacity=3)
        for key in ("a", "b", "c"):
            history.remember(key)
        history.remember("a")     # 'b' is now the oldest, not 'a'
        history.remember("d")
        self.assertFalse(history.seen("b"))
        self.assertTrue(history.seen("a"))

    def test_zero_capacity_disables_the_guard(self) -> None:
        history = RNG.PhraseHistory(capacity=0)
        history.remember("a")
        self.assertFalse(history.seen("a"))
        self.assertEqual(len(history), 0)

    def test_rejects_negative_capacity(self) -> None:
        with self.assertRaises(ValueError):
            RNG.PhraseHistory(capacity=-1)


if __name__ == "__main__":
    unittest.main()
