// ============================================================================
// notes.h — Piezo audio reference: note→frequency table + hardware findings.
//
// All audio synthesis code has been stripped from the firmware. This header
// preserves the *knowledge* we discovered while experimenting (note math,
// hardware envelope, things that worked vs failed) so we can re-introduce a
// sound engine later without re-learning the gotchas.
//
// Include from main.cpp only if/when you actually wire audio back in.
// At rest this file costs zero bytes — pure constexpr + comments.
// ============================================================================

#pragma once
#include <stdint.h>

// ----------------------------------------------------------------------------
// HARDWARE ENVELOPE — Waveshare ESP32-S3-Touch-LCD-1.69 onboard piezo, GPIO42.
// ----------------------------------------------------------------------------
//   * Pin:                 GPIO42 (single-ended; other terminal is GND).
//   * Effective range:     ~55 Hz .. ~3323 Hz  (device-tested by Nick).
//   * Resonance peak:      ~2000-4000 Hz (loudest, brightest).
//   * Below ~500 Hz:       weak / nearly inaudible.
//   * Above ~6000 Hz:      thin "squeak", quiet again.
//   * Amplitude control:   none. Piezo is a 1-bit transducer driven by `tone()`.
//                          Duty-cycle variation does NOT yield clean volume control.
//   * LDO caveat:          never digitalWrite(GPIO42, HIGH) continuously --
//                          the on-board LDO overheats. Always drive AC (tone) or
//                          leave LOW.
//   * No DAC on S3:        ESP32-S3 dropped the DAC peripherals of the original
//                          ESP32. Pure LEDC PWM or I2S PDM are the options.
//
// Polyphony / chord findings (all on the single piezo, all failed to varying
// degrees):
//   * LEDC `tone()` is monophonic, sounds cleanest. **This is the best path.**
//   * LEDC dither (time-multiplex two pitches per ms) just sounds like a
//     warble, not a chord.
//   * Software 1-bit PDM @ 32 kHz produces muddy chords; quantization noise
//     in band.
//   * Hardware I2S PDM TX @ 48 kHz × 128 oversample (~6 MHz pulse rate) is
//     the cleanest poly attempt -- but piezo's narrow mechanical bandwidth
//     still smears the result. Notes inside 2-4 kHz resonance fare best.
//
// Speech / babble findings:
//   * Single-tone TTS (Klatt, SAM, Talkie LPC ports) is unintelligible on
//     this hardware.  Removed; do not re-add without a real speaker.
//   * Synthesized babble with per-mood pitch contours doesn't read as
//     emotion -- the bandwidth is too narrow to encode prosody.
//   * Playing back real R2-D2 samples (from a YouTube comp) AS-IS via
//     audio_to_piezo.py mapped well to mood expressions and was the
//     subjectively best result.

// ----------------------------------------------------------------------------
// PRACTICAL TIPS for picking notes for melodies on this piezo
// ----------------------------------------------------------------------------
//   * Keep the melody inside ~1500-3500 Hz for loudness. That maps to roughly
//     octave 6 - octave 7 in equal-tempered tuning (NOTE_*_6 .. NOTE_*_7).
//   * If a song's natural range is too low, transpose UP by octaves rather
//     than down -- the piezo barely produces anything below ~500 Hz.
//   * Add a small inter-note gap (30-50 ms) when re-articulating the same
//     pitch back-to-back so the ear hears two notes, not one long tone.

// ----------------------------------------------------------------------------
// EQUAL-TEMPERED NOTE TABLE  (A4 = 440 Hz, 12-TET)
// Sharps use 'S' suffix (NOTE_CS4 = C#4). Use sharps for flats (E♭4 = D#4).
// Range chosen to cover the piezo's tested envelope plus headroom for any
// shifted/transposed source material.
// ----------------------------------------------------------------------------

// Octave 1  (A1=55 Hz is the bottom of the device's audible band)
constexpr uint16_t NOTE_A1   = 55;
constexpr uint16_t NOTE_AS1  = 58;
constexpr uint16_t NOTE_B1   = 62;

