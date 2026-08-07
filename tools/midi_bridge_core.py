"""Pure state engine for reducing MIDI note messages to piezo frequencies."""

from __future__ import annotations

import math
import time
from dataclasses import dataclass
from typing import Callable


_PITCH_CLASSES = 12
_A4_NOTE = 69
_A4_HZ = 440.0

# Mirrors the firmware `tone` command: 0 (stop) or 200..6000 Hz.
MIN_TONE_HZ = 200
MAX_TONE_HZ = 6000


def note_to_hz(note: float) -> float:
    """Equal-tempered frequency for a MIDI note number, A4 = 440 Hz."""
    return _A4_HZ * (2.0 ** ((note - _A4_NOTE) / _PITCH_CLASSES))


def _lowest_playable_note() -> int:
    return int(math.ceil(
        _A4_NOTE + _PITCH_CLASSES * math.log2(MIN_TONE_HZ / _A4_HZ)
    ))


def _highest_playable_note() -> int:
    return int(math.floor(
        _A4_NOTE + _PITCH_CLASSES * math.log2(MAX_TONE_HZ / _A4_HZ)
    ))


# MIDI 56 (G#3, ~207.7 Hz) through MIDI 114 (F#8, ~5919.9 Hz): roughly 4.8
# octaves the piezo can actually reach.
LOWEST_PLAYABLE_NOTE = _lowest_playable_note()
HIGHEST_PLAYABLE_NOTE = _highest_playable_note()

# Legacy single-octave behaviour: every pitch class collapsed onto A6..G#7.
# Loudest, but octaves are indistinguishable.
_FOLDED_LOW_NOTE = 93  # A6


def _folded_hz(note: int) -> int:
    folded_note = _FOLDED_LOW_NOTE + ((note - _FOLDED_LOW_NOTE) % _PITCH_CLASSES)
    return round(note_to_hz(folded_note))


def transpose_into_range(note: int) -> int:
    """Shift a note by whole octaves until the piezo can play it.

    Octave transposition preserves pitch class exactly, so a melody keeps its
    shape and — unlike folding — notes an octave apart stay an octave apart
    anywhere inside the playable span.
    """
    while note < LOWEST_PLAYABLE_NOTE:
        note += _PITCH_CLASSES
    while note > HIGHEST_PLAYABLE_NOTE:
        note -= _PITCH_CLASSES
    return note


def playable_hz(
    note: int, octave_shift: int = 0, fold_octaves: bool = False
) -> int:
    """Map a MIDI note to a frequency the firmware will accept."""
    if fold_octaves:
        return _folded_hz(note)
    shifted = transpose_into_range(note + octave_shift * _PITCH_CLASSES)
    hz = round(note_to_hz(shifted))
    return max(MIN_TONE_HZ, min(MAX_TONE_HZ, hz))


@dataclass
class _PressToken:
    note: int
    is_down: bool = True


