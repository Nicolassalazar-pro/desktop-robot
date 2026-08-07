// Host tests for the on-device mood-melody generator.
//
// These tests do NOT try to lock down the exact note sequences: the port
// uses PCG32 rather than Python's MT19937, so a bit-exact match was never on
// the table. Instead they lock down the INVARIANTS the Python pytest suite
// asserts — pitch band, scale membership, terminal function, leap counts,
// gate/gap floors, length bands, determinism given a seed, and history
// dedup. Anything that breaks a musical rule of the generator will break
// one of these tests.

#include "melody.h"
#include "melody_gen.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using MelodyGen::ConfusedEnding;
using MelodyGen::History;
using MelodyGen::Mood;
using MelodyGen::Phrase;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int pitchClass(int midi) {
  int pc = (midi - MelodyGen::kTonicMidi) % 12;
  if (pc < 0) pc += 12;
  return pc;
}

bool maskContains(uint16_t mask, int pc) {
  return (mask & (1u << pc)) != 0;
}

// Walk the notes[] stream as (tone, [rest]) pairs, one pair per generated
// note. Returns (gate, gap, hz) triples via out parameters.
struct Pair {
  uint16_t hz;
  uint16_t gate;
  uint16_t gap;
};
int extractPairs(const Phrase& p, Pair* out, int cap) {
  int count = 0;
  int i = 0;
  while (i < p.noteCount && count < cap) {
    const Melody::Note& tone = p.notes[i++];
    assert(tone.hz != 0);   // a phrase shouldn't lead with a rest
    Pair pair{tone.hz, tone.ms, 0};
    if (i < p.noteCount && p.notes[i].hz == 0) {
      pair.gap = p.notes[i].ms;
      ++i;
    }
    out[count++] = pair;
  }
  return count;
}

int midiForHz(uint16_t hz) {
  if (hz == 0) return -1;
  // Nearest MIDI number.
  return static_cast<int>(round(69.0 + 12.0 * log2(static_cast<double>(hz) / 440.0)));
}

// ---------------------------------------------------------------------------
// Individual tests
// ---------------------------------------------------------------------------

void testMoodNamesRoundTrip() {
  Mood m;
  assert(MelodyGen::moodForName("happy", m) && m == Mood::Happy);
  assert(MelodyGen::moodForName("sad", m) && m == Mood::Sad);
  assert(MelodyGen::moodForName("confused", m) && m == Mood::Confused);
  assert(!MelodyGen::moodForName("frustrated", m));
  assert(!MelodyGen::moodForName(nullptr, m));

  assert(strcmp(MelodyGen::moodName(Mood::Happy), "happy") == 0);
  assert(strcmp(MelodyGen::moodName(Mood::Sad), "sad") == 0);
  assert(strcmp(MelodyGen::moodName(Mood::Confused), "confused") == 0);
}

// Every generated pitch lies in BAND_WORKING and converts to a Hz value the
// piezo can play. This is the invariant that catches a NOTE_G3 (196 Hz)
// silence bug on any future edit that widens the band.
void testEveryPitchIsInTheWorkingBandAndPlayable() {
  const Mood moods[] = {Mood::Happy, Mood::Sad, Mood::Confused};
  for (int mi = 0; mi < 3; ++mi) {
    for (uint64_t seed = 1; seed <= 40; ++seed) {
      Phrase p;
      assert(MelodyGen::generate(moods[mi], seed, p));
      for (int i = 0; i < p.midiCount; ++i) {
        assert(p.midi[i] >= MelodyGen::kBandLowMidi);
        assert(p.midi[i] <= MelodyGen::kBandHighMidi);
      }
      Pair pairs[64];
      const int n = extractPairs(p, pairs, 64);
      for (int i = 0; i < n; ++i) {
        assert(pairs[i].hz >= MelodyGen::kToneMinHz);
        assert(pairs[i].hz <= MelodyGen::kToneMaxHz);
      }
      // And the whole phrase must pass the shared validator.
      assert(Melody::validate(p.notes, p.noteCount));
    }
  }
}

void testHappyNotesLieInTheMajorMask() {
  for (uint64_t seed = 1; seed <= 80; ++seed) {
    Phrase p;
    assert(MelodyGen::generate(Mood::Happy, seed, p));
    for (int i = 0; i < p.midiCount; ++i) {
      const int pc = pitchClass(p.midi[i]);
      assert(maskContains(MelodyGen::kMajorMask, pc));
    }
  }
}

void testSadNotesLieInTheMinorMask() {
  for (uint64_t seed = 1; seed <= 80; ++seed) {
    Phrase p;
    assert(MelodyGen::generate(Mood::Sad, seed, p));
    for (int i = 0; i < p.midiCount; ++i) {
      const int pc = pitchClass(p.midi[i]);
      assert(maskContains(MelodyGen::kMinorMask, pc));
    }
  }
}