// Octave 2
constexpr uint16_t NOTE_C2   = 65;
constexpr uint16_t NOTE_CS2  = 69;
constexpr uint16_t NOTE_D2   = 73;
constexpr uint16_t NOTE_DS2  = 78;
constexpr uint16_t NOTE_E2   = 82;
constexpr uint16_t NOTE_F2   = 87;
constexpr uint16_t NOTE_FS2  = 93;
constexpr uint16_t NOTE_G2   = 98;
constexpr uint16_t NOTE_GS2  = 104;
constexpr uint16_t NOTE_A2   = 110;
constexpr uint16_t NOTE_AS2  = 117;
constexpr uint16_t NOTE_B2   = 123;

// Octave 3
constexpr uint16_t NOTE_C3   = 131;
constexpr uint16_t NOTE_CS3  = 139;
constexpr uint16_t NOTE_D3   = 147;
constexpr uint16_t NOTE_DS3  = 156;
constexpr uint16_t NOTE_E3   = 165;
constexpr uint16_t NOTE_F3   = 175;
constexpr uint16_t NOTE_FS3  = 185;
constexpr uint16_t NOTE_G3   = 196;
constexpr uint16_t NOTE_GS3  = 208;
constexpr uint16_t NOTE_A3   = 220;
constexpr uint16_t NOTE_AS3  = 233;
constexpr uint16_t NOTE_B3   = 247;

// Octave 4  (A4 = 440 Hz reference)
constexpr uint16_t NOTE_C4   = 262;
constexpr uint16_t NOTE_CS4  = 277;
constexpr uint16_t NOTE_D4   = 294;
constexpr uint16_t NOTE_DS4  = 311;
constexpr uint16_t NOTE_E4   = 330;
constexpr uint16_t NOTE_F4   = 349;
constexpr uint16_t NOTE_FS4  = 370;
constexpr uint16_t NOTE_G4   = 392;
constexpr uint16_t NOTE_GS4  = 415;
constexpr uint16_t NOTE_A4   = 440;
constexpr uint16_t NOTE_AS4  = 466;
constexpr uint16_t NOTE_B4   = 494;

// Octave 5
constexpr uint16_t NOTE_C5   = 523;
constexpr uint16_t NOTE_CS5  = 554;
constexpr uint16_t NOTE_D5   = 587;
constexpr uint16_t NOTE_DS5  = 622;
constexpr uint16_t NOTE_E5   = 659;
constexpr uint16_t NOTE_F5   = 698;
constexpr uint16_t NOTE_FS5  = 740;
constexpr uint16_t NOTE_G5   = 784;
constexpr uint16_t NOTE_GS5  = 831;
constexpr uint16_t NOTE_A5   = 880;
constexpr uint16_t NOTE_AS5  = 932;
constexpr uint16_t NOTE_B5   = 988;

// Octave 6  (entering the loud zone)
constexpr uint16_t NOTE_C6   = 1047;
constexpr uint16_t NOTE_CS6  = 1109;
constexpr uint16_t NOTE_D6   = 1175;
constexpr uint16_t NOTE_DS6  = 1245;
constexpr uint16_t NOTE_E6   = 1319;
constexpr uint16_t NOTE_F6   = 1397;
constexpr uint16_t NOTE_FS6  = 1480;
constexpr uint16_t NOTE_G6   = 1568;
constexpr uint16_t NOTE_GS6  = 1661;
constexpr uint16_t NOTE_A6   = 1760;
constexpr uint16_t NOTE_AS6  = 1865;
constexpr uint16_t NOTE_B6   = 1976;

// Octave 7  (peak resonance band -- loudest)
constexpr uint16_t NOTE_C7   = 2093;
constexpr uint16_t NOTE_CS7  = 2217;
constexpr uint16_t NOTE_D7   = 2349;
constexpr uint16_t NOTE_DS7  = 2489;
constexpr uint16_t NOTE_E7   = 2637;
constexpr uint16_t NOTE_F7   = 2794;
constexpr uint16_t NOTE_FS7  = 2960;
constexpr uint16_t NOTE_G7   = 3136;
constexpr uint16_t NOTE_GS7  = 3322;   // <-- ~upper edge of useful range

