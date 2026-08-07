"""Pure, deterministic mood-melody generation for the desktop robot.

The generator deliberately separates four concerns:

* a contour supplies the phrase-scale shape;
* a mean-reverting constrained walk supplies local notes;
* backward reachability keeps every intermediate choice compatible with the
  preselected ending;
* rhythm is generated independently, because tempo and articulation carry
  more emotional weight than pitch height on this piezo.

No serial, timing, or command-line work belongs here.  In particular, gates
longer than the firmware watchdog are represented honestly so a player can
refresh the tone instead of silently shortening the music.
"""

from __future__ import annotations

import random
from dataclasses import dataclass
from functools import lru_cache
from typing import Iterable

try:
    from .melody_pitch import (
        BAND_WORKING,
        MIN_NOTE_MS,
        MIN_REARTICULATION_GAP_MS,
        TONIC_MIDI,
        hz_to_midi,
        quantize_hz,
    )
    from .melody_rng import (
        PhraseHistory,
        new_seed,
        rng_for,
        suppress,
        triangular_weights,
        weighted_choice,
    )
except ImportError:
    try:
        from tools.melody_pitch import (
            BAND_WORKING,
            MIN_NOTE_MS,
            MIN_REARTICULATION_GAP_MS,
            TONIC_MIDI,
            hz_to_midi,
            quantize_hz,
        )
        from tools.melody_rng import (
            PhraseHistory,
            new_seed,
            rng_for,
            suppress,
            triangular_weights,
            weighted_choice,
        )
    except ImportError:
        from melody_pitch import (  # type: ignore[no-redef]
            BAND_WORKING,
            MIN_NOTE_MS,
            MIN_REARTICULATION_GAP_MS,
            TONIC_MIDI,
            hz_to_midi,
            quantize_hz,
        )
        from melody_rng import (  # type: ignore[no-redef]
            PhraseHistory,
            new_seed,
            rng_for,
            suppress,
            triangular_weights,
            weighted_choice,
        )


PIEZO_WATCHDOG_MS = 750
"""Maximum unrefreshed tone duration in ``src/piezo_tone.cpp``."""

TONE_WATCHDOG_MS = PIEZO_WATCHDOG_MS
"""Player-facing alias for the firmware watchdog."""

FIRMWARE_WATCHDOG_MS = PIEZO_WATCHDOG_MS
"""Explicit firmware-facing alias used by transport implementations."""

TONE_REFRESH_MS = 250
"""Safe refresh cadence for a player emitting a long :class:`Note` gate."""

LENGTH_BANDS = ((2, 5), (3, 9), (4, 16))
HISTORY_RETRIES = 8
RESOLUTION_START = 0.65
POST_SKIP_SEMITONES = 5

MAJOR_MASK = (0, 2, 4, 5, 7, 9, 11)
MINOR_MASK = (0, 2, 3, 5, 7, 8, 10)

# Confusion was whole-tone, chosen because that scale cannot resolve. But
# whole-tone contains NO SEMITONES — every step is a whole tone, which is
# inherently smooth. That is why it kept reading as dreamy or curious rather
# than frustrated: "cannot resolve" and "sour" are different properties, and
# conflating them cost several bench rounds.
#
# Bench 2026-08-04, Nick: "sour note combos give confused and frustrated".
# Sourness needs the minor second — the most dissonant melodic interval — so
# the mask goes fully chromatic and the step weighting supplies the clash.
CONFUSED_MASK = tuple(range(12))

_VALID_MOODS = ("happy", "sad", "confused")
_MODE_THIRD = {"happy": 4, "sad": 3}
_SCALE_NAMES = {
    "happy": "ionian",
    "sad": "aeolian",
    "confused": "whole-tone-fragment",
}
_SCALE_MASKS = {
    "happy": MAJOR_MASK,
    "sad": MINOR_MASK,
    "confused": CONFUSED_MASK,
}
_TERMINAL_OPTIONS = {
    "happy": (("tonic", 0.5), ("third", 0.3), ("fifth", 0.2)),
    # Sad ends on the tonic only. The minor third sits ABOVE the tonic, so a
    # third-ending lifts the phrase at the exact moment it should settle —
    # bench 2026-08-04 produced 1568 -> 1865 Hz, rising into the final note.
    # Sadness needs the line to come to rest, not to lift.
    "sad": (("tonic", 1.0),),
}
_FUNCTION_OFFSETS = {
    "tonic": 0,
    "fifth": 7,
}


@dataclass(frozen=True)
class Note:
    """One already-quantized piezo note."""

    hz: int
    gate_ms: int
    gap_ms: int