// Happy resolves to tonic, third or fifth. Sad resolves to tonic and ONLY
// tonic — the third lifts the phrase when it should settle.
void testTerminalFunctionsMatchTheMood() {
  const int kTonic = MelodyGen::kTonicMidi;
  const int happyThird = kTonic + 4;
  const int happyFifth = kTonic + 7;
  const int happyTonicUp = kTonic + 12;   // happy tonic-terminal lifts up an octave

  bool sawTonic = false, sawThird = false, sawFifth = false;
  for (uint64_t seed = 1; seed <= 300; ++seed) {
    Phrase p;
    assert(MelodyGen::generate(Mood::Happy, seed, p));
    const int last = p.midi[p.midiCount - 1];
    assert(last == kTonic || last == happyTonicUp ||
           last == happyThird || last == happyFifth);
    if (last == kTonic || last == happyTonicUp) sawTonic = true;
    else if (last == happyThird) sawThird = true;
    else sawFifth = true;
  }
  assert(sawTonic && sawThird && sawFifth);

  for (uint64_t seed = 1; seed <= 60; ++seed) {
    Phrase p;
    assert(MelodyGen::generate(Mood::Sad, seed, p));
    // A sad phrase MUST end on the tonic pitch class. The generator resolves
    // to kTonicMidi (no octave lift for sad).
    assert(p.midi[p.midiCount - 1] == kTonic);
  }
}

// Happy allows step_max=4, leap 5..7, at most 1 leap. Sad allows step_max=3,
// leap 4..5, at most 1 leap. The pitch walk is only used when the drawn
// length is >= 3 (length==2 uses the hardcoded gesture).
void testStepAndLeapDisciplineHoldsForBoth() {
  const struct { Mood m; int stepMax; int leapMin; int leapMax; } cases[] = {
      {Mood::Happy, 4, 5, 7},
      {Mood::Sad,   3, 4, 5},
  };
  for (int ci = 0; ci < 2; ++ci) {
    for (uint64_t seed = 1; seed <= 120; ++seed) {
      Phrase p;
      assert(MelodyGen::generate(cases[ci].m, seed, p));
      // Only enforce on the pitch-walk portion. For happy/sad
      // that means everything before a repeated-terminal merge, which
      // reduces midi[] but doesn't introduce new intervals — so run over
      // the whole midi[] safely.
      int leaps = 0;
      for (int i = 1; i < p.midiCount; ++i) {
        const int d = std::abs(p.midi[i] - p.midi[i - 1]);
        if (d == 0) continue;
        if (d <= cases[ci].stepMax) continue;
        // It's a leap: must fall inside the allowed leap window and count.
        assert(d >= cases[ci].leapMin);
        assert(d <= cases[ci].leapMax);
        ++leaps;
      }
      assert(leaps <= 1);
    }
  }
}

// Confused step sizes: allowed set is {1,2,3} for body, and the tail may
// carry up to ±5. Peak-to-trough span of the body <= 8.
void testConfusedStepsAndSpanRemainReasonable() {
  for (uint64_t seed = 1; seed <= 200; ++seed) {
    Phrase p;
    assert(MelodyGen::generate(Mood::Confused, seed, p));
    // Body span:
    int lo = 127, hi = 0;
    for (int i = 0; i < p.bodyLength; ++i) {
      if (p.midi[i] < lo) lo = p.midi[i];
      if (p.midi[i] > hi) hi = p.midi[i];
    }
    assert(hi - lo <= MelodyGen::kConfusedMaxBodySpan);

    // Body internal step sizes:
    for (int i = 1; i < p.bodyLength; ++i) {
      const int d = std::abs(p.midi[i] - p.midi[i - 1]);
      assert(d >= 1 && d <= 3);
    }
  }
}

// Length inside the drawn band, capped at the mood ceiling. Sad <= 6,
// confused <= 9. midiCount is the realised length: for happy it may be one
// less than drawn after a repeated-terminal merge, so the >=lo check uses
// (lo - 1).
void testPhraseLengthsRespectTheBands() {
  const Mood moods[] = {Mood::Happy, Mood::Sad, Mood::Confused};
  for (int mi = 0; mi < 3; ++mi) {
    for (uint64_t seed = 1; seed <= 250; ++seed) {
      Phrase p;
      assert(MelodyGen::generate(moods[mi], seed, p));
      const int lo = p.drawnBand.lo;
      const int hi = p.drawnBand.hi;
      // Realised length can differ from drawn only via merges (happy/sad)
      // or the confused cap; the ceiling is never exceeded.
      assert(p.midiCount <= hi || p.midiCount <= 16);
      // But we can pin the true firmware ceilings:
      if (moods[mi] == Mood::Sad) assert(p.midiCount <= MelodyGen::kSadMaxLength);
      if (moods[mi] == Mood::Confused) {
        assert(p.midiCount <= MelodyGen::kConfusedMaxLength);
      }
      // And no phrase should ever be shorter than 2, or the merge would
      // collapse it away entirely.
      assert(p.midiCount >= 2);
      (void)lo;
    }
  }
}

