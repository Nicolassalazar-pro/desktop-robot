"""Pitch and timing primitives for the piezo mood-melody generator.

Everything here is hardware fact rather than musical opinion, so it is kept
separate from the emotion parameter tables that research will fill in.

Measured piezo envelope (src/notes.h, device-tested):

    audible          ~55 .. ~3323 Hz
    near-inaudible   below ~500 Hz
    loudest          1500 .. 3500 Hz  (element resonance)
    firmware `tone`  0 (stop) or 200 .. 6000 Hz

The transducer has no amplitude control, so loudness is purely a function of
frequency against the resonance curve. That makes the working band an
expressive decision, not just a safety limit.
"""

from __future__ import annotations

from dataclasses import dataclass

# Firmware contract: src/main.cpp rejects anything else.
TONE_MIN_HZ = 200
TONE_MAX_HZ = 6000
TONE_STOP_HZ = 0

# Measured audibility, from src/notes.h and HARDWARE.md:315-333.
#   200-500 Hz  very weak, mostly clicky
#   500-800 Hz  audible but thin
#   800-2000 Hz solid, clearly tonal
#   2000-4000   loudest (element mechanical resonance)
AUDIBLE_FLOOR_HZ = 500
TONAL_FLOOR_HZ = 800
RESONANT_LOW_HZ = 1500
RESONANT_HIGH_HZ = 3500

# Attack is 10-20 ms, so anything shorter than this clicks instead of sounding
# tonal. HARDWARE.md:329-331; the MIDI bridge enforces the same floor.
MIN_NOTE_MS = 45

# Re-articulating one pitch needs a real gap or the ear hears one long tone.
MIN_REARTICULATION_GAP_MS = 40

SEMITONES_PER_OCTAVE = 12
A4_MIDI = 69
A4_HZ = 440.0


def midi_to_hz(note: float) -> float:
    """Equal-tempered frequency for a MIDI note number, A4 = 440 Hz."""
    return A4_HZ * (2.0 ** ((note - A4_MIDI) / SEMITONES_PER_OCTAVE))


def hz_to_midi(hz: float) -> float:
    """Inverse of midi_to_hz; fractional result."""
    if hz <= 0.0:
        raise ValueError("frequency must be positive")
    import math

    return A4_MIDI + SEMITONES_PER_OCTAVE * math.log2(hz / A4_HZ)


def lowest_midi_at_or_above(hz: float) -> int:
    """Lowest integer MIDI note whose frequency is >= hz."""
    import math

    return int(math.ceil(hz_to_midi(hz) - 1e-9))


def highest_midi_at_or_below(hz: float) -> int:
    """Highest integer MIDI note whose frequency is <= hz."""
    import math

    return int(math.floor(hz_to_midi(hz) + 1e-9))


@dataclass(frozen=True)
class PitchBand:
    """A working pitch range expressed in MIDI notes.

    Bands are built from Hz because the hardware constraint is in Hz, but the
    generator walks in semitones, so both representations are kept.
    """

    name: str
    low_midi: int
    high_midi: int

    @classmethod
    def from_hz(cls, name: str, low_hz: float, high_hz: float) -> "PitchBand":
        if low_hz > high_hz:
            raise ValueError("low_hz must not exceed high_hz")
        if low_hz < TONE_MIN_HZ or high_hz > TONE_MAX_HZ:
            raise ValueError(
                f"band {low_hz}..{high_hz} Hz falls outside the firmware "
                f"tone range {TONE_MIN_HZ}..{TONE_MAX_HZ} Hz"
            )
        low = lowest_midi_at_or_above(low_hz)
        high = highest_midi_at_or_below(high_hz)
        if low > high:
            raise ValueError(
                f"band {low_hz}..{high_hz} Hz contains no whole semitone"
            )
        return cls(name=name, low_midi=low, high_midi=high)

    @property
    def semitone_span(self) -> int:
        return self.high_midi - self.low_midi

    @property
    def octave_span(self) -> float:
        return self.semitone_span / SEMITONES_PER_OCTAVE

    def contains(self, note: int) -> bool:
        return self.low_midi <= note <= self.high_midi

    def clamp(self, note: int) -> int:
        return max(self.low_midi, min(self.high_midi, note))


# --- Working band decision (R6, revised 2026-08-04 from listening) ----------
#
# Originally one octave (G6-G7), chosen from an inference about how sharply
# output falls away either side of the piezo resonance. Nick then played the
# robot through the live MIDI bridge, which exposes MIDI 56..114 (208-5920 Hz),
# and reported roughly two octaves as musically usable. Direct listening beats
# a modelled roll-off curve, so the band is widened to match what he heard.
#
# Still ONE shared band for all three moods. Per-emotion bands remain a trap:
# loudness here is purely a function of frequency, so a lower band for "sad"
# would make sad quieter, and the emotion would ride on an artifact.
#
# G5..B7 spans 784-3951 Hz. The top is extended past a round two octaves to
# reach the far edge of the 2-4 kHz resonant peak — that range is free, since
# it is the loudest part of the device's envelope.
#
# It deliberately stops short of the 4.8 octaves the MIDI bridge exposes. That
# figure is the FIRMWARE limit (200-6000 Hz), not the musical one: below
# ~800 Hz the piezo goes thin and then clicky, and above ~4 kHz it turns sharp
# and harsh. Roughly 2.4 octaves is what the device actually plays well, which
# matches what Nick heard.
# The tonic sits at G6, not at the band floor. Contours are built as offsets
# UPWARD from the tonic, so anchoring it low would park every phrase in the
# quietest usable octave — exactly the "no depth" problem, self-inflicted.
# From G6 a phrase spans roughly 1568-3136 Hz, i.e. the solid-to-loudest part
# of the envelope, while the wider band below and above leaves headroom for
# descending contours and terminal rises.
TONIC_MIDI = 91  # G6, ~1568 Hz
BAND_WORKING = PitchBand(name="working", low_midi=79, high_midi=107)

# The previous one-octave band, kept for A/B comparison on the real device.
BAND_SINGLE_OCTAVE = PitchBand(name="single-octave", low_midi=91, high_midi=103)

# Retained for A/B auditioning on the real device if the decision is revisited.
BAND_LOUD = PitchBand.from_hz("loud", RESONANT_LOW_HZ, RESONANT_HIGH_HZ)
BAND_WIDE = PitchBand.from_hz("wide", AUDIBLE_FLOOR_HZ, 4000)


def quantize_hz(note: int) -> int:
    """Integer Hz for a MIDI note, as the firmware `tone` command expects."""
    return int(round(midi_to_hz(note)))


def is_playable_hz(hz: int) -> bool:
    return hz == TONE_STOP_HZ or TONE_MIN_HZ <= hz <= TONE_MAX_HZ