@dataclass(frozen=True)
class Phrase:
    """A generated phrase plus the decisions needed to explain and replay it."""

    mood: str
    seed: int
    notes: tuple[Note, ...]
    scale: str = ""
    scale_mask: tuple[int, ...] = ()
    terminal_function: str = ""
    terminal_midi: int | None = None
    length_band: tuple[int, int] | None = None
    midi_notes: tuple[int, ...] = ()
    # How many leading notes form the body, as opposed to the closing gesture.
    # Confusion's tail is one or two notes depending on the gesture chosen, so
    # callers and tests cannot infer the split from the note list alone.
    body_length: int = 0

    def total_ms(self) -> int:
        """Total sounding and inter-note silence represented by the phrase."""
        return sum(note.gate_ms + note.gap_ms for note in self.notes)

    def signature(self) -> tuple:
        """Transposition- and tempo-invariant key for recent-phrase rejection."""
        midi_notes = self.midi_notes
        if len(midi_notes) != len(self.notes):
            midi_notes = tuple(
                int(round(hz_to_midi(note.hz))) for note in self.notes
            )
        intervals = tuple(
            right - left for left, right in zip(midi_notes, midi_notes[1:])
        )

        duration_total = sum(
            note.gate_ms + note.gap_ms for note in self.notes
        )
        if duration_total <= 0:
            rhythm = tuple((0, 0) for _ in self.notes)
        else:
            # Millisecond timing cannot preserve an arbitrary tempo scale
            # exactly. Percentage bins absorb that quantization noise while
            # retaining the phrase's gate/gap pattern for deduplication.
            rhythm = tuple(
                (
                    round(note.gate_ms * 100 / duration_total),
                    round(note.gap_ms * 100 / duration_total),
                )
                for note in self.notes
            )
        return intervals, rhythm


@dataclass(frozen=True)
class _WalkRules:
    step_max: int
    leap_min: int | None
    leap_max: int
    max_leaps: int
    window_width: float
    theta: float
    ewma_alpha: float


_WALK_RULES = {
    "happy": _WalkRules(
        step_max=4,
        leap_min=5,
        leap_max=7,
        max_leaps=1,
        window_width=6.0,
        theta=0.62,
        ewma_alpha=0.48,
    ),
    "sad": _WalkRules(
        step_max=3,
        leap_min=4,
        leap_max=5,
        max_leaps=1,
        # Tighter window and stronger pull toward the descending contour.
        # Bench 2026-08-04: a wandering minor line in a high register was
        # heard as "haunted house, more scary than sad". Wandering reads as
        # eerie; a decisive fall reads as a sigh.
        window_width=3.0,
        theta=0.78,
        ewma_alpha=0.30,
    ),
}

# Sad has a length ceiling for a different reason than confused: sustained
# notes make it slow, so a long draw becomes a 6+ second dirge that outstays
# its welcome on a desk robot. Nick called 4.5 s "too long".
SAD_MAX_LENGTH = 6


def generate(
    mood: str,
    seed: int | None = None,
    history: PhraseHistory | None = None,
) -> Phrase:
    """Generate one phrase, optionally avoiding recently heard signatures.

    A retry uses a derived 128-bit seed and stores that actual seed on the
    returned phrase, so even a history-induced alternative remains replayable.
    """
    if mood not in _VALID_MOODS:
        choices = ", ".join(_VALID_MOODS)
        raise ValueError(f"unknown mood {mood!r}; expected one of: {choices}")

    requested_seed = new_seed() if seed is None else seed
    retry_rng = rng_for(requested_seed)
    last_phrase: Phrase | None = None

    for attempt in range(HISTORY_RETRIES + 1):
        attempt_seed = (
            requested_seed if attempt == 0 else retry_rng.getrandbits(128)
        )
        phrase = _generate_once(mood, attempt_seed)
        last_phrase = phrase
        if history is None or not history.seen(phrase.signature()):
            if history is not None:
                history.remember(phrase.signature())
            return phrase

    # A tiny history capacity or a two-note gesture can exhaust all practical
    # alternatives. Returning the final bounded attempt is preferable to an
    # unbounded novelty loop.
    assert last_phrase is not None
    history.remember(last_phrase.signature())
    return last_phrase


def _generate_once(mood: str, seed: int) -> Phrase:
    rng = rng_for(seed)

    band = LENGTH_BANDS[rng.randrange(len(LENGTH_BANDS))]
    length = rng.randint(band[0], band[1])

    if mood == "confused":
        # Confusion does not use the tonic/third/fifth terminal machinery at
        # all. Its ending is a *relative* rise from the penultimate note, and
        # its rhythm carries a single hesitation. Those two cues are the whole
        # emotion, so it gets a dedicated path rather than being bent through
        # the tonal walk used by happy and sad.
        #
        # Confusion has a genuine length ceiling, and it is body compactness
        # rather than band headroom. Research puts a confused body at 5-8
        # semitones — five whole-tone pitches — because wider reads as
        # agitation. Many notes inside a compact span must therefore oscillate,
        # which is the every-note zigzag we are trying to avoid. Long draws are
        # remapped across 5-9 rather than collapsed onto one value, so the
        # length histogram stays smooth.
        if length > CONFUSED_MAX_LENGTH:
            length = rng.randint(5, CONFUSED_MAX_LENGTH)
        return _confused_phrase(seed, band, length, rng)

    if mood == "sad" and length > SAD_MAX_LENGTH:
        # Slow notes make a long sad phrase drag; remapped across a range
        # rather than pinned to one value so the length stays varied.
        length = rng.randint(3, SAD_MAX_LENGTH)

    if length == 2:
        midi_notes, terminal_function, terminal_midi = _two_note_gesture(
            mood
        )
        scale_mask = _SCALE_MASKS[mood]
    else:
        terminal_function = _choose_terminal_function(mood, rng)
        terminal_midi = _terminal_midi(mood, terminal_function)
        (
            midi_notes,
            scale_mask,
        ) = _generate_pitch_walk(
            mood=mood,
            length=length,
            terminal_midi=terminal_midi,
            rng=rng,
        )

    notes, midi_notes = _make_rhythm(mood, midi_notes, rng)
    return Phrase(
        mood=mood,
        seed=seed,
        notes=notes,
        scale=_SCALE_NAMES[mood],
        scale_mask=tuple(scale_mask),
        terminal_function=terminal_function,
        terminal_midi=terminal_midi,
        length_band=band,
        midi_notes=midi_notes,
    )