// Beyond NOTE_GS7 the piezo is thin / quiet — included for completeness only.
// Stop using these in actual melodies; they sound like a mosquito.
constexpr uint16_t NOTE_A7   = 3520;
constexpr uint16_t NOTE_AS7  = 3729;
constexpr uint16_t NOTE_B7   = 3951;

constexpr uint16_t NOTE_C8   = 4186;
constexpr uint16_t NOTE_CS8  = 4435;
constexpr uint16_t NOTE_D8   = 4699;
constexpr uint16_t NOTE_DS8  = 4978;
constexpr uint16_t NOTE_E8   = 5274;
constexpr uint16_t NOTE_F8   = 5588;
constexpr uint16_t NOTE_FS8  = 5920;
constexpr uint16_t NOTE_G8   = 6272;
constexpr uint16_t NOTE_GS8  = 6645;
constexpr uint16_t NOTE_A8   = 7040;
constexpr uint16_t NOTE_AS8  = 7459;
constexpr uint16_t NOTE_B8   = 7902;

// Convenience: silence / rest marker.
constexpr uint16_t NOTE_REST = 0;

// ----------------------------------------------------------------------------
// PROCEDURAL MOOD MELODIES (2026-08-03) — tools/mood_melody.py
// Findings from six research threads, recorded so they survive a rewrite.
// ----------------------------------------------------------------------------
//
// WORKING BAND: MIDI 79..107 (784..3951 Hz), ~2.4 octaves, TONIC AT G6
// (1568 Hz). Shared by every mood. Per-emotion bands are a TRAP on this device:
// loudness here is
// purely a function of frequency, so giving "sad" a lower band makes sad
// quieter — the emotion would then ride on an artifact and just sound broken.
// Both G major and G minor fit entirely inside this octave.
//
// WHAT ACTUALLY CARRIES EMOTION HERE (strongest first):
//   1. Note duration / tempo. More salient than mode (Gagnon & Peretz).
//   2. Scale mask. Mode explains ~48% of happy-rating variance (Eerola 2013).
//   3. Contour and resolution.
//   Register is NOT usable: it needs 2-3 octaves to identify on its own
//   (Brewster) and we have one.
//
// ARPEGGIO CHORD SIMULATION IS NOT VIABLE. The classic chiptune trick for
// implying major/minor on one voice needs 20-35 ms per pitch, but the attack
// here is 10-20 ms so anything under ~40 ms clicks instead of sounding tonal.
// Mode must come from SEQUENTIAL intervals only.
//
// THE 750 ms WATCHDOG BITES. PiezoTone stops any tone 750 ms after the last
// command. Sad note gates reach ~790 ms, so a player MUST re-send the same
// tone periodically (250 ms works) or long notes are silently truncated
// mid-note. This is the easiest bug here to misdiagnose by ear.
//
// CONFUSION — honest limit: no controlled study shows a short monophonic
// phrase is reliably heard as "confused". Two cues do the work:
//   * a TERMINAL RISE of +3..+5 semitones, held 220-300 ms (most "Huh?"-type
//     repair interjections across 10 languages rise — Dingemanse 2013);
//   * exactly ONE hesitation gap of 180-260 ms (filled pauses dominate
//     perceived low confidence — Kirkland 2022).
// AVOID TRITONES AND DIMINISHED SHAPES. They are the obvious choice for an
// "unstable" interval and the wrong one: they read as menacing, not puzzled.
// Confusion must NOT be globally slow either — that is exactly what makes it
// collide with sadness. Keep it quick with one interruption.
//
// SAD SOUNDS PLAINTIVE, NOT GRAVE. Everything sits in octave 6-7, so sadness
// reads fragile/childlike rather than heavy. On a small desk robot that is
// arguably on-character.
//
// "RANDOM EVERY TIME" IS NOT ACHIEVABLE BY RNG ALONE. A 13-pitch band admits
// only a few hundred distinct short phrases and seeds collide. Real novelty
// needs a recent-phrase memory that compares RELATIVE intervals, so the same
// tune transposed still counts as a repeat.