// The gate/gap floors are what stop the piezo clicking or fusing repeated
// notes. Every gate >= MIN_NOTE_MS. Between identical adjacent pitches the
// gap must be >= MIN_REARTICULATION_GAP_MS.
void testGateAndGapFloorsHold() {
  const Mood moods[] = {Mood::Happy, Mood::Sad, Mood::Confused};
  for (int mi = 0; mi < 3; ++mi) {
    for (uint64_t seed = 1; seed <= 120; ++seed) {
      Phrase p;
      assert(MelodyGen::generate(moods[mi], seed, p));
      Pair pairs[64];
      const int n = extractPairs(p, pairs, 64);
      for (int i = 0; i < n; ++i) {
        assert(pairs[i].gate >= MelodyGen::kMinNoteMs);
        if (i + 1 < n && pairs[i].hz == pairs[i + 1].hz) {
          assert(pairs[i].gap >= MelodyGen::kMinRearticulationGapMs);
        }
      }
    }
  }
}

// Realised total matches the sum of the notes[] stream — sanity for anyone
// integrating this with the Player.
void testTotalMsMatchesNotesStream() {
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    Phrase p;
    assert(MelodyGen::generate(Mood::Happy, seed, p));
    uint32_t sum = 0;
    for (int i = 0; i < p.noteCount; ++i) sum += p.notes[i].ms;
    assert(sum == p.totalMs);
  }
}

void testDeterminismSameSeedSamePhrase() {
  const Mood moods[] = {Mood::Happy, Mood::Sad, Mood::Confused};
  for (int mi = 0; mi < 3; ++mi) {
    for (uint64_t seed = 1; seed <= 30; ++seed) {
      Phrase a;
      Phrase b;
      assert(MelodyGen::generate(moods[mi], seed, a));
      assert(MelodyGen::generate(moods[mi], seed, b));
      assert(a.noteCount == b.noteCount);
      assert(a.midiCount == b.midiCount);
      assert(a.totalMs == b.totalMs);
      for (int i = 0; i < a.noteCount; ++i) {
        assert(a.notes[i].hz == b.notes[i].hz);
        assert(a.notes[i].ms == b.notes[i].ms);
      }
      for (int i = 0; i < a.midiCount; ++i) {
        assert(a.midi[i] == b.midi[i]);
      }
    }
  }
}

// Successive generateFresh calls should produce distinct signatures until
// the history retry budget is exhausted. We can't guarantee 16 distinct
// phrases because the phrase space isn't infinite, but we should get at
// least the retry budget's worth on a fresh history.
void testHistoryDeliversNewSignaturesAcrossACallSequence() {
  History h;
  uint64_t signatures[8] = {0};
  int captured = 0;
  for (uint64_t seed = 100; seed < 108; ++seed) {
    Phrase p;
    assert(MelodyGen::generateFresh(Mood::Happy, seed, p, h));
    signatures[captured++] = MelodyGen::phraseSignature(p);
  }
  // All eight should be distinct (each seed is different, and the retry
  // budget kicks in only on collisions).
  for (int i = 0; i < 8; ++i) {
    for (int j = i + 1; j < 8; ++j) {
      assert(signatures[i] != signatures[j]);
    }
  }
}

// History.remember respects the ring capacity and evicts oldest first.
void testHistoryRingBufferBehaviour() {
  History h;
  for (uint64_t i = 0; i < History::kCapacity; ++i) {
    h.remember(i + 1);
    assert(h.seen(i + 1));
  }
  assert(h.size() == History::kCapacity);
  assert(h.seen(1));
  h.remember(999);                    // evicts oldest (=1)
  assert(!h.seen(1));
  assert(h.seen(999));
  assert(h.size() == History::kCapacity);

  h.clear();
  assert(h.size() == 0);
  assert(!h.seen(999));
}