CONFUSED_ANCHOR_LOW_MIDI = 91       # G6, ~1568 Hz
CONFUSED_ANCHOR_HIGH_MIDI = 97      # C#7, ~2217 Hz
CONFUSED_RISE_CHOICES = (3, 4, 5)   # terminal "please clarify?"

# Confusion had exactly one closing gesture — a rising question — which made
# 600 phrases collapse onto 57 shapes with 77% sharing the same +3 ending.
# Worse, that gesture is only EMPIRICAL IN SPEECH (repair interjections such
# as "Huh?"); applying it to buzzer tones was always an inference.
#
# Bench 2026-08-04 then showed the replacements had their own bias: falter and
# trail both END DOWNWARD, and a descending ending is the sadness cue, so
# confusion drifted toward sad. Meanwhile rise read as merely curious. Nick
# asked for something more frustrated.
#
# Frustration is HIGH-arousal negative, where sadness is LOW-arousal negative,
# so it needs different cues: faster, tighter, and above all INSISTENT — the
# sound of repeating yourself because nothing answered. The research warned
# that repeated attacks and agitated oscillation read as urgency and told us
# to avoid them for puzzlement; for frustration they are exactly right.
#
#   insist  repeat the pitch, then bite off a semitone  -> "hello? HELLO."
#   rise    a lift of +3..+5, held                      -> "huh?"
#   falter  a small dip then a lift                     -> "wait... what?"
#   trail   a short, abrupt, low ending                 -> "hmm..." tailing off
#
# Weighted toward insist and away from trail, which was the most sad-leaning.
CONFUSED_ENDINGS = (
    ("insist", 0.4),
    ("falter", 0.25),
    ("rise", 0.25),
    ("trail", 0.1),
)
CONFUSED_BODY_GATE_MS = (90, 150)
CONFUSED_GAP_MS = (35, 75)
CONFUSED_HESITATION_MS = (180, 260)
CONFUSED_FINAL_GATE_MS = (220, 300)
CONFUSED_MAX_LEAP = 7
TRITONE_SEMITONES = 6

# Each tuple describes the whole-tone body relative to the anchor. The final
# +3..+5 rise is appended separately.
#
# CRITICAL: no body may ascend monotonically. The terminal rise already climbs,
# so a rising body turns the whole phrase into one long ascent — which reads as
# a fanfare, not a question. (Bench test 2026-08-04: the length-4 body (0,2,4)
# plus a rise was heard as "the Star Spangled Banner".) Every body therefore
# turns or descends before handing over to the rise, so the rise stands out as
# a question rather than as the top of a ramp.
_CONFUSED_BODY_TEMPLATES = {
    2: ((0,),),
    # Every body from three notes up must END below its own peak by enough
    # that the terminal lift lands UNDER that peak — otherwise the rise becomes
    # the phrase's highest note and reads as victory rather than as a question
    # (bench 2026-08-04). Steps stay at +/-2 or +/-4: a +/-6 move is a tritone,
    # which reads as menacing.
    3: ((0, -4),),
    4: ((0, 2, -2), (0, -2, -4)),
    5: ((0, 2, 4, 0), (0, 2, 0, -2)),
    6: ((0, 2, 4, 2, 0), (0, 2, 4, 0, -2)),
    7: ((0, 2, 4, 2, 0, -2), (0, 2, 4, 6, 4, 0)),
}

# A compact span keeps the wander searching rather than leaping about, and
# must leave headroom for the mandatory terminal rise inside the one-octave
# band. {-2, 0, 2, 4} is four nearby whole-tone pitches, matching the research
# guidance to use only 3-4 members rather than the full scale.
# Research put a confused body at a compact 5-8 semitones — wide leaps read as
# agitation, not searching. Two octaves lets us use the top of that range
# instead of the four pitches the old one-octave band allowed.
_CONFUSED_SPAN = (-4, 4)
_CONFUSED_MAX_BODY_SPAN = 8

# Longest confused phrase that still fits a compact body with few reversals.
# The wider two-octave band raised this from 7; the limit is the researched
# body compactness, not the available range.
CONFUSED_MAX_LENGTH = 9