class MidiNoteEngine:
    """Track MIDI note state and emit only changes to the desired wire tone.

    Notes use true last-press priority. Repeated note-on messages create
    separate tokens, and each release consumes the oldest still-down matching
    token. ``held_count`` counts physically down tokens, excluding notes that
    are only latched by sustain.
    """

    def __init__(
        self,
        min_gate_ms: float = 45,
        sustain_enabled: bool = True,
        clock_ns: Callable[[], int] | None = None,
        octave_shift: int = 0,
        fold_octaves: bool = False,
    ) -> None:
        if min_gate_ms < 0:
            raise ValueError("min_gate_ms must be non-negative")
        if not isinstance(octave_shift, int) or isinstance(octave_shift, bool):
            raise ValueError("octave_shift must be an integer")
        if abs(octave_shift) > 8:
            raise ValueError("octave_shift must be within -8..8")

        self._min_gate_ns = int(min_gate_ms * 1_000_000)
        self._sustain_enabled = bool(sustain_enabled)
        self._octave_shift = octave_shift
        self._fold_octaves = bool(fold_octaves)
        self._clock_ns = clock_ns if clock_ns is not None else time.monotonic_ns
        self._tokens: list[_PressToken] = []
        self._sustain_down = False
        self._current_hz = 0
        self._current_started_ns: int | None = None
        self._pending_stop_ns: int | None = None

    def note_hz(self, note: int) -> int:
        """Frequency this engine would emit for a MIDI note."""
        return playable_hz(note, self._octave_shift, self._fold_octaves)

    @property
    def current_hz(self) -> int:
        """Frequency currently expected on the wire; zero means stopped."""
        return self._current_hz

    @property
    def held_count(self) -> int:
        """Number of press tokens that have not received a release."""
        return sum(token.is_down for token in self._tokens)

    def handle_message(self, message: object, now_ns: int | None = None) -> int | None:
        """Apply one mido-like message and return a new desired wire frequency."""
        message_type = getattr(message, "type", None)

        if message_type == "note_on":
            note = getattr(message, "note", None)
            if not self._valid_note(note):
                return None
            now = self._now(now_ns)
            if getattr(message, "velocity", 0) == 0:
                return self._release(note, now)
            self._tokens.append(_PressToken(note))
            return self._transition(now)

        if message_type == "note_off":
            note = getattr(message, "note", None)
            if not self._valid_note(note):
                return None
            return self._release(note, self._now(now_ns))

        if message_type == "control_change":
            control = getattr(message, "control", None)
            if control in (120, 123):
                return self.panic()
            if control == 64 and self._sustain_enabled:
                return self._set_sustain(
                    getattr(message, "value", 0) >= 64,
                    self._now(now_ns),
                )

        return None

    def poll(self, now_ns: int | None = None) -> int | None:
        """Return zero once a delayed final-note stop reaches its due time."""
        if self._pending_stop_ns is None:
            return None
        if self._now(now_ns) < self._pending_stop_ns:
            return None

        self._pending_stop_ns = None
        if self._current_hz == 0:
            return None
        self._current_hz = 0
        self._current_started_ns = None
        return 0

    def panic(self) -> int | None:
        """Clear all state and stop immediately, bypassing the final-note gate."""
        self._tokens.clear()
        self._sustain_down = False
        self._pending_stop_ns = None
        self._current_started_ns = None
        if self._current_hz == 0:
            return None
        self._current_hz = 0
        return 0

    @staticmethod
    def _valid_note(note: object) -> bool:
        return isinstance(note, int) and not isinstance(note, bool) and 0 <= note <= 127

    def _now(self, now_ns: int | None) -> int:
        return int(self._clock_ns() if now_ns is None else now_ns)

    def _release(self, note: int, now_ns: int) -> int | None:
        for index, token in enumerate(self._tokens):
            if token.note != note or not token.is_down:
                continue
            if self._sustain_enabled and self._sustain_down:
                token.is_down = False
            else:
                self._tokens.pop(index)
            return self._transition(now_ns)
        return None

    def _set_sustain(self, is_down: bool, now_ns: int) -> int | None:
        if is_down == self._sustain_down:
            return None

        self._sustain_down = is_down
        if is_down:
            return None

        self._tokens = [token for token in self._tokens if token.is_down]
        return self._transition(now_ns)

    def _transition(self, now_ns: int) -> int | None:
        desired_hz = self.note_hz(self._tokens[-1].note) if self._tokens else 0

        if desired_hz:
            self._pending_stop_ns = None
            if desired_hz == self._current_hz:
                return None
            self._current_hz = desired_hz
            self._current_started_ns = now_ns
            return desired_hz

        if self._current_hz == 0:
            self._pending_stop_ns = None
            return None

        started_ns = self._current_started_ns
        if started_ns is None:
            started_ns = now_ns
            self._current_started_ns = started_ns
        stop_due_ns = started_ns + self._min_gate_ns
        if now_ns >= stop_due_ns:
            self._pending_stop_ns = None
            self._current_hz = 0
            self._current_started_ns = None
            return 0

        self._pending_stop_ns = stop_due_ns
        return None
