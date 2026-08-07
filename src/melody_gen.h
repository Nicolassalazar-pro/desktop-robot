#pragma once

// ============================================================================
// melody_gen.h — on-device port of the Python mood-melody generator.
//
// WHY THIS FILE EXISTS
// --------------------
// The original design generated seeded, musical happy/sad/confused phrases in
// Python (tools/melody_generate.py) and streamed frequencies to the robot over
// USB. That is fine on the bench and wrong for shipping: the robot is now
// controlled entirely from the phone over its own softAP, and there is no PC
// attached. This file ports the generator into firmware so the robot can
// generate its own phrases standalone.
//
// BEHAVIOURAL PORT, NOT BIT-EXACT
// --------------------------------
// Python's `random.Random` is Mersenne Twister with a specific cumulative
// weighted-choice implementation. A bit-exact port would need MT19937 in C++
// AND the same call sequence, both of which are fragile, heavy, and buy
// nothing musically: phrases are RANDOM by design, so a phrase that is
// "different but equally valid" is total success. This port therefore keeps
// the same algorithm, structure, tuned constants, and musical rules, and uses
// a small on-device PRNG (PCG32) seeded from a 64-bit key. Do NOT read a
// non-matching output as a porting bug: it is not.
//
// The bench-tuned rationales (`Bench 2026-08-04, Nick: ...` etc.) that appear
// as comments here are copied deliberately from the Python source. They are
// the reason each constant sits where it does; changing a constant silently
// would erase why it was tuned.
//
// FIRMWARE CONSTRAINTS THIS FILE OBEYS
// -----------------------------------
// * No heap. All buffers are static or on the stack, bounded and
//   static_assert'd at compile time.
// * No exceptions, no RTTI, no <vector>/<string>/<function>.
// * On failure the generator returns `false` rather than raising. A robot must
//   never crash over a note; degradation beats an exception every time.
// * Compiles for both the ESP32 target and the host test harness. This header
//   pulls in no Arduino-side symbols. Note that including <Arduino.h> defines
//   a `degrees()` macro; nothing here uses that name for exactly this reason.
// * Piezo hardware facts are honoured: firmware `tone` accepts 0 or
//   200..6000 Hz only; a MIDI 79..107 working band feeds directly into that
//   window; MIN_NOTE_MS = 45 (attack floor); MIN_REARTICULATION_GAP_MS = 40;
//   the 750 ms watchdog is handled by the Player refreshing tones, not by
//   silently shortening a long gate.
//
// STORAGE LIFETIME
// -----------------
// `Melody::Player::start` stores the const Note* POINTER and does not copy the
// note table. Generated phrases must therefore live in storage that outlasts
// playback. The provided pattern is: keep a `static MelodyGen::Phrase g_phrase`
// (or a global), generate into it, then feed `g_phrase.notes` to the Player.
// A stack-allocated Phrase whose owner returns before the Player finishes will
// leave the Player reading freed memory — do not do this.
// ============================================================================

#include <stdint.h>

#include "melody.h"