def _confused_body_offsets(
    body_length: int, rng: random.Random
) -> tuple[int, ...]:
    """Whole-tone body offsets of exactly ``body_length`` notes.

    Earlier this drew from one or two fixed contours per length, which made
    confusion far too predictable — 600 phrases produced only 57 distinct
    shapes. Bodies are now generated and filtered against the same rules, so
    the constraints shape the output without collapsing it onto a handful of
    templates.

    Constraints, each from a bench finding:
      * steps of +/-2 or +/-4 only — a +/-6 move is a tritone and reads as
        menacing rather than puzzled;
      * never monotonically ascending — with the closing gesture that becomes
        one long ramp and reads as a fanfare;
      * must end below its own peak, so a rising gesture stays a local lift
        rather than a triumphant climb;
      * compact span and few turns, since wide leaps and constant reversals
        read as agitation.
    """
    body_length = max(1, body_length)
    if body_length == 1:
        return (0,)

    for _ in range(60):
        offsets = [0]
        direction = 1 if rng.randrange(2) else -1
        for _ in range(body_length - 1):
            # Momentum: mostly continue in the current direction, occasionally
            # turn. Choosing each step's sign independently reverses about half
            # the time, which zigzags — that reads as agitation rather than
            # searching. Runs with occasional turns read as looking around.
            if rng.random() < 0.35:
                direction = -direction
            # Semitone steps are the sour ones and carry the frustration; whole
            # tones keep the line moving; the occasional minor third stops it
            # becoming a chromatic crawl.
            roll = rng.random()
            size = 1 if roll < 0.45 else (2 if roll < 0.85 else 3)
            offsets.append(offsets[-1] + direction * size)
        if max(offsets) - min(offsets) > _CONFUSED_MAX_BODY_SPAN:
            continue
        if all(
            offsets[i + 1] > offsets[i] for i in range(len(offsets) - 1)
        ):
            continue
        if max(offsets) - offsets[-1] < min(CONFUSED_RISE_CHOICES):
            continue
        # Reject a body that simply oscillates between two pitches; that reads
        # as a mechanical alternation rather than as searching.
        if len(set(offsets)) < min(3, body_length):
            continue
        # Require at least one semitone clash once there is room for it. The
        # sourness IS the frustration; a body that happens to draw only whole
        # tones slides back toward the smooth, dreamy reading.
        steps = [
            abs(offsets[i + 1] - offsets[i])
            for i in range(len(offsets) - 1)
        ]
        if len(offsets) >= 3 and 1 not in steps:
            continue
        # Reject an over-turning contour outright. Momentum makes this rare,
        # but a body that changes direction on most notes reads as agitation.
        if body_length >= 4:
            directions = [
                1 if offsets[i + 1] > offsets[i] else -1
                for i in range(len(offsets) - 1)
            ]
            turns = sum(
                a != b for a, b in zip(directions, directions[1:])
            )
            if turns > max(2, (len(directions) - 1) // 2):
                continue
        return tuple(offsets)

    # Deterministic fallback for the rare draw where 60 attempts all fail
    # (short bodies have few legal shapes). Built to the same rules rather than
    # reusing the old whole-tone templates, which contain +/-4 steps and would
    # violate the chromatic step limit.
    fallback = [0]
    if body_length >= 3:
        fallback.append(1)          # a semitone up, for the sour clash
    while len(fallback) < body_length:
        step = -1 if len(fallback) % 2 else -2
        fallback.append(fallback[-1] + step)
    # Guarantee the body ends far enough below its peak for the closing
    # gesture to stay underneath.
    shortfall = min(CONFUSED_RISE_CHOICES) - (max(fallback) - fallback[-1])
    if shortfall > 0:
        fallback[-1] -= shortfall
    return tuple(fallback[:body_length])


def _confused_phrase(
    seed: int, band: tuple[int, int], length: int, rng: random.Random
) -> Phrase:
    """Build confusion around one hesitation and an unresolved ending."""
    variants = []
    # The body is sized per gesture so body + tail always equals the drawn
    # length; a two-note falter would otherwise make the phrase one note
    # longer than requested and quietly break the length contract.
    bodies: dict[int, tuple[int, ...]] = {}
    for ending, _ in CONFUSED_ENDINGS:
        for tail in _confused_tail_offsets(ending, rng):
            body_length = length - len(tail)
            if body_length < 1:
                continue
            if body_length not in bodies:
                bodies[body_length] = _confused_body_offsets(
                    body_length, rng
                )
            offsets = bodies[body_length]
            for anchor in range(
                CONFUSED_ANCHOR_LOW_MIDI, CONFUSED_ANCHOR_HIGH_MIDI + 1
            ):
                body = tuple(anchor + offset for offset in offsets)
                if not all(BAND_WORKING.contains(note) for note in body):
                    continue
                if not all(
                    (note - TONIC_MIDI) % 12 in CONFUSED_MASK
                    for note in body
                ):
                    continue
                if max(body) - min(body) > _CONFUSED_MAX_BODY_SPAN:
                    continue
                final_notes = tuple(body[-1] + step for step in tail)
                if not all(
                    BAND_WORKING.contains(note) for note in final_notes
                ):
                    continue
                if final_notes[-1] == body[0]:
                    continue
                # Whatever the gesture, the phrase must not finish on its own
                # peak. Bench 2026-08-04: an ending that topped everything
                # already played was heard as "victory" — a triumph gesture,
                # where a question lifts from a settled position. Tying the
                # peak still reads conclusive, hence the strict test. Applied
                # even to single-note bodies, since a two-note tail can peak
                # at the end regardless of how short the body was.
                if max(final_notes) >= max(body):
                    continue
                variants.append((body, ending, final_notes))

    if not variants:
        # A two-note phrase has a one-note body, so there is no earlier peak to
        # stay under — it is the one shape allowed to end on its highest note.
        offsets = _confused_body_offsets(max(1, length - 1), rng)
        for anchor in range(
            CONFUSED_ANCHOR_LOW_MIDI, CONFUSED_ANCHOR_HIGH_MIDI + 1
        ):
            body = tuple(anchor + offset for offset in offsets)
            if not all(BAND_WORKING.contains(note) for note in body):
                continue
            for rise in CONFUSED_RISE_CHOICES:
                final = body[-1] + rise
                if BAND_WORKING.contains(final) and final != body[0]:
                    variants.append((body, "rise", (final,)))

    if not variants:
        raise RuntimeError(
            f"no confused body can realize a {length}-note ending"
        )

    # Pick the GESTURE by its intended weight first, then a variant within it.
    # Choosing uniformly across all variants silently weighted by how many tail
    # shapes each gesture happens to define, which buried the frustrated
    # "insist" ending under the ones with more spellings.
    gesture_weights = dict(CONFUSED_ENDINGS)
    available = sorted({ending for _, ending, _ in variants})
    chosen_index = weighted_choice(
        rng,
        list(range(len(available))),
        [gesture_weights.get(name, 0.0) or 1e-6 for name in available],
    )
    gesture = available[chosen_index]
    matching = [v for v in variants if v[1] == gesture]
    body, ending, final_notes = matching[rng.randrange(len(matching))]
    midi_notes = body + final_notes
    used_classes = tuple(
        sorted({(note - TONIC_MIDI) % 12 for note in midi_notes})
    )
    return Phrase(
        mood="confused",
        seed=seed,
        notes=_confused_rhythm(midi_notes, rng, ending=ending),
        scale=_SCALE_NAMES["confused"],
        scale_mask=used_classes,
        terminal_function=ending,
        terminal_midi=midi_notes[-1],
        length_band=band,
        midi_notes=midi_notes,
        body_length=len(body),
    )


def _confused_tail_offsets(
    ending: str, rng: random.Random
) -> tuple[tuple[int, ...], ...]:
    """Sample candidate closing gestures, as offsets from the last body note.

    These used to be a hardcoded list, which produced only 16 distinct endings
    across 600 phrases — the bench verdict was that the endings were "too
    similar". They are sampled from parameter ranges now, so each gesture keeps
    its character while its exact shape varies.
    """
    candidates: set[tuple[int, ...]] = set()
    if ending == "insist":
        # Repeat the pitch, then bite off a small interval. Repetition is the
        # insistence — the sound of asking again — and a semitone bite is
        # sourer than the tone around it. This is the frustrated reading.
        for _ in range(10):
            repeats = rng.choice((1, 1, 2))
            drop = rng.choice((1, 1, 2, 3))
            candidates.add(tuple([0] * repeats + [-drop]))
    elif ending == "rise":
        for _ in range(10):
            candidates.add((rng.randint(2, 5),))
    elif ending == "falter":
        # Dip, then lift back part-way: the "wait... what?" double gesture.
        for _ in range(12):
            dip = rng.randint(1, 4)
            lift = rng.randint(2, 5)
            candidates.add((-dip, -dip + lift))
    else:
        # trail: settle lower and stop short, tailing off rather than asking.
        # Unresolved by omission instead of by inflection.
        for _ in range(10):
            first = rng.randint(1, 5)
            if rng.random() < 0.4:
                candidates.add((-first, -first - rng.randint(1, 3)))
            else:
                candidates.add((-first,))
    return tuple(sorted(candidates))


def _confused_rhythm(
    midi_notes: tuple[int, ...],
    rng: random.Random,
    *,
    ending: str = "rise",
) -> tuple[Note, ...]:
    """Keep a normal pace except for one deliberate hesitation."""
    count = len(midi_notes)
    hesitation_index = (
        None
        if count == 2
        else 0
        if count == 3
        else rng.randint(1, count - 3)
    )

    notes = []
    for index, midi_note in enumerate(midi_notes):
        if index == count - 1:
            # The closing gate depends on the gesture, but is drawn from a wide
            # continuous range rather than a fixed value — the bench verdict
            # was that endings sounded "too similar", and near-identical final
            # durations were half the reason.
            if ending == "trail":
                gate_ms = rng.randint(55, 140)
            elif ending == "insist":
                # Bitten off short: holding it would sound resigned, not
                # annoyed.
                gate_ms = rng.randint(50, 120)
            elif ending == "falter":
                gate_ms = rng.randint(140, 320)
            else:  # rise — held so the question hangs in the air
                gate_ms = rng.randint(190, 340)
            gap_ms = 0
        else:
            gate_range = (
                (90, 110)
                if index == hesitation_index
                else CONFUSED_BODY_GATE_MS
            )
            gate_ms = rng.randint(*gate_range)
            gap_ms = (
                rng.randint(*CONFUSED_HESITATION_MS)
                if index == hesitation_index
                else rng.randint(*CONFUSED_GAP_MS)
            )
        # The insist gesture repeats a pitch deliberately, so it needs the
        # re-articulation gap or the two strikes fuse into one held note and
        # the insistence disappears.
        if index < count - 1 and midi_note == midi_notes[index + 1]:
            gap_ms = max(gap_ms, MIN_REARTICULATION_GAP_MS)
        notes.append(
            Note(
                hz=quantize_hz(midi_note),
                gate_ms=gate_ms,
                gap_ms=gap_ms,
            )
        )
    return tuple(notes)


def _choose_terminal_function(mood: str, rng: random.Random) -> str:
    options = _TERMINAL_OPTIONS[mood]
    selected = weighted_choice(
        rng, list(range(len(options))), [weight for _, weight in options]
    )
    return options[selected][0]


def _terminal_offset(mood: str, function: str) -> int:
    if function == "third":
        return _MODE_THIRD[mood]
    return _FUNCTION_OFFSETS[function]


def _terminal_midi(mood: str, function: str) -> int:
    offset = _terminal_offset(mood, function)
    if function == "tonic" and mood == "happy":
        return TONIC_MIDI + 12
    return TONIC_MIDI + offset


def _two_note_gesture(mood: str) -> tuple[tuple[int, int], str, int]:
    if mood == "happy":
        terminal = TONIC_MIDI + _MODE_THIRD["happy"]
        return (TONIC_MIDI, terminal), "third", terminal
    terminal = TONIC_MIDI
    return (
        TONIC_MIDI + _MODE_THIRD["sad"],
        terminal,
    ), "tonic", terminal


def _generate_pitch_walk(
    mood: str,
    length: int,
    terminal_midi: int,
    rng: random.Random,
) -> tuple[tuple[int, ...], tuple[int, ...]]:
    rules = _WALK_RULES[mood]
    scale_mask, allowed_pitches, foreign_classes = _pitch_material(
        mood, terminal_midi, rng
    )
    foreign_limit = length
    deadline = min(3, length - 1)
    required = tuple(sorted((0, _MODE_THIRD[mood])))

    def legal_options(
        current: int | None,
        index: int,
        missing: tuple[int, ...],
        leaps_used: int,
        foreign_used: int,
    ) -> list[tuple[int, tuple[int, ...], int, int]]:
        if index >= length:
            return []

        pool: Iterable[int]
        if index == length - 1:
            pool = (terminal_midi,)
        else:
            pool = allowed_pitches

        options = []
        for candidate in pool:
            pitch_class = (candidate - TONIC_MIDI) % 12

            next_leaps = leaps_used
            if current is not None:
                distance = abs(candidate - current)
                if distance > rules.step_max:
                    if (
                        rules.leap_min is None
                        or distance < rules.leap_min
                        or distance > rules.leap_max
                        or leaps_used >= rules.max_leaps
                    ):
                        continue
                    next_leaps += 1

            next_foreign = foreign_used + int(
                pitch_class in foreign_classes
            )
            if next_foreign > foreign_limit:
                continue

            next_missing = tuple(
                item for item in missing if item != pitch_class
            )
            if index >= deadline and next_missing:
                continue

            options.append(
                (candidate, next_missing, next_leaps, next_foreign)
            )
        return options

    @lru_cache(maxsize=None)
    def can_finish(
        current: int | None,
        index: int,
        missing: tuple[int, ...],
        leaps_used: int,
        foreign_used: int,
    ) -> bool:
        if index == length:
            return current == terminal_midi and not missing
        for candidate, next_missing, next_leaps, next_foreign in legal_options(
            current, index, missing, leaps_used, foreign_used
        ):
            if can_finish(
                candidate,
                index + 1,
                next_missing,
                next_leaps,
                next_foreign,
            ):
                return True
        return False

    if not can_finish(None, 0, required, 0, 0):
        raise RuntimeError(
            f"no reachable {mood} phrase of length {length} can end at "
            f"MIDI {terminal_midi}"
        )

    chosen: list[int] = []
    current: int | None = None
    missing = required
    leaps_used = 0
    foreign_used = 0
    history_centre: float | None = None

    for index in range(length):
        viable = [
            option
            for option in legal_options(
                current, index, missing, leaps_used, foreign_used
            )
            if can_finish(
                option[0],
                index + 1,
                option[1],
                option[2],
                option[3],
            )
        ]
        if not viable:
            raise RuntimeError("backward reachability lost all legal moves")

        candidates = [option[0] for option in viable]
        progress = index / (length - 1)
        macro_target = _macro_target(mood, progress, terminal_midi)
        if history_centre is None:
            history_centre = macro_target

        # Reverting toward a moving contour target preserves macro shape.  A
        # fixed tonic centre would erase the late happy peak and early sad one.
        window_centre = history_centre + rules.theta * (
            macro_target - history_centre
        )
        half_width = _window_width(rules.window_width, progress)
        weights = triangular_weights(
            candidates, centre=window_centre, half_width=half_width
        )
        if sum(weights) <= 0.0:
            # Reachability can occasionally leave every legal move outside the
            # local window. In that explicit conflict, steer toward the ending;
            # otherwise zero-weight notes stay filtered rather than revived.
            weights = [
                1.0 / (1.0 + abs(candidate - terminal_midi))
                for candidate in candidates
            ]
        weights = _shape_candidate_weights(
            mood=mood,
            candidates=candidates,
            weights=weights,
            chosen=chosen,
            missing=missing,
            deadline=deadline,
            index=index,
            terminal_midi=terminal_midi,
            progress=progress,
        )

        if current is not None:
            weights = suppress(
                candidates, weights, banned=(current,), factor=0.08
            )

        selected = weighted_choice(rng, candidates, weights)
        selected_option = next(
            option for option in viable if option[0] == selected
        )
        chosen.append(selected)
        current = selected
        missing = selected_option[1]
        leaps_used = selected_option[2]
        foreign_used = selected_option[3]
        history_centre = (
            selected
            if index == 0
            else (
                rules.ewma_alpha * selected
                + (1.0 - rules.ewma_alpha) * history_centre
            )
        )

    return tuple(chosen), tuple(scale_mask)


def _pitch_material(
    mood: str, terminal_midi: int, rng: random.Random
) -> tuple[tuple[int, ...], tuple[int, ...], frozenset[int]]:
    del terminal_midi, rng
    mask = _SCALE_MASKS[mood]
    allowed = tuple(
        note
        for note in range(
            BAND_WORKING.low_midi, BAND_WORKING.high_midi + 1
        )
        if (note - TONIC_MIDI) % 12 in mask
    )
    return mask, allowed, frozenset()


def _shape_candidate_weights(
    mood: str,
    candidates: list[int],
    weights: list[float],
    chosen: list[int],
    missing: tuple[int, ...],
    deadline: int,
    index: int,
    terminal_midi: int,
    progress: float,
) -> list[float]:
    shaped = list(weights)

    for position, candidate in enumerate(candidates):
        if chosen:
            distance = abs(candidate - chosen[-1])
            if mood == "happy":
                move_factor = (
                    0.70
                    if distance == 0
                    else 1.35
                    if distance <= 2
                    else 1.0
                    if distance <= 4
                    else 0.26
                )
            elif mood == "sad":
                move_factor = (
                    0.72
                    if distance == 0
                    else 1.45
                    if distance <= 2
                    else 1.0
                    if distance <= 3
                    else 0.14
                )
            shaped[position] *= move_factor

        pitch_class = (candidate - TONIC_MIDI) % 12
        if index <= deadline and pitch_class in missing:
            shaped[position] *= 2.4

    _apply_reversal_bias(mood, candidates, shaped, chosen)

    if chosen and progress >= RESOLUTION_START:
        resolution = (progress - RESOLUTION_START) / (
            1.0 - RESOLUTION_START
        )
        old_distance = abs(chosen[-1] - terminal_midi)
        for position, candidate in enumerate(candidates):
            new_distance = abs(candidate - terminal_midi)
            if new_distance < old_distance:
                shaped[position] *= 1.0 + 2.2 * resolution
            elif new_distance > old_distance:
                shaped[position] *= max(0.35, 1.0 - 0.55 * resolution)

    return shaped


def _apply_reversal_bias(
    mood: str,
    candidates: list[int],
    weights: list[float],
    chosen: list[int],
) -> None:
    if len(chosen) < 2:
        return
    last_interval = chosen[-1] - chosen[-2]
    if last_interval == 0:
        return

    direction = 1 if last_interval > 0 else -1
    available_directions = {
        1 if candidate > chosen[-1] else -1
        for candidate in candidates
        if candidate != chosen[-1]
    }

    # Only bias a reversal when continuation was also genuinely available.
    # Near a band edge the candidate filter already points inward, so adding a
    # second "reversal" bonus there would double-count the boundary.
    if not ({direction, -direction} <= available_directions):
        return

    if abs(last_interval) < POST_SKIP_SEMITONES:
        return
    reverse_factor, continue_factor = 2.25, 0.62

    for position, candidate in enumerate(candidates):
        move = candidate - chosen[-1]
        if move * direction < 0:
            weights[position] *= reverse_factor
        elif move * direction > 0:
            weights[position] *= continue_factor


def _window_width(base_width: float, progress: float) -> float:
    if progress < RESOLUTION_START:
        return base_width
    resolution = (progress - RESOLUTION_START) / (1.0 - RESOLUTION_START)
    return base_width + (1.35 - base_width) * resolution


def _macro_target(mood: str, progress: float, terminal_midi: int) -> float:
    target_offset = terminal_midi - TONIC_MIDI
    if mood == "happy":
        points = (
            (0.0, 2.0),
            (0.28, 1.0),
            (0.72, 10.0),
            (1.0, target_offset),
        )
    elif mood == "sad":
        opening_offset = min(12.0, target_offset + 3.5)
        peak_offset = min(12.0, target_offset + 5.5)
        falling_offset = min(12.0, target_offset + 2.5)
        points = (
            (0.0, opening_offset),
            (0.24, peak_offset),
            (0.62, falling_offset),
            (1.0, target_offset),
        )
    offset = _interpolate(points, progress)
    if progress >= RESOLUTION_START:
        resolution = (progress - RESOLUTION_START) / (
            1.0 - RESOLUTION_START
        )
        offset = (1.0 - 0.65 * resolution) * offset + (
            0.65 * resolution * target_offset
        )
    return TONIC_MIDI + offset


def _interpolate(
    points: tuple[tuple[float, float], ...], progress: float
) -> float:
    for (left_x, left_y), (right_x, right_y) in zip(points, points[1:]):
        if progress <= right_x:
            span = right_x - left_x
            fraction = 0.0 if span == 0.0 else (progress - left_x) / span
            return left_y + fraction * (right_y - left_y)
    return points[-1][1]


def _make_rhythm(
    mood: str, midi_notes: tuple[int, ...], rng: random.Random
) -> tuple[tuple[Note, ...], tuple[int, ...]]:
    count = len(midi_notes)
    iois: list[int] = []
    gates: list[int] = []

    # BENCH-DRIVEN OVERRIDE of the research tempo values.
    #
    # The literature says slow tempo is the strongest sadness cue, but that
    # evidence comes from instruments with timbre and natural decay. On a bare
    # square wave a 700 ms note rings like a theremin, and the bench verdict
    # was "haunted house, more scary than sad". Slow simply is not available to
    # us as a sad cue.
    #
    # So every mood now lives in the fast, chirp-like register that the two
    # working moods already occupied, and sadness is carried by DESCENDING
    # CONTOUR and MINOR INTERVALS instead. Sad stays relatively the slowest to
    # keep an ordering, but is quick enough to read as a chirp rather than a
    # drone.
    ioi_low, ioi_high = (95, 155) if mood == "happy" else (170, 260)
    # Gate ratio stays clipped so notes never sustain into drone territory,
    # while remaining above the ~45 ms floor where notes click instead of
    # sounding tonal.
    ratio_low, ratio_high = (
        (0.40, 0.62) if mood == "happy" else (0.50, 0.68)
    )
    for _ in midi_notes:
        ioi = rng.randint(ioi_low, ioi_high)
        ratio = rng.uniform(ratio_low, ratio_high)
        iois.append(ioi)
        gates.append(max(MIN_NOTE_MS, round(ioi * ratio)))

    if mood == "happy":
        gates[-1] = round(gates[-1] * rng.uniform(1.5, 2.0))
    else:
        gates[-1] = round(gates[-1] * 1.5)

    notes = []
    for index, (midi_note, gate_ms, ioi_ms) in enumerate(
        zip(midi_notes, gates, iois)
    ):
        gap_ms = 0 if index == count - 1 else max(0, ioi_ms - gate_ms)
        if (
            index < count - 1
            and midi_note == midi_notes[index + 1]
        ):
            gap_ms = max(gap_ms, MIN_REARTICULATION_GAP_MS)
        notes.append(
            Note(
                hz=quantize_hz(midi_note),
                gate_ms=gate_ms,
                gap_ms=gap_ms,
            )
        )
    return _merge_repeated_final_note(tuple(notes), midi_notes)


def _merge_repeated_final_note(
    notes: tuple[Note, ...], midi_notes: tuple[int, ...]
) -> tuple[tuple[Note, ...], tuple[int, ...]]:
    """Hold a repeated closing pitch instead of striking it twice.

    Bench 2026-08-04: a phrase ending on the tonic struck twice was heard as a
    stutter rather than a resolution — Nick asked for "2 beats continuous"
    instead. Sustaining the final pitch lets the line come to rest, which is
    what a closing gesture should do.

    ``midi_notes`` is collapsed alongside ``notes`` so the reported pitch list
    always matches what is actually played; letting them drift apart would make
    every downstream length and contour check quietly wrong.
    """
    if len(notes) < 2:
        return notes, midi_notes
    merged = list(notes)
    merged_midi = list(midi_notes)
    # Loop: three identical closing pitches would otherwise collapse to two
    # and still stutter.
    while len(merged) >= 2 and merged[-1].hz == merged[-2].hz:
        last = merged.pop()
        previous = merged.pop()
        merged.append(
            Note(
                hz=last.hz,
                gate_ms=previous.gate_ms + previous.gap_ms + last.gate_ms,
                gap_ms=last.gap_ms,
            )
        )
        if len(merged_midi) >= 2:
            merged_midi.pop()
    return tuple(merged), tuple(merged_midi)


__all__ = [
    "CONFUSED_MASK",
    "FIRMWARE_WATCHDOG_MS",
    "HISTORY_RETRIES",
    "LENGTH_BANDS",
    "MAJOR_MASK",
    "MINOR_MASK",
    "Note",
    "PIEZO_WATCHDOG_MS",
    "Phrase",
    "TONE_REFRESH_MS",
    "TONE_WATCHDOG_MS",
    "generate",
]
