"""Seeded randomness for the mood-melody generator.

Design follows the RNG research thread:

* A fresh 128-bit seed is drawn from ``secrets`` per phrase and logged as hex,
  so any phrase can be replayed exactly.
* All decisions within one phrase come from a dedicated ``random.Random``
  instance rather than the module-level ``random``, for isolation and thread
  safety.
* Cryptographic generator quality is deliberately *not* used for the phrase
  itself. It buys resistance to prediction, not musicality, and makes no
  perceptible difference across 2-16 notes. What actually matters is how the
  uniform source is shaped.

One honest caveat, worth stating plainly because it corrects a natural
assumption: no RNG can guarantee "a different phrase every single time".
Seeds can collide, and two different seeds can still produce the same short
phrase — a 13-pitch band admits only a few hundred distinct two-note phrases.
Genuine novelty requires remembering recent phrases and rejecting repeats,
which is what :class:`PhraseHistory` is for.
"""

from __future__ import annotations

import random
import secrets
from collections import deque
from typing import Hashable, Iterable, Sequence

SEED_BITS = 128


def new_seed() -> int:
    """Draw a fresh unpredictable seed."""
    return secrets.randbits(SEED_BITS)


def format_seed(seed: int) -> str:
    """Render a seed as fixed-width hex so logs line up and round-trip."""
    return f"{seed:0{SEED_BITS // 4}x}"


def parse_seed(text: str) -> int:
    """Accept a hex seed with or without an 0x prefix."""
    cleaned = text.strip().lower()
    if cleaned.startswith("0x"):
        cleaned = cleaned[2:]
    if not cleaned or any(c not in "0123456789abcdef" for c in cleaned):
        raise ValueError(f"seed must be hexadecimal, got {text!r}")
    value = int(cleaned, 16)
    if value.bit_length() > SEED_BITS:
        raise ValueError(f"seed must fit in {SEED_BITS} bits")
    return value


def rng_for(seed: int) -> random.Random:
    """A generator isolated to a single phrase."""
    return random.Random(seed)


def weighted_choice(
    rng: random.Random,
    candidates: Sequence[int],
    weights: Sequence[float],
) -> int:
    """Pick one candidate, honouring weights.

    Raises rather than silently biasing when the weights are unusable, since a
    degenerate weight vector means the caller's musical rules produced no legal
    move and that should surface as a bug, not as a wrong note.
    """
    if len(candidates) != len(weights):
        raise ValueError("candidates and weights must be the same length")
    if not candidates:
        raise ValueError("no candidates to choose from")
    total = 0.0
    for weight in weights:
        if weight < 0.0:
            raise ValueError("weights must be non-negative")
        total += weight
    if total <= 0.0:
        raise ValueError("weights sum to zero; no candidate is reachable")
    return rng.choices(list(candidates), weights=list(weights), k=1)[0]


def triangular_weights(
    candidates: Sequence[int], centre: float, half_width: float
) -> list[float]:
    """Tent-shaped weights peaking at ``centre`` and reaching zero at the edges.

    Chosen over a Gaussian because it has genuinely bounded support: a
    truncated normal still needs an explicit hard cap, whereas this cannot
    propose a candidate beyond ``half_width`` at all. Small moves therefore
    dominate and large leaps stay rare without a second clamping step.
    """
    if half_width <= 0.0:
        raise ValueError("half_width must be positive")
    weights = []
    for candidate in candidates:
        distance = abs(candidate - centre)
        weights.append(max(0.0, 1.0 - distance / half_width))
    return weights


def suppress(
    candidates: Sequence[int],
    weights: Sequence[float],
    banned: Iterable[int],
    factor: float = 0.0,
) -> list[float]:
    """Damp specific candidates, used to stop audible note repetition.

    Weight damping is preferred over rejection sampling because it is bounded:
    rejection can loop indefinitely when the musical rules leave only the
    banned candidate legal. A ``factor`` above zero discourages rather than
    forbids, which keeps a fallback path when everything else is masked out.
    """
    if not 0.0 <= factor <= 1.0:
        raise ValueError("factor must be within 0..1")
    banned_set = set(banned)
    damped = [
        weight * factor if candidate in banned_set else weight
        for candidate, weight in zip(candidates, weights)
    ]
    if sum(damped) <= 0.0:
        # Everything legal was banned. Fall back to the undamped weights
        # rather than failing outright — a repeated note beats no note.
        return list(weights)
    return damped


class PhraseHistory:
    """Remembers recent phrases so freshly generated ones can avoid repeats.

    This is the only mechanism that can actually deliver "different every
    time"; the RNG alone cannot, because the phrase space is small.
    """

    def __init__(self, capacity: int = 16) -> None:
        if capacity < 0:
            raise ValueError("capacity must be non-negative")
        self._capacity = capacity
        self._seen: deque[Hashable] = deque(maxlen=capacity or 1)

    @property
    def capacity(self) -> int:
        return self._capacity

    def __len__(self) -> int:
        return 0 if self._capacity == 0 else len(self._seen)

    def seen(self, key: Hashable) -> bool:
        return self._capacity > 0 and key in self._seen

    def remember(self, key: Hashable) -> None:
        if self._capacity == 0:
            return
        if key in self._seen:
            self._seen.remove(key)
        self._seen.append(key)