// BAND REVISION (2026-08-04) — from listening, not modelling.
// The original one-octave band came from a modelled resonance roll-off that
// was never measured. Nick then played the robot through the live MIDI bridge
// and reported ~2 octaves musically usable. Direct listening wins, so the band
// widened to 784-3951 Hz (~2.4 octaves).
//
// Note what the MIDI bridge exposes (208-5920 Hz, 4.8 octaves) is the FIRMWARE
// limit, not the musical one. Below ~800 Hz the piezo goes thin then clicky;
// above ~4 kHz it turns sharp and harsh. About 2.4 octaves is what this device
// actually plays well.
//
// TONIC PLACEMENT MATTERS AS MUCH AS BAND WIDTH. Contours are built as offsets
// UPWARD from the tonic. Anchoring the tonic at the band floor parked every
// phrase in the quietest usable octave — a self-inflicted version of the very
// "no depth" problem we were trying to avoid. Tonic now sits at G6 (1568 Hz):
// all notes land above the 800 Hz tonal threshold and 30-48% fall inside the
// loudest 2-4 kHz window.
//
// Rapid articulation is a real expressive lever here. With no volume control,
// speed is one of the few ways left to signal energy, so happy's fastest rate
// was extended to ~7 notes/s (140 ms), just past the fastest tempo the
// research literature had tested.

// ============================================================================
// BENCH RESULTS 2026-08-04 — all three moods CONFIRMED READABLE on hardware.
// This settles the open question left by the earlier failed attempt.
// ============================================================================
//
// Verdicts, in Nick's words:
//   happy    -> "like task accomplished"
//   sad      -> "like task failed"
//   confused -> "confused but not frustrated, like curious"
// All three mutually distinguishable. They landed as a functional set, which
// maps better onto a desk robot's needs than abstract emotion labels do.
//
// THE BIG CORRECTION — SLOW IS NOT AVAILABLE AS A SAD CUE HERE.
// The music-psychology literature is emphatic that slow tempo is the strongest
// sadness cue, and it is wrong for this device. That evidence comes from
// instruments with timbre and natural decay. A bare square wave held for
// ~700 ms rings like a theremin: the first sad attempt was heard as "haunted
// house, more scary than sad".
//
// What actually works is Nick's own observation: "emotions are gonna have to
// be notes in quick succession, real quick, kinda like how a bird chirps."
// The evidence agreed — the two moods that read first (happy ~80 ms notes,
// confused ~100 ms) were the fast ones; the only failure was the slow one.
// This also matches the earlier finding recorded above that R2-D2 sample
// playback "mapped well to mood expressions" — R2-D2 is rapid chirps.
//
// So: EVERY mood lives in the fast chirp register (~95-260 ms inter-onset),
// and emotion is carried by CONTOUR and INTERVAL, not by tempo:
//   happy    fastest, major, rising, resolves up to the tonic
//   sad      slightly slower, minor, falling, settles ON the tonic
//   confused mid, whole-tone/atonal, one hesitation, ends on a rising "?"
//
// THREE SPECIFIC TRAPS FOUND BY LISTENING, each invisible in the data:
//
//  1. FANFARE. A monotonically ascending body plus the terminal rise makes one
//     long ramp and reads as an anthem — heard as "the Star Spangled Banner".
//     No confused body may ascend throughout.
//  2. LIFTING CADENCE. Sad ended on the minor third 40% of the time, and the
//     third sits ABOVE the tonic, so the line rose at the moment it should
//     settle. Sad now ends on the tonic only.
//  3. STUTTER. A repeated closing pitch struck twice reads as a stutter, not a
//     resolution. Repeated final pitches are merged into one held note
//     ("2 beats continuous"). Merge the pitch list alongside the note list or
//     the metadata silently desyncs.
//
// Register note: everything sits in octave 6-7, so sad reads plaintive rather
// than grave. On a small desk robot that is arguably on-character.