namespace MelodyGen {

// --------------------------------------------------------------------------
// Public constants — pitch/timing primitives mirrored from melody_pitch.py.
// --------------------------------------------------------------------------

// G6, ~1568 Hz. The tonic sits at G6 (not at the band floor) so contours
// built upward from the tonic reach the loudest part of the piezo envelope
// (1.5-3.5 kHz) rather than parking in the quiet octave below.
constexpr int8_t kTonicMidi = 91;

// Working band from melody_pitch.py:BAND_WORKING. Widened from one octave
// (G6-G7) after a live listening session showed roughly two octaves are
// usable on this transducer.
constexpr int8_t kBandLowMidi = 79;
constexpr int8_t kBandHighMidi = 107;

constexpr uint16_t kToneMinHz = 200;
constexpr uint16_t kToneMaxHz = 6000;

// Attack is 10-20 ms, so anything shorter clicks instead of sounding tonal.
constexpr uint16_t kMinNoteMs = 45;

// Two strikes of the same pitch fuse into one held note without this.
constexpr uint16_t kMinRearticulationGapMs = 40;

// PiezoTone's inactivity watchdog. Long gates must be represented HONESTLY
// and refreshed by the Player every loop tick; the generator never clamps.
constexpr uint16_t kPiezoWatchdogMs = 750;

// --------------------------------------------------------------------------
// Public constants — musical rules mirrored from melody_generate.py.
// --------------------------------------------------------------------------

// Length bands: draw a band, then draw a length inside it. Sad and confused
// cap their draws for reasons that are audible, not arbitrary — see the
// implementation of _generate_once.
struct LengthBand {
  uint8_t lo;
  uint8_t hi;
};
constexpr LengthBand kLengthBands[3] = {{2, 5}, {3, 9}, {4, 16}};

// Sustained notes make a long sad phrase drag; a 6+ second dirge outstays
// its welcome on a desk robot (Nick called 4.5 s "too long").
constexpr uint8_t kSadMaxLength = 6;

// Confusion caps by BODY compactness, not by band headroom. Wider bodies
// read as agitation; many notes inside a compact span oscillate, which
// reads as zigzagging rather than as searching.
constexpr uint8_t kConfusedMaxLength = 9;

// Backward-reachability memoisation is small, but rerunning it for every
// history retry still costs time. Eight retries is the same cap Python uses.
constexpr uint8_t kHistoryRetries = 8;

// 12-bit pitch-class masks (LSB = pitch class 0 = tonic).
// Major (Ionian): {0,2,4,5,7,9,11}. Minor (Aeolian): {0,2,3,5,7,8,10}.
// Confused is fully chromatic — Bench 2026-08-04, Nick: "sour note combos
// give confused and frustrated". Whole-tone had no semitones and read as
// dreamy; a chromatic pool supplies the sourness the emotion needs.
constexpr uint16_t kMajorMask = 0x0AB5;  // 0b101010110101
constexpr uint16_t kMinorMask = 0x05AD;  // 0b010110101101
constexpr uint16_t kConfusedMask = 0x0FFF;

// Confusion anchor range: G6..C#7. Confusion has no tonic to resolve to;
// it anchors instead.
constexpr int8_t kConfusedAnchorLowMidi = 91;
constexpr int8_t kConfusedAnchorHighMidi = 97;

// Confusion's compact-body span cap (semitones peak-to-trough).
constexpr int8_t kConfusedMaxBodySpan = 8;

// The four terminal gestures confusion uses in place of a tonal cadence.
enum class ConfusedEnding : uint8_t {
  Insist,
  Falter,
  Rise,
  Trail,
};
constexpr uint8_t kConfusedEndingCount = 4;

// --------------------------------------------------------------------------
// Note-shape conversion
// --------------------------------------------------------------------------
// Python's Note is {hz, gate_ms, gap_ms} — a tone plus its trailing silence.
// Melody::Note is {hz, ms} with hz==0 for a rest. Each generated note becomes
// 1 or 2 Player notes (the trailing rest is emitted only if gap_ms > 0).
//
// Worst case: happy's LENGTH_BANDS max is 16 notes; each may carry a
// non-zero gap except the final one which always has gap_ms==0. So the true
// worst case is 15 tones + 15 rests + 1 tone = 31 Player notes. A little
// headroom keeps the static_assert clean.
constexpr uint8_t kMaxPhraseNotes = 16;
constexpr uint8_t kMaxPlayerNotes = 2 * kMaxPhraseNotes;  // 32
static_assert(kMaxPlayerNotes >= 31,
              "kMaxPlayerNotes must fit 15 tones + 15 rests + 1 tone");

// --------------------------------------------------------------------------
// Types
// --------------------------------------------------------------------------

enum class Mood : uint8_t { Happy, Sad, Confused };

// A generated phrase, sized for the biggest legal happy contour. Kept as a
// plain aggregate so the caller owns it and can store it statically.
struct Phrase {
  Melody::Note notes[kMaxPlayerNotes];
  uint8_t noteCount;

  // Per-note MIDI numbers — the pitches actually played (post repeated-final
  // merge). Length matches midiCount, not noteCount, because a repeated
  // terminal is folded into one held Player note.
  int8_t midi[kMaxPhraseNotes];
  uint8_t midiCount;

  Mood mood;
  uint64_t seed;
  uint32_t totalMs;
  // For confused, the first `bodyLength` MIDI notes are the body; the rest
  // are the closing gesture. For happy/sad, bodyLength == midiCount.
  uint8_t bodyLength;
  int8_t terminalMidi;
  // Length band that was DRAWN. Realised length may sit one below the
  // minimum after a repeated-terminal merge; it never exceeds the maximum.
  LengthBand drawnBand;
  // The gesture chosen for confused; unused for happy/sad.
  ConfusedEnding confusedEnding;
};

// Fixed-capacity ring of recent phrase signatures.
//
// WHY THIS EXISTS: the RNG alone cannot deliver "a different phrase every
// time" — a 13-pitch band admits only a few hundred distinct short phrases,
// so two independent seeds can hash to the same shape. History is the only
// mechanism that guarantees novelty; it remembers signatures and rejects
// repeats until the retry budget is spent.
class History {
 public:
  static constexpr uint8_t kCapacity = 16;

  History();

  bool seen(uint64_t signature) const;
  void remember(uint64_t signature);

  uint8_t size() const { return count_; }
  void clear();

 private:
  uint64_t entries_[kCapacity];
  uint8_t count_;
};

// --------------------------------------------------------------------------
// API
// --------------------------------------------------------------------------

bool moodForName(const char* name, Mood& out);
const char* moodName(Mood m);

// Compute the transposition- and tempo-invariant signature hash of a phrase.
// Interval sequence plus percentage-binned rhythm — the same key the Python
// PhraseHistory uses, folded into a 64-bit hash so a fixed-size ring can
// hold it.
uint64_t phraseSignature(const Phrase& phrase);

// Generate one phrase. Returns false only if the mood is unrecognised or the
// generator itself becomes stuck (which the algorithm guarantees against for
// happy/sad/confused; the return value is for defensive future-proofing).
bool generate(Mood mood, uint64_t seed, Phrase& out);

// Generate one phrase, retrying against `hist` up to kHistoryRetries times
// against a derived seed to dodge a recent signature. Once accepted the
// signature is remembered. If the retry budget is exhausted the last
// candidate is accepted and remembered anyway — an unbounded novelty loop
// would be worse than an occasional repeat.
bool generateFresh(Mood mood, uint64_t seed, Phrase& out, History& hist);

}  // namespace MelodyGen