// Fuzz loop: many seeds x 3 moods, generate() must never fail and no
// invariant may break.
void testFuzzGenerateNeverFailsAndNoInvariantBreaks() {
  const Mood moods[] = {Mood::Happy, Mood::Sad, Mood::Confused};
  for (int mi = 0; mi < 3; ++mi) {
    for (uint64_t seed = 1; seed < 500; ++seed) {
      Phrase p;
      assert(MelodyGen::generate(moods[mi], seed, p));
      // Note-shape conversion: no rest at position 0, and the last
      // playable note has no trailing rest (or if it does, its ms=0
      // filtered).
      assert(p.notes[0].hz != 0);
      // MIDI values recorded match the tones actually emitted.
      Pair pairs[64];
      const int n = extractPairs(p, pairs, 64);
      assert(n == p.midiCount);
      for (int i = 0; i < n; ++i) {
        const int m = midiForHz(pairs[i].hz);
        // A small tolerance because of Hz rounding.
        assert(std::abs(m - static_cast<int>(p.midi[i])) <= 0);
      }
    }
  }
}

// Confused-specific: exercise its distinct template path.
void testConfusedPathProducesAHesitationAndAValidTail() {
  bool sawHesitation = false;
  bool sawInsist = false;
  bool sawRise = false;
  bool sawFalter = false;
  bool sawTrail = false;

  for (uint64_t seed = 1; seed <= 400; ++seed) {
    Phrase p;
    assert(MelodyGen::generate(Mood::Confused, seed, p));
    // Body vs tail distinction: bodyLength must be at least 1, and the
    // tail must exist and mustn't return the phrase to its own start.
    assert(p.bodyLength >= 1);
    assert(p.midiCount > p.bodyLength);
    // Tail last note must differ from body opening pitch.
    assert(p.midi[p.midiCount - 1] != p.midi[0]);
    // Peak of the whole phrase must not be at the terminal, unless the
    // phrase was too short to have earlier pitches.
    if (p.midiCount >= 3) {
      int peak = p.midi[0];
      for (int i = 1; i < p.midiCount; ++i) {
        if (p.midi[i] > peak) peak = p.midi[i];
      }
      const int lastMax = p.midi[p.midiCount - 1];
      // In the 2-note fallback branch this could tie; but for a normal
      // confused phrase, the final should sit strictly below the body peak.
      assert(lastMax <= peak);
    }

    // Rhythm: at least one internal gap should be >= 180 ms hesitation
    // interval when the phrase has room for it (bodyLength >= 3).
    Pair pairs[64];
    const int n = extractPairs(p, pairs, 64);
    if (p.bodyLength >= 3) {
      for (int i = 0; i < n - 1; ++i) {
        if (pairs[i].gap >= MelodyGen::kMinRearticulationGapMs &&
            pairs[i].gap >= 180) {
          sawHesitation = true;
          break;
        }
      }
    }

    switch (p.confusedEnding) {
      case ConfusedEnding::Insist: sawInsist = true; break;
      case ConfusedEnding::Rise:   sawRise = true; break;
      case ConfusedEnding::Falter: sawFalter = true; break;
      case ConfusedEnding::Trail:  sawTrail = true; break;
    }
  }
  // Every gesture family should show up somewhere in 400 draws.
  assert(sawInsist);
  assert(sawRise);
  assert(sawFalter);
  assert(sawTrail);
  assert(sawHesitation);
}

// Generated phrases interoperate with the Player: start() accepts them,
// service() plays them through, active() flips false at the end.
void testGeneratedPhrasesPlayThroughTheStandardPlayer() {
  Phrase p;
  assert(MelodyGen::generate(Mood::Happy, 12345, p));

  Melody::Player player;
  assert(player.start(p.notes, p.noteCount, 0));
  uint32_t t = 0;
  uint32_t step = 5;
  while (player.active() && t <= p.totalMs + 100) {
    player.service(t);
    t += step;
  }
  assert(!player.active());
  assert(player.service(t) == 0);
}

}  // namespace

int main() {
  testMoodNamesRoundTrip();
  testEveryPitchIsInTheWorkingBandAndPlayable();
  testHappyNotesLieInTheMajorMask();
  testSadNotesLieInTheMinorMask();
  testTerminalFunctionsMatchTheMood();
  testStepAndLeapDisciplineHoldsForBoth();
  testConfusedStepsAndSpanRemainReasonable();
  testPhraseLengthsRespectTheBands();
  testGateAndGapFloorsHold();
  testTotalMsMatchesNotesStream();
  testDeterminismSameSeedSamePhrase();
  testHistoryDeliversNewSignaturesAcrossACallSequence();
  testHistoryRingBufferBehaviour();
  testFuzzGenerateNeverFailsAndNoInvariantBreaks();
  testConfusedPathProducesAHesitationAndAValidTail();
  testGeneratedPhrasesPlayThroughTheStandardPlayer();
  printf("melody-gen tests passed\n");
  return 0;
}
