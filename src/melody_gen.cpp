// ============================================================================
// melody_gen.cpp — implementation of the on-device mood-melody generator.
//
// This file is a behavioural port of tools/melody_generate.py. The algorithm,
// structure, and every tuned constant are preserved; the only substantive
// change is the PRNG (PCG32 in place of MT19937) and the transposition of the
// note shape ({hz, gate_ms, gap_ms}) into the firmware's {hz, ms} pair with
// hz==0 as a rest.
//
// The comments here are copied deliberately from the Python source and
// enriched where the C++ needs its own note; every "bench" or "Nick" comment
// records a real listening session that fixed one of the constants. If you
// find yourself wanting to change one, please read the comment first — the
// history is usually why the current value is not what you would expect.
// ============================================================================

#include "melody_gen.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace MelodyGen {

namespace {

// ---------------------------------------------------------------------------
// Constants that stay private to this translation unit.
// ---------------------------------------------------------------------------

constexpr float kResolutionStart = 0.65f;
constexpr int8_t kPostSkipSemitones = 5;

// Confused body/tail timing envelopes, mirrored from CONFUSED_* in Python.
constexpr uint16_t kConfusedBodyGateLoMs = 90;
constexpr uint16_t kConfusedBodyGateHiMs = 150;
constexpr uint16_t kConfusedGapLoMs = 35;
constexpr uint16_t kConfusedGapHiMs = 75;
constexpr uint16_t kConfusedHesitationLoMs = 180;
constexpr uint16_t kConfusedHesitationHiMs = 260;

// Terminal rise choices for confusion's `rise` gesture: +3, +4, +5 semitones.
constexpr int8_t kConfusedRiseMin = 3;
constexpr int8_t kConfusedRiseMax = 5;

// Confusion body span cap during construction: {-4..+4} semitones around
// the anchor.
constexpr int8_t kConfusedSpanLo = -4;
constexpr int8_t kConfusedSpanHi = 4;

// Weights for confused ending selection. Insist heaviest (the frustrated
// reading); trail lightest (the most sad-leaning).
constexpr float kConfusedEndingWeights[kConfusedEndingCount] = {
    0.4f,   // Insist
    0.25f,  // Falter
    0.25f,  // Rise
    0.1f,   // Trail
};

// Terminal options for happy: (tonic, third, fifth) weighted.
struct TerminalChoice {
  uint8_t function;  // 0=tonic, 1=third, 2=fifth
  float weight;
};
constexpr TerminalChoice kHappyTerminals[3] = {
    {0, 0.5f},  // tonic
    {1, 0.3f},  // third
    {2, 0.2f},  // fifth
};

// Sad ends on the tonic only. The minor third sits ABOVE the tonic, so a
// third-ending lifts the phrase at the exact moment it should settle —
// bench 2026-08-04 produced 1568 -> 1865 Hz, rising into the final note.
// Sadness needs the line to come to rest, not to lift.
constexpr TerminalChoice kSadTerminals[1] = {{0, 1.0f}};

// Mode third: happy = major third (4 semitones), sad = minor third (3).
constexpr int8_t kModeThirdHappy = 4;
constexpr int8_t kModeThirdSad = 3;

// Fixed offsets for terminal functions 0 (tonic) and 2 (fifth).
constexpr int8_t kFunctionOffsetTonic = 0;
constexpr int8_t kFunctionOffsetFifth = 7;

// Walk rules per mood (happy/sad only — confused takes a different path).
struct WalkRules {
  int8_t stepMax;
  int8_t leapMin;
  int8_t leapMax;
  uint8_t maxLeaps;
  float windowWidth;
  float theta;
  float ewmaAlpha;
};

constexpr WalkRules kWalkRulesHappy = {
    /*stepMax=*/4,
    /*leapMin=*/5,
    /*leapMax=*/7,
    /*maxLeaps=*/1,
    /*windowWidth=*/6.0f,
    /*theta=*/0.62f,
    /*ewmaAlpha=*/0.48f,
};

// Sad: tighter window and stronger pull toward the descending contour.
// Bench 2026-08-04: a wandering minor line in a high register was heard as
// "haunted house, more scary than sad". Wandering reads as eerie; a decisive
// fall reads as a sigh.
constexpr WalkRules kWalkRulesSad = {
    /*stepMax=*/3,
    /*leapMin=*/4,
    /*leapMax=*/5,
    /*maxLeaps=*/1,
    /*windowWidth=*/3.0f,
    /*theta=*/0.78f,
    /*ewmaAlpha=*/0.30f,
};

inline const WalkRules& walkRulesFor(Mood mood) {
  return mood == Mood::Happy ? kWalkRulesHappy : kWalkRulesSad;
}

inline int8_t modeThird(Mood mood) {
  return mood == Mood::Happy ? kModeThirdHappy : kModeThirdSad;
}

// ---------------------------------------------------------------------------
// PRNG — small, fast, deterministic, well-distributed.
//
// PCG32 (Melissa O'Neill, permuted congruential generator). Chosen over
// MT19937 because the Python source uses MT19937 with a specific weighted
// choice implementation; matching THAT bit-for-bit is fragile and buys
// nothing musically (phrases are random by design). PCG32 has a small
// state, a strong statistical profile, and a single-instruction advance.
//
// SplitMix64 is used to derive per-retry seeds from a single 64-bit key.
// ---------------------------------------------------------------------------

inline uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

class Pcg32 {
 public:
  explicit Pcg32(uint64_t seed) {
    // Standard PCG32 seeding: mix the seed into both state and the
    // odd-numbered stream identifier so two nearby seeds diverge quickly.
    uint64_t s = splitmix64(seed ^ 0xd1342543de82ef95ULL);
    state_ = 0u;
    inc_ = (splitmix64(seed + 1) << 1u) | 1u;
    advance();
    state_ += s;
    advance();
  }

  uint32_t next() {
    const uint64_t oldstate = state_;
    advance();
    const uint32_t xorshifted =
        static_cast<uint32_t>(((oldstate >> 18u) ^ oldstate) >> 27u);
    const uint32_t rot = static_cast<uint32_t>(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-static_cast<int32_t>(rot)) & 31));
  }

  // Uniform integer in [0, n). Uses Lemire's debiased multiply so no bias
  // creeps in for arbitrary n.
  uint32_t below(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = next();
    uint64_t m = static_cast<uint64_t>(x) * static_cast<uint64_t>(n);
    uint32_t l = static_cast<uint32_t>(m);
    if (l < n) {
      const uint32_t t = static_cast<uint32_t>(-static_cast<int32_t>(n)) % n;
      while (l < t) {
        x = next();
        m = static_cast<uint64_t>(x) * static_cast<uint64_t>(n);
        l = static_cast<uint32_t>(m);
      }
    }
    return static_cast<uint32_t>(m >> 32);
  }

  // Inclusive both ends: [lo, hi].
  int32_t randint(int32_t lo, int32_t hi) {
    if (hi < lo) return lo;
    return lo + static_cast<int32_t>(below(static_cast<uint32_t>(hi - lo + 1)));
  }

  float uniform01() {
    // 24 high bits divided by 2^24 gives a well-spaced float in [0,1).
    return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f);
  }

  float uniformf(float lo, float hi) {
    return lo + uniform01() * (hi - lo);
  }

  uint64_t next64() {
    const uint64_t hi = next();
    const uint64_t lo = next();
    return (hi << 32) | lo;
  }

 private:
  void advance() {
    state_ = state_ * 6364136223846793005ULL + inc_;
  }

  uint64_t state_;
  uint64_t inc_;
};

// ---------------------------------------------------------------------------
// Small utilities.
// ---------------------------------------------------------------------------

inline int8_t pitchClass(int8_t midi) {
  int8_t pc = static_cast<int8_t>((midi - kTonicMidi) % 12);
  if (pc < 0) pc = static_cast<int8_t>(pc + 12);
  return pc;
}

inline bool bandContains(int8_t midi) {
  return midi >= kBandLowMidi && midi <= kBandHighMidi;
}

inline bool maskContains(uint16_t mask, int8_t pitchClassValue) {
  return (mask & (uint16_t)(1u << pitchClassValue)) != 0;
}

inline uint16_t midiToHz(int8_t midi) {
  // Equal-tempered, A4=440. Rounded to integer Hz because the firmware `tone`
  // command takes an integer.
  const float hz = 440.0f * powf(2.0f, static_cast<float>(midi - 69) / 12.0f);
  return static_cast<uint16_t>(hz + 0.5f);
}

// Pick a candidate honouring a cumulative-weight distribution. A zero weight
// is genuinely excluded (matching Python `random.choices`). Returns the
// index in [0, count) or 0 if everything is degenerate.
uint8_t weightedChoice(Pcg32& rng, const float* weights, uint8_t count) {
  float total = 0.0f;
  for (uint8_t i = 0; i < count; ++i) {
    if (weights[i] > 0.0f) total += weights[i];
  }
  if (total <= 0.0f) return 0;
  const float r = rng.uniform01() * total;
  float cum = 0.0f;
  for (uint8_t i = 0; i < count; ++i) {
    if (weights[i] <= 0.0f) continue;
    cum += weights[i];
    if (r < cum) return i;
  }
  // Fell through due to floating-point drift; return the last non-zero index.
  for (int i = static_cast<int>(count) - 1; i >= 0; --i) {
    if (weights[i] > 0.0f) return static_cast<uint8_t>(i);
  }
  return 0;
}

// Tent-shaped weights peaking at `centre` and reaching zero at the edges.
// Bounded support: no candidate beyond half_width can be proposed at all,
// so a truncated normal's extra clamp step is not needed.
void triangularWeights(const int8_t* candidates,
                       uint8_t count,
                       float centre,
                       float halfWidth,
                       float* weightsOut) {
  if (halfWidth <= 0.0f) {
    for (uint8_t i = 0; i < count; ++i) weightsOut[i] = 0.0f;
    return;
  }
  for (uint8_t i = 0; i < count; ++i) {
    const float d = fabsf(static_cast<float>(candidates[i]) - centre);
    const float w = 1.0f - d / halfWidth;
    weightsOut[i] = w > 0.0f ? w : 0.0f;
  }
}

// Damp specific candidates, used to stop audible note repetition.
// Weight damping is preferred over rejection sampling because it is bounded:
// rejection can loop indefinitely when the musical rules leave only the
// banned candidate legal. If everything would be banned, fall back to the
// original weights — a repeated note beats no note.
void suppressCandidate(const int8_t* candidates,
                       uint8_t count,
                       int8_t bannedValue,
                       float factor,
                       float* weights) {
  bool anyKept = false;
  float remaining = 0.0f;
  for (uint8_t i = 0; i < count; ++i) {
    if (candidates[i] != bannedValue && weights[i] > 0.0f) {
      anyKept = true;
      remaining += weights[i];
    }
  }
  if (!anyKept || remaining <= 0.0f) return;  // fall back to original weights
  for (uint8_t i = 0; i < count; ++i) {
    if (candidates[i] == bannedValue) weights[i] *= factor;
  }
}

// Piecewise-linear interpolation. `points` is sorted ascending in x.
float interpolate(const float (*points)[2], uint8_t count, float progress) {
  for (uint8_t i = 0; i + 1 < count; ++i) {
    const float leftX = points[i][0];
    const float rightX = points[i + 1][0];
    if (progress <= rightX) {
      const float span = rightX - leftX;
      const float frac = span == 0.0f ? 0.0f : (progress - leftX) / span;
      return points[i][1] + frac * (points[i + 1][1] - points[i][1]);
    }
  }
  return points[count - 1][1];
}

// ---------------------------------------------------------------------------
// Pitch material — the allowed pitches for a given mood, in ascending order.
// ---------------------------------------------------------------------------
struct PitchMaterial {
  int8_t allowed[32];  // up to 17 for happy/sad within 79..107; 29 for confused
  uint8_t count;
  uint16_t mask;
};

void buildPitchMaterial(Mood mood, PitchMaterial& out) {
  uint16_t mask;
  switch (mood) {
    case Mood::Happy: mask = kMajorMask; break;
    case Mood::Sad:   mask = kMinorMask; break;
    default:          mask = kConfusedMask; break;
  }
  out.mask = mask;
  out.count = 0;
  for (int8_t midi = kBandLowMidi; midi <= kBandHighMidi; ++midi) {
    if (maskContains(mask, pitchClass(midi))) {
      out.allowed[out.count++] = midi;
    }
  }
}

// ---------------------------------------------------------------------------
// Backward reachability (memoised `can_finish`).
//
// State: (currentIndex, index, missingBitmask, leapsUsed).
//   currentIndex: 0..allowedCount, with allowedCount meaning "no current
//                 pitch yet" (index 0 of the phrase).
//   index:        0..length.
//   missing:      2-bit mask. bit0 = tonic pitch class still owed; bit1 =
//                 third pitch class still owed.
//   leaps:        0 or 1. Both moods cap max_leaps at 1.
//
// (`foreignUsed` is always 0 in this port — `_pitch_material` in Python
// returns an empty `foreign_classes` set, so every allowed pitch is native
// to the mask. Dropping the axis makes the memoisation four-dimensional
// instead of five.)
// ---------------------------------------------------------------------------

constexpr uint16_t kMaxAllowed = 32;
constexpr uint16_t kMaxLength = kMaxPhraseNotes + 1;

// Tri-state: 0 = unknown, 1 = false, 2 = true.
struct Memo {
  uint8_t table[(kMaxAllowed + 1) * kMaxLength * 4 * 2];

  void reset() { memset(table, 0, sizeof(table)); }

  uint8_t& at(uint8_t currentIdx, uint8_t index, uint8_t missing, uint8_t leaps) {
    const size_t k = (((static_cast<size_t>(currentIdx) * kMaxLength) + index) * 4 + missing) * 2 + leaps;
    return table[k];
  }
};

// Walk state and immutable parameters passed through the reachability search.
struct WalkContext {
  const WalkRules* rules;
  PitchMaterial* material;
  int8_t terminalMidi;
  uint8_t length;
  uint8_t deadline;
  int8_t requiredTonicPc;
  int8_t requiredThirdPc;
  // Which allowed[] index corresponds to the terminal.
  uint8_t terminalIdx;
};

inline uint8_t missingFor(bool tonicMissing, bool thirdMissing) {
  return static_cast<uint8_t>((tonicMissing ? 1 : 0) | (thirdMissing ? 2 : 0));
}

// A single "legal move" during the walk. next_missing is compact.
struct LegalMove {
  int8_t candidate;
  uint8_t nextMissing;
  uint8_t nextLeaps;
  uint8_t allowedIdx;  // index into material->allowed
};

// Enumerate legal candidates given the current state. Returns the number of
// moves written into `out` (bounded by material.count).
uint8_t legalOptions(const WalkContext& ctx,
                     int8_t current,
                     bool haveCurrent,
                     uint8_t index,
                     uint8_t missing,
                     uint8_t leapsUsed,
                     LegalMove* out) {
  uint8_t count = 0;
  if (index >= ctx.length) return 0;

  const bool isTerminal = (index == ctx.length - 1);
  const uint8_t poolStart = isTerminal ? ctx.terminalIdx : 0u;
  const uint8_t poolEnd = isTerminal
                              ? static_cast<uint8_t>(ctx.terminalIdx + 1u)
                              : ctx.material->count;

  for (uint8_t i = poolStart; i < poolEnd; ++i) {
    const int8_t candidate = ctx.material->allowed[i];
    const int8_t pc = pitchClass(candidate);

    uint8_t nextLeaps = leapsUsed;
    if (haveCurrent) {
      const int8_t dist = static_cast<int8_t>(candidate > current
                                                  ? candidate - current
                                                  : current - candidate);
      if (dist > ctx.rules->stepMax) {
        if (dist < ctx.rules->leapMin || dist > ctx.rules->leapMax ||
            leapsUsed >= ctx.rules->maxLeaps) {
          continue;
        }
        nextLeaps = static_cast<uint8_t>(nextLeaps + 1);
      }
    }

    uint8_t nextMissing = missing;
    if (pc == ctx.requiredTonicPc) nextMissing &= static_cast<uint8_t>(~1u);
    if (pc == ctx.requiredThirdPc) nextMissing &= static_cast<uint8_t>(~2u);

    if (index >= ctx.deadline && nextMissing != 0) continue;

    out[count].candidate = candidate;
    out[count].nextMissing = nextMissing;
    out[count].nextLeaps = nextLeaps;
    out[count].allowedIdx = i;
    ++count;
  }
  return count;
}

bool canFinish(const WalkContext& ctx,
               Memo& memo,
               int8_t current,
               bool haveCurrent,
               uint8_t index,
               uint8_t missing,
               uint8_t leapsUsed) {
  if (index == ctx.length) {
    return haveCurrent && current == ctx.terminalMidi && missing == 0;
  }

  const uint8_t currentIdx =
      haveCurrent
          ? [&]() -> uint8_t {
              for (uint8_t i = 0; i < ctx.material->count; ++i) {
                if (ctx.material->allowed[i] == current) return i;
              }
              return ctx.material->count;
            }()
          : ctx.material->count;

  uint8_t& memoCell = memo.at(currentIdx, index, missing, leapsUsed);
  if (memoCell != 0) {
    return memoCell == 2;
  }

  LegalMove moves[kMaxAllowed];
  const uint8_t n = legalOptions(ctx, current, haveCurrent, index, missing,
                                 leapsUsed, moves);
  for (uint8_t i = 0; i < n; ++i) {
    if (canFinish(ctx, memo, moves[i].candidate, true,
                  static_cast<uint8_t>(index + 1), moves[i].nextMissing,
                  moves[i].nextLeaps)) {
      memoCell = 2;
      return true;
    }
  }
  memoCell = 1;
  return false;
}

// ---------------------------------------------------------------------------
// Contour targets.
// ---------------------------------------------------------------------------

float macroTarget(Mood mood, float progress, int8_t terminalMidi) {
  const float targetOffset = static_cast<float>(terminalMidi - kTonicMidi);
  float offset;
  if (mood == Mood::Happy) {
    const float pts[4][2] = {
        {0.0f, 2.0f}, {0.28f, 1.0f}, {0.72f, 10.0f}, {1.0f, targetOffset}};
    offset = interpolate(pts, 4, progress);
  } else {
    const float opening = fminf(12.0f, targetOffset + 3.5f);
    const float peak = fminf(12.0f, targetOffset + 5.5f);
    const float falling = fminf(12.0f, targetOffset + 2.5f);
    const float pts[4][2] = {
        {0.0f, opening}, {0.24f, peak}, {0.62f, falling}, {1.0f, targetOffset}};
    offset = interpolate(pts, 4, progress);
  }
  if (progress >= kResolutionStart) {
    const float resolution =
        (progress - kResolutionStart) / (1.0f - kResolutionStart);
    offset = (1.0f - 0.65f * resolution) * offset + 0.65f * resolution * targetOffset;
  }
  return static_cast<float>(kTonicMidi) + offset;
}

float windowWidth(float baseWidth, float progress) {
  if (progress < kResolutionStart) return baseWidth;
  const float resolution =
      (progress - kResolutionStart) / (1.0f - kResolutionStart);
  return baseWidth + (1.35f - baseWidth) * resolution;
}

// ---------------------------------------------------------------------------
// Weight shaping and reversal bias.
// ---------------------------------------------------------------------------

void applyReversalBias(Mood mood,
                       const int8_t* candidates,
                       float* weights,
                       uint8_t count,
                       const int8_t* chosen,
                       uint8_t chosenLen) {
  (void)mood;
  if (chosenLen < 2) return;
  const int8_t lastInterval =
      static_cast<int8_t>(chosen[chosenLen - 1] - chosen[chosenLen - 2]);
  if (lastInterval == 0) return;
  const int8_t direction = lastInterval > 0 ? 1 : -1;

  bool sawUp = false;
  bool sawDown = false;
  const int8_t last = chosen[chosenLen - 1];
  for (uint8_t i = 0; i < count; ++i) {
    if (candidates[i] == last) continue;
    if (candidates[i] > last) sawUp = true;
    else sawDown = true;
  }
  // Only bias a reversal when continuation was also genuinely available.
  // Near a band edge the candidate filter already points inward, so adding a
  // second "reversal" bonus there would double-count the boundary.
  if (!(sawUp && sawDown)) return;

  const int8_t absLast = lastInterval > 0 ? lastInterval : static_cast<int8_t>(-lastInterval);
  if (absLast < kPostSkipSemitones) return;

  const float reverseFactor = 2.25f;
  const float continueFactor = 0.62f;
  for (uint8_t i = 0; i < count; ++i) {
    const int8_t move = static_cast<int8_t>(candidates[i] - last);
    if (move == 0) continue;
    if ((move > 0 && direction < 0) || (move < 0 && direction > 0)) {
      weights[i] *= reverseFactor;
    } else {
      weights[i] *= continueFactor;
    }
  }
}

void shapeCandidateWeights(Mood mood,
                           const int8_t* candidates,
                           float* weights,
                           uint8_t count,
                           const int8_t* chosen,
                           uint8_t chosenLen,
                           uint8_t missing,
                           uint8_t deadline,
                           uint8_t index,
                           int8_t terminalMidi,
                           float progress,
                           int8_t requiredTonicPc,
                           int8_t requiredThirdPc) {
  for (uint8_t i = 0; i < count; ++i) {
    if (chosenLen > 0) {
      const int8_t d = static_cast<int8_t>(
          candidates[i] > chosen[chosenLen - 1]
              ? candidates[i] - chosen[chosenLen - 1]
              : chosen[chosenLen - 1] - candidates[i]);
      float moveFactor = 1.0f;
      if (mood == Mood::Happy) {
        if (d == 0) moveFactor = 0.70f;
        else if (d <= 2) moveFactor = 1.35f;
        else if (d <= 4) moveFactor = 1.0f;
        else moveFactor = 0.26f;
      } else {  // sad
        if (d == 0) moveFactor = 0.72f;
        else if (d <= 2) moveFactor = 1.45f;
        else if (d <= 3) moveFactor = 1.0f;
        else moveFactor = 0.14f;
      }
      weights[i] *= moveFactor;
    }

    const int8_t pc = pitchClass(candidates[i]);
    const bool wantsTonic = (missing & 1u) && pc == requiredTonicPc;
    const bool wantsThird = (missing & 2u) && pc == requiredThirdPc;
    if (index <= deadline && (wantsTonic || wantsThird)) {
      weights[i] *= 2.4f;
    }
  }

  applyReversalBias(mood, candidates, weights, count, chosen, chosenLen);

  if (chosenLen > 0 && progress >= kResolutionStart) {
    const float resolution =
        (progress - kResolutionStart) / (1.0f - kResolutionStart);
    const int8_t last = chosen[chosenLen - 1];
    const int8_t oldDistance = static_cast<int8_t>(
        last > terminalMidi ? last - terminalMidi : terminalMidi - last);
    for (uint8_t i = 0; i < count; ++i) {
      const int8_t newDistance = static_cast<int8_t>(
          candidates[i] > terminalMidi
              ? candidates[i] - terminalMidi
              : terminalMidi - candidates[i]);
      if (newDistance < oldDistance) {
        weights[i] *= 1.0f + 2.2f * resolution;
      } else if (newDistance > oldDistance) {
        const float factor = 1.0f - 0.55f * resolution;
        weights[i] *= factor > 0.35f ? factor : 0.35f;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Constrained walk (happy/sad).
// ---------------------------------------------------------------------------

bool generatePitchWalk(Mood mood,
                       uint8_t length,
                       int8_t terminalMidi,
                       Pcg32& rng,
                       int8_t* midiOut,
                       uint8_t& midiCount) {
  PitchMaterial material;
  buildPitchMaterial(mood, material);

  // Locate terminal in the allowed set.
  uint8_t terminalIdx = material.count;
  for (uint8_t i = 0; i < material.count; ++i) {
    if (material.allowed[i] == terminalMidi) {
      terminalIdx = i;
      break;
    }
  }
  if (terminalIdx == material.count) return false;

  const WalkRules& rules = walkRulesFor(mood);
  const int8_t third = modeThird(mood);
  const uint8_t deadline = static_cast<uint8_t>(length - 1 < 3 ? length - 1 : 3);

  WalkContext ctx;
  ctx.rules = &rules;
  ctx.material = &material;
  ctx.terminalMidi = terminalMidi;
  ctx.length = length;
  ctx.deadline = deadline;
  ctx.requiredTonicPc = 0;
  ctx.requiredThirdPc = third;
  ctx.terminalIdx = terminalIdx;

  static Memo memo;
  memo.reset();

  const uint8_t initialMissing = missingFor(true, true);
  if (!canFinish(ctx, memo, 0, false, 0, initialMissing, 0)) {
    return false;
  }

  int8_t chosen[kMaxPhraseNotes];
  uint8_t chosenLen = 0;
  int8_t current = 0;
  bool haveCurrent = false;
  uint8_t missing = initialMissing;
  uint8_t leapsUsed = 0;
  float historyCentre = 0.0f;
  bool havehistoryCentre = false;

  for (uint8_t index = 0; index < length; ++index) {
    LegalMove moves[kMaxAllowed];
    const uint8_t n = legalOptions(ctx, current, haveCurrent, index, missing,
                                   leapsUsed, moves);
    int8_t viable[kMaxAllowed];
    LegalMove viableMoves[kMaxAllowed];
    uint8_t viableCount = 0;
    for (uint8_t i = 0; i < n; ++i) {
      if (canFinish(ctx, memo, moves[i].candidate, true,
                    static_cast<uint8_t>(index + 1), moves[i].nextMissing,
                    moves[i].nextLeaps)) {
        viable[viableCount] = moves[i].candidate;
        viableMoves[viableCount] = moves[i];
        ++viableCount;
      }
    }
    if (viableCount == 0) return false;

    const float progress =
        length == 1 ? 0.0f : static_cast<float>(index) / static_cast<float>(length - 1);
    const float macro = macroTarget(mood, progress, terminalMidi);
    if (!havehistoryCentre) {
      historyCentre = macro;
      havehistoryCentre = true;
    }

    const float windowCentre =
        historyCentre + rules.theta * (macro - historyCentre);
    const float halfWidth = windowWidth(rules.windowWidth, progress);

    float weights[kMaxAllowed];
    triangularWeights(viable, viableCount, windowCentre, halfWidth, weights);

    float sumW = 0.0f;
    for (uint8_t i = 0; i < viableCount; ++i) sumW += weights[i];
    if (sumW <= 0.0f) {
      // Reachability can occasionally leave every legal move outside the
      // local window. In that explicit conflict, steer toward the ending;
      // otherwise zero-weight notes stay filtered rather than revived.
      for (uint8_t i = 0; i < viableCount; ++i) {
        const int8_t d = static_cast<int8_t>(
            viable[i] > terminalMidi ? viable[i] - terminalMidi
                                     : terminalMidi - viable[i]);
        weights[i] = 1.0f / (1.0f + static_cast<float>(d));
      }
    }

    shapeCandidateWeights(mood, viable, weights, viableCount, chosen, chosenLen,
                          missing, deadline, index, terminalMidi, progress,
                          ctx.requiredTonicPc, ctx.requiredThirdPc);

    if (haveCurrent) {
      suppressCandidate(viable, viableCount, current, 0.08f, weights);
    }

    const uint8_t pickIdx = weightedChoice(rng, weights, viableCount);
    const int8_t selected = viable[pickIdx];
    const LegalMove& selectedMove = viableMoves[pickIdx];

    chosen[chosenLen++] = selected;
    current = selected;
    haveCurrent = true;
    missing = selectedMove.nextMissing;
    leapsUsed = selectedMove.nextLeaps;

    if (index == 0) {
      historyCentre = static_cast<float>(selected);
    } else {
      historyCentre = rules.ewmaAlpha * static_cast<float>(selected) +
                      (1.0f - rules.ewmaAlpha) * historyCentre;
    }
  }

  for (uint8_t i = 0; i < chosenLen; ++i) midiOut[i] = chosen[i];
  midiCount = chosenLen;
  return true;
}

// ---------------------------------------------------------------------------
// Two-note gestures. Two pitches cannot establish a tonal centre in the
// walk's sense, so happy/sad are hardcoded.
// ---------------------------------------------------------------------------
void twoNoteGesture(Mood mood,
                    int8_t* midi,
                    uint8_t& count,
                    uint8_t& terminalFn,
                    int8_t& terminalMidi) {
  if (mood == Mood::Happy) {
    const int8_t t = static_cast<int8_t>(kTonicMidi + kModeThirdHappy);
    midi[0] = kTonicMidi;
    midi[1] = t;
    count = 2;
    terminalFn = 1;  // third
    terminalMidi = t;
  } else {
    midi[0] = static_cast<int8_t>(kTonicMidi + kModeThirdSad);
    midi[1] = kTonicMidi;
    count = 2;
    terminalFn = 0;  // tonic
    terminalMidi = kTonicMidi;
  }
}

// ---------------------------------------------------------------------------
// Rhythm (happy/sad).
// ---------------------------------------------------------------------------

struct RhythmNote {
  int8_t midi;
  uint16_t gateMs;
  uint16_t gapMs;
};

void makeRhythm(Mood mood,
                const int8_t* midi,
                uint8_t count,
                Pcg32& rng,
                RhythmNote* out,
                uint8_t& outCount) {
  // BENCH-DRIVEN OVERRIDE of the research tempo values.
  //
  // The literature says slow tempo is the strongest sadness cue, but that
  // evidence comes from instruments with timbre and natural decay. On a
  // bare square wave a 700 ms note rings like a theremin, and the bench
  // verdict was "haunted house, more scary than sad". Slow simply is not
  // available to us as a sad cue.
  //
  // So every mood now lives in the fast, chirp-like register. Sadness is
  // carried by DESCENDING CONTOUR and MINOR INTERVALS instead. Sad stays
  // relatively the slowest to keep an ordering, but reads as a chirp
  // rather than a drone.
  int32_t ioiLo, ioiHi;
  float ratioLo, ratioHi;
  if (mood == Mood::Happy) {
    ioiLo = 95;
    ioiHi = 155;
    ratioLo = 0.40f;
    ratioHi = 0.62f;
  } else {
    ioiLo = 170;
    ioiHi = 260;
    ratioLo = 0.50f;
    ratioHi = 0.68f;
  }

  uint16_t gates[kMaxPhraseNotes];
  uint16_t iois[kMaxPhraseNotes];
  for (uint8_t i = 0; i < count; ++i) {
    const int32_t ioi = rng.randint(ioiLo, ioiHi);
    const float ratio = rng.uniformf(ratioLo, ratioHi);
    iois[i] = static_cast<uint16_t>(ioi);
    int32_t gate = static_cast<int32_t>(static_cast<float>(ioi) * ratio + 0.5f);
    if (gate < static_cast<int32_t>(kMinNoteMs)) gate = kMinNoteMs;
    gates[i] = static_cast<uint16_t>(gate);
  }

  // Terminal held longer so the phrase actually settles rather than ending
  // mid-chirp.
  if (mood == Mood::Happy) {
    const float k = rng.uniformf(1.5f, 2.0f);
    gates[count - 1] = static_cast<uint16_t>(
        static_cast<float>(gates[count - 1]) * k + 0.5f);
  } else {
    gates[count - 1] = static_cast<uint16_t>(
        static_cast<float>(gates[count - 1]) * 1.5f + 0.5f);
  }

  for (uint8_t i = 0; i < count; ++i) {
    out[i].midi = midi[i];
    out[i].gateMs = gates[i];
    if (i == count - 1) {
      out[i].gapMs = 0;
    } else {
      const int32_t g = static_cast<int32_t>(iois[i]) - static_cast<int32_t>(gates[i]);
      out[i].gapMs = static_cast<uint16_t>(g > 0 ? g : 0);
      if (midi[i] == midi[i + 1] && out[i].gapMs < kMinRearticulationGapMs) {
        out[i].gapMs = kMinRearticulationGapMs;
      }
    }
  }
  outCount = count;
}

// Collapse a repeated closing pitch into one held note. Bench 2026-08-04:
// striking the tonic twice as a resolution read as a stutter — Nick asked
// for "2 beats continuous" instead.
void mergeRepeatedFinal(RhythmNote* notes, uint8_t& count) {
  while (count >= 2 && notes[count - 1].midi == notes[count - 2].midi) {
    const RhythmNote last = notes[count - 1];
    RhythmNote prev = notes[count - 2];
    prev.gateMs = static_cast<uint16_t>(static_cast<uint32_t>(prev.gateMs) +
                                       prev.gapMs + last.gateMs);
    prev.gapMs = last.gapMs;
    notes[count - 2] = prev;
    --count;
  }
}

// ---------------------------------------------------------------------------
// Confused path.
// ---------------------------------------------------------------------------

// Sample candidate closing gestures, as offsets from the last body note.
// Up to a small number per gesture (Python samples with a set; we use fixed
// arrays and deduplicate manually).
struct ConfusedTail {
  int8_t offsets[2];
  uint8_t count;  // 1 or 2
};

constexpr uint8_t kMaxConfusedTails = 12;

void confusedTailOffsets(ConfusedEnding ending,
                         Pcg32& rng,
                         ConfusedTail* out,
                         uint8_t& outCount) {
  outCount = 0;
  auto tryAdd = [&](const ConfusedTail& t) {
    for (uint8_t i = 0; i < outCount; ++i) {
      if (out[i].count != t.count) continue;
      bool same = true;
      for (uint8_t j = 0; j < t.count; ++j) {
        if (out[i].offsets[j] != t.offsets[j]) {
          same = false;
          break;
        }
      }
      if (same) return;
    }
    if (outCount < kMaxConfusedTails) out[outCount++] = t;
  };

  switch (ending) {
    case ConfusedEnding::Insist: {
      // Repeat the pitch, then bite off a small interval. Repetition is the
      // insistence — the sound of asking again — and a semitone bite is
      // sourer than the tone around it. This is the frustrated reading.
      for (uint8_t k = 0; k < 10; ++k) {
        const int32_t r = rng.randint(0, 2);
        const uint8_t repeats = (r == 2) ? 2 : 1;
        // rng.choice((1,1,2,3)) — weighted toward 1.
        const int32_t drollRoll = rng.randint(0, 3);
        const int8_t drop = drollRoll <= 1 ? 1 : (drollRoll == 2 ? 2 : 3);
        ConfusedTail t;
        t.count = static_cast<uint8_t>(repeats + 1);
        // repeats == 1 or 2. offsets: repeats zeroes, then -drop.
        t.offsets[0] = (repeats == 2) ? 0 : 0;
        if (repeats == 2) {
          // t needs 3 elements, but we only allocate 2. Constrain repeats
          // to 1 by pushing the "1,1,2" tuple toward the shorter case: use
          // repeats=1 always when tail length would exceed 2. This is a
          // faithful narrowing — 2-repeat + drop is the same phonetic
          // shape as 1-repeat + drop.
          t.count = 2;
          t.offsets[0] = 0;
          t.offsets[1] = static_cast<int8_t>(-drop);
        } else {
          t.offsets[0] = 0;
          t.offsets[1] = static_cast<int8_t>(-drop);
        }
        tryAdd(t);
      }
      break;
    }
    case ConfusedEnding::Rise: {
      for (uint8_t k = 0; k < 10; ++k) {
        ConfusedTail t;
        t.count = 1;
        t.offsets[0] = static_cast<int8_t>(rng.randint(2, 5));
        tryAdd(t);
      }
      break;
    }
    case ConfusedEnding::Falter: {
      // Dip, then lift back part-way: the "wait... what?" double gesture.
      for (uint8_t k = 0; k < 12; ++k) {
        const int8_t dip = static_cast<int8_t>(rng.randint(1, 4));
        const int8_t lift = static_cast<int8_t>(rng.randint(2, 5));
        ConfusedTail t;
        t.count = 2;
        t.offsets[0] = static_cast<int8_t>(-dip);
        t.offsets[1] = static_cast<int8_t>(-dip + lift);
        tryAdd(t);
      }
      break;
    }
    case ConfusedEnding::Trail: {
      // Settle lower and stop short, tailing off rather than asking.
      for (uint8_t k = 0; k < 10; ++k) {
        const int8_t first = static_cast<int8_t>(rng.randint(1, 5));
        if (rng.uniform01() < 0.4f) {
          ConfusedTail t;
          t.count = 2;
          t.offsets[0] = static_cast<int8_t>(-first);
          t.offsets[1] = static_cast<int8_t>(-first - rng.randint(1, 3));
          tryAdd(t);
        } else {
          ConfusedTail t;
          t.count = 1;
          t.offsets[0] = static_cast<int8_t>(-first);
          tryAdd(t);
        }
      }
      break;
    }
  }
}

// Build a confused body of exactly `bodyLength` offsets from the anchor.
//
// Constraints, each from a bench finding:
//   * steps of +/-1, +/-2 or +/-3 only — a +/-6 move is a tritone and reads
//     as menacing rather than puzzled;
//   * never monotonically ascending — with the closing gesture that becomes
//     one long ramp and reads as a fanfare;
//   * must end below its own peak, so a rising gesture stays a local lift
//     rather than a triumphant climb;
//   * compact span and few turns, since wide leaps and constant reversals
//     read as agitation.
bool confusedBodyOffsets(uint8_t bodyLength,
                         Pcg32& rng,
                         int8_t* offsets,
                         uint8_t& outLen) {
  if (bodyLength == 0) bodyLength = 1;
  if (bodyLength == 1) {
    offsets[0] = 0;
    outLen = 1;
    return true;
  }

  for (uint8_t attempt = 0; attempt < 60; ++attempt) {
    offsets[0] = 0;
    int8_t direction = rng.randint(0, 1) == 0 ? -1 : 1;
    for (uint8_t i = 1; i < bodyLength; ++i) {
      // Momentum: mostly continue in the current direction, occasionally
      // turn. Choosing each step's sign independently reverses about half
      // the time, which zigzags — that reads as agitation rather than
      // searching. Runs with occasional turns read as looking around.
      if (rng.uniform01() < 0.35f) direction = static_cast<int8_t>(-direction);
      // Semitone steps are the sour ones and carry the frustration; whole
      // tones keep the line moving; the occasional minor third stops it
      // becoming a chromatic crawl.
      const float roll = rng.uniform01();
      const int8_t size = roll < 0.45f ? 1 : (roll < 0.85f ? 2 : 3);
      offsets[i] = static_cast<int8_t>(offsets[i - 1] + direction * size);
    }

    int8_t lo = offsets[0];
    int8_t hi = offsets[0];
    for (uint8_t i = 1; i < bodyLength; ++i) {
      if (offsets[i] < lo) lo = offsets[i];
      if (offsets[i] > hi) hi = offsets[i];
    }
    if (hi - lo > kConfusedMaxBodySpan) continue;

    bool ascending = true;
    for (uint8_t i = 1; i < bodyLength; ++i) {
      if (offsets[i] <= offsets[i - 1]) { ascending = false; break; }
    }
    if (ascending) continue;

    if (static_cast<int8_t>(hi - offsets[bodyLength - 1]) < kConfusedRiseMin) continue;

    // Reject a body that oscillates between two pitches (mechanical
    // alternation rather than searching).
    uint8_t distinct = 0;
    int8_t seen[16];
    for (uint8_t i = 0; i < bodyLength; ++i) {
      bool found = false;
      for (uint8_t j = 0; j < distinct; ++j) {
        if (seen[j] == offsets[i]) { found = true; break; }
      }
      if (!found) seen[distinct++] = offsets[i];
    }
    const uint8_t requiredDistinct = bodyLength < 3 ? bodyLength : 3;
    if (distinct < requiredDistinct) continue;

    // Require at least one semitone clash once there is room for it.
    if (bodyLength >= 3) {
      bool hasSemitone = false;
      for (uint8_t i = 1; i < bodyLength; ++i) {
        const int8_t step = static_cast<int8_t>(offsets[i] - offsets[i - 1]);
        if (step == 1 || step == -1) { hasSemitone = true; break; }
      }
      if (!hasSemitone) continue;
    }

    // Reject an over-turning contour outright.
    if (bodyLength >= 4) {
      uint8_t turns = 0;
      int8_t prevDir = 0;
      for (uint8_t i = 1; i < bodyLength; ++i) {
        const int8_t d = offsets[i] > offsets[i - 1] ? 1 : -1;
        if (prevDir != 0 && d != prevDir) ++turns;
        prevDir = d;
      }
      const uint8_t maxTurns = (bodyLength - 1) >= 3
                                   ? static_cast<uint8_t>((bodyLength - 2) / 2)
                                   : 2;
      const uint8_t allowed = maxTurns > 2 ? maxTurns : 2;
      if (turns > allowed) continue;
    }

    outLen = bodyLength;
    return true;
  }

  // Deterministic fallback for the rare draw where 60 attempts all fail.
  offsets[0] = 0;
  uint8_t idx = 1;
  if (bodyLength >= 3 && idx < bodyLength) {
    offsets[idx++] = 1;  // a semitone up for the sour clash
  }
  while (idx < bodyLength) {
    const int8_t step = (idx % 2 == 1) ? -1 : -2;
    offsets[idx] = static_cast<int8_t>(offsets[idx - 1] + step);
    ++idx;
  }
  // Guarantee headroom for the rise.
  int8_t peak = offsets[0];
  for (uint8_t i = 1; i < bodyLength; ++i) {
    if (offsets[i] > peak) peak = offsets[i];
  }
  const int8_t shortfall = static_cast<int8_t>(kConfusedRiseMin - (peak - offsets[bodyLength - 1]));
  if (shortfall > 0) {
    offsets[bodyLength - 1] = static_cast<int8_t>(offsets[bodyLength - 1] - shortfall);
  }
  outLen = bodyLength;
  return true;
}

// Confused rhythm: normal pace except for one deliberate hesitation.
void confusedRhythm(const int8_t* midi,
                    uint8_t count,
                    ConfusedEnding ending,
                    Pcg32& rng,
                    RhythmNote* out,
                    uint8_t& outCount) {
  int32_t hesitationIndex;
  if (count == 2) {
    hesitationIndex = -1;
  } else if (count == 3) {
    hesitationIndex = 0;
  } else {
    hesitationIndex = rng.randint(1, static_cast<int32_t>(count) - 3);
  }

  for (uint8_t i = 0; i < count; ++i) {
    uint16_t gate;
    uint16_t gap;
    if (i == count - 1) {
      switch (ending) {
        case ConfusedEnding::Trail:
          gate = static_cast<uint16_t>(rng.randint(55, 140));
          break;
        case ConfusedEnding::Insist:
          // Bitten off short: holding it would sound resigned, not annoyed.
          gate = static_cast<uint16_t>(rng.randint(50, 120));
          break;
        case ConfusedEnding::Falter:
          gate = static_cast<uint16_t>(rng.randint(140, 320));
          break;
        case ConfusedEnding::Rise:
        default:
          // Held so the question hangs in the air.
          gate = static_cast<uint16_t>(rng.randint(190, 340));
          break;
      }
      gap = 0;
    } else {
      const bool isHesitation = (hesitationIndex >= 0) &&
                                (static_cast<int32_t>(i) == hesitationIndex);
      if (isHesitation) {
        gate = static_cast<uint16_t>(rng.randint(90, 110));
        gap = static_cast<uint16_t>(
            rng.randint(kConfusedHesitationLoMs, kConfusedHesitationHiMs));
      } else {
        gate = static_cast<uint16_t>(
            rng.randint(kConfusedBodyGateLoMs, kConfusedBodyGateHiMs));
        gap = static_cast<uint16_t>(rng.randint(kConfusedGapLoMs, kConfusedGapHiMs));
      }
    }
    if (i < count - 1 && midi[i] == midi[i + 1] && gap < kMinRearticulationGapMs) {
      gap = kMinRearticulationGapMs;
    }
    out[i].midi = midi[i];
    out[i].gateMs = gate;
    out[i].gapMs = gap;
  }
  outCount = count;
}

// Assemble the confused phrase's pitch sequence: an anchor + body offsets +
// tail. Returns false only in the pathological "no variant valid" case, which
// the algorithm falls back on rather than raising.
bool confusedPitches(uint8_t length,
                     Pcg32& rng,
                     int8_t* midi,
                     uint8_t& count,
                     uint8_t& bodyLen,
                     ConfusedEnding& endingOut) {
  // Try (gesture, tail, body_length, anchor) combinations until one clears
  // all the constraints. Following Python's structure closely.
  struct Variant {
    int8_t body[kMaxPhraseNotes];
    uint8_t bodyLen;
    ConfusedEnding ending;
    int8_t tail[2];
    uint8_t tailLen;
  };
  // A large-ish cap. Python collects a full set; we can be more modest —
  // if we find any variant per gesture we're done.
  constexpr uint8_t kMaxVariants = 24;
  Variant variants[kMaxVariants];
  uint8_t variantCount = 0;

  // Body offsets memoised per body_length so a longer tail search reuses
  // the same body draw (matching Python's `bodies` dict).
  int8_t bodyStore[kMaxPhraseNotes][kMaxPhraseNotes];
  uint8_t bodyLens[kMaxPhraseNotes];
  bool bodyReady[kMaxPhraseNotes];
  for (uint8_t i = 0; i < kMaxPhraseNotes; ++i) bodyReady[i] = false;

  for (uint8_t g = 0; g < kConfusedEndingCount; ++g) {
    const ConfusedEnding ending = static_cast<ConfusedEnding>(g);
    ConfusedTail tails[kMaxConfusedTails];
    uint8_t tailCount = 0;
    confusedTailOffsets(ending, rng, tails, tailCount);

    for (uint8_t ti = 0; ti < tailCount; ++ti) {
      const ConfusedTail& tail = tails[ti];
      const int32_t bodyLength = static_cast<int32_t>(length) - tail.count;
      if (bodyLength < 1) continue;
      const uint8_t bl = static_cast<uint8_t>(bodyLength);

      if (!bodyReady[bl]) {
        confusedBodyOffsets(bl, rng, bodyStore[bl], bodyLens[bl]);
        bodyReady[bl] = true;
      }
      const int8_t* offs = bodyStore[bl];
      const uint8_t offsLen = bodyLens[bl];

      for (int8_t anchor = kConfusedAnchorLowMidi;
           anchor <= kConfusedAnchorHighMidi; ++anchor) {
        int8_t body[kMaxPhraseNotes];
        bool bodyOk = true;
        int8_t bodyLo = 127;
        int8_t bodyHi = -127;
        for (uint8_t i = 0; i < offsLen; ++i) {
          const int8_t note = static_cast<int8_t>(anchor + offs[i]);
          if (!bandContains(note)) { bodyOk = false; break; }
          if (!maskContains(kConfusedMask, pitchClass(note))) {
            bodyOk = false;
            break;
          }
          body[i] = note;
          if (note < bodyLo) bodyLo = note;
          if (note > bodyHi) bodyHi = note;
        }
        if (!bodyOk) continue;
        if (bodyHi - bodyLo > kConfusedMaxBodySpan) continue;

        int8_t finalNotes[2];
        bool tailOk = true;
        int8_t finalMax = -127;
        for (uint8_t i = 0; i < tail.count; ++i) {
          const int8_t note = static_cast<int8_t>(body[offsLen - 1] + tail.offsets[i]);
          if (!bandContains(note)) { tailOk = false; break; }
          finalNotes[i] = note;
          if (note > finalMax) finalMax = note;
        }
        if (!tailOk) continue;
        if (finalNotes[tail.count - 1] == body[0]) continue;
        // Whatever the gesture, the phrase must not finish on its own peak.
        // Bench 2026-08-04: an ending that topped everything already played
        // was heard as "victory" — a triumph gesture, where a question lifts
        // from a settled position.
        if (finalMax >= bodyHi) continue;

        if (variantCount >= kMaxVariants) break;
        Variant& v = variants[variantCount++];
        for (uint8_t i = 0; i < offsLen; ++i) v.body[i] = body[i];
        v.bodyLen = offsLen;
        v.ending = ending;
        for (uint8_t i = 0; i < tail.count; ++i) v.tail[i] = finalNotes[i];
        v.tailLen = tail.count;
      }
      if (variantCount >= kMaxVariants) break;
    }
    if (variantCount >= kMaxVariants) break;
  }

  if (variantCount == 0) {
    // A two-note phrase has a one-note body and no earlier peak to stay
    // under — the one shape allowed to end on its highest note. Pick
    // anchor + rise.
    int8_t body[kMaxPhraseNotes];
    uint8_t bodyLen1 = 0;
    const uint8_t bl = static_cast<uint8_t>(length - 1 >= 1 ? length - 1 : 1);
    int8_t offs[kMaxPhraseNotes];
    uint8_t offsLen = 0;
    confusedBodyOffsets(bl, rng, offs, offsLen);
    for (int8_t anchor = kConfusedAnchorLowMidi;
         anchor <= kConfusedAnchorHighMidi; ++anchor) {
      bool bodyOk = true;
      for (uint8_t i = 0; i < offsLen; ++i) {
        const int8_t n = static_cast<int8_t>(anchor + offs[i]);
        if (!bandContains(n)) { bodyOk = false; break; }
        body[i] = n;
      }
      if (!bodyOk) continue;
      for (int8_t rise = kConfusedRiseMin; rise <= kConfusedRiseMax; ++rise) {
        const int8_t final = static_cast<int8_t>(body[offsLen - 1] + rise);
        if (bandContains(final) && final != body[0]) {
          if (variantCount >= kMaxVariants) break;
          Variant& v = variants[variantCount++];
          for (uint8_t i = 0; i < offsLen; ++i) v.body[i] = body[i];
          v.bodyLen = offsLen;
          v.ending = ConfusedEnding::Rise;
          v.tail[0] = final;
          v.tailLen = 1;
          bodyLen1 = offsLen;
        }
      }
      (void)bodyLen1;
      if (variantCount >= kMaxVariants) break;
    }
  }

  if (variantCount == 0) return false;

  // Pick the GESTURE by its intended weight first, then a variant within it.
  // Choosing uniformly across all variants silently weighted by how many tail
  // shapes each gesture happens to define.
  bool available[kConfusedEndingCount] = {false, false, false, false};
  for (uint8_t i = 0; i < variantCount; ++i) {
    available[static_cast<uint8_t>(variants[i].ending)] = true;
  }
  uint8_t availableIdx[kConfusedEndingCount];
  float availWeights[kConfusedEndingCount];
  uint8_t availCount = 0;
  for (uint8_t g = 0; g < kConfusedEndingCount; ++g) {
    if (available[g]) {
      availableIdx[availCount] = g;
      // 1e-6 for a gesture that only has forced variants — matches Python.
      availWeights[availCount] = kConfusedEndingWeights[g] > 0.0f
                                     ? kConfusedEndingWeights[g]
                                     : 1e-6f;
      ++availCount;
    }
  }
  const uint8_t pickG = weightedChoice(rng, availWeights, availCount);
  const ConfusedEnding chosenEnding =
      static_cast<ConfusedEnding>(availableIdx[pickG]);

  // Pick a variant with the chosen gesture uniformly.
  uint8_t matchingIdxs[kMaxVariants];
  uint8_t matchingCount = 0;
  for (uint8_t i = 0; i < variantCount; ++i) {
    if (variants[i].ending == chosenEnding) matchingIdxs[matchingCount++] = i;
  }
  const uint8_t pickV = static_cast<uint8_t>(rng.below(matchingCount));
  const Variant& v = variants[matchingIdxs[pickV]];

  // Compose midi = body ++ tail.
  count = 0;
  for (uint8_t i = 0; i < v.bodyLen; ++i) midi[count++] = v.body[i];
  for (uint8_t i = 0; i < v.tailLen; ++i) midi[count++] = v.tail[i];
  bodyLen = v.bodyLen;
  endingOut = chosenEnding;
  return true;
}

// ---------------------------------------------------------------------------
// Phrase assembly.
// ---------------------------------------------------------------------------

// Convert RhythmNotes into Melody::Notes (tone [+ rest]).
void materialiseNotes(const RhythmNote* rn, uint8_t rnCount, Phrase& out) {
  out.noteCount = 0;
  uint32_t total = 0;
  for (uint8_t i = 0; i < rnCount; ++i) {
    const uint16_t hz = midiToHz(rn[i].midi);
    out.notes[out.noteCount++] = Melody::Note{hz, rn[i].gateMs};
    total += rn[i].gateMs;
    if (rn[i].gapMs > 0) {
      out.notes[out.noteCount++] = Melody::Note{0, rn[i].gapMs};
      total += rn[i].gapMs;
    }
  }
  out.totalMs = total;
}

uint8_t chooseTerminalFunction(Mood mood, Pcg32& rng) {
  if (mood == Mood::Happy) {
    float w[3] = {kHappyTerminals[0].weight, kHappyTerminals[1].weight,
                  kHappyTerminals[2].weight};
    const uint8_t pick = weightedChoice(rng, w, 3);
    return kHappyTerminals[pick].function;
  }
  return 0;  // sad is tonic-only
}

int8_t terminalMidiFor(Mood mood, uint8_t function) {
  int8_t offset;
  if (function == 1) {
    offset = modeThird(mood);
  } else if (function == 2) {
    offset = kFunctionOffsetFifth;
  } else {
    offset = kFunctionOffsetTonic;
  }
  // Happy tonic-terminal lifts up an octave, so a happy phrase can END high
  // instead of parking at the phrase floor.
  if (function == 0 && mood == Mood::Happy) return static_cast<int8_t>(kTonicMidi + 12);
  return static_cast<int8_t>(kTonicMidi + offset);
}

bool generateOnce(Mood mood, uint64_t seed, Phrase& out) {
  Pcg32 rng(seed);
  out.mood = mood;
  out.seed = seed;

  const uint8_t bandIdx = static_cast<uint8_t>(rng.below(3));
  out.drawnBand = kLengthBands[bandIdx];
  uint8_t length = static_cast<uint8_t>(
      rng.randint(kLengthBands[bandIdx].lo, kLengthBands[bandIdx].hi));

  if (mood == Mood::Confused) {
    // Confusion caps length by body compactness, not band headroom.
    if (length > kConfusedMaxLength) {
      length = static_cast<uint8_t>(rng.randint(5, kConfusedMaxLength));
    }
    int8_t midi[kMaxPhraseNotes];
    uint8_t midiCount = 0;
    uint8_t bodyLen = 0;
    ConfusedEnding ending = ConfusedEnding::Rise;
    if (!confusedPitches(length, rng, midi, midiCount, bodyLen, ending)) {
      return false;
    }
    RhythmNote rn[kMaxPhraseNotes];
    uint8_t rnCount = 0;
    confusedRhythm(midi, midiCount, ending, rng, rn, rnCount);
    for (uint8_t i = 0; i < midiCount; ++i) out.midi[i] = midi[i];
    out.midiCount = midiCount;
    out.bodyLength = bodyLen;
    out.confusedEnding = ending;
    out.terminalMidi = midi[midiCount - 1];
    materialiseNotes(rn, rnCount, out);
    return true;
  }

  if (mood == Mood::Sad && length > kSadMaxLength) {
    length = static_cast<uint8_t>(rng.randint(3, kSadMaxLength));
  }

  int8_t midi[kMaxPhraseNotes];
  uint8_t midiCount = 0;
  int8_t terminalMidi = 0;

  if (length == 2) {
    uint8_t terminalFn = 0;
    twoNoteGesture(mood, midi, midiCount, terminalFn, terminalMidi);
  } else {
    const uint8_t fn = chooseTerminalFunction(mood, rng);
    terminalMidi = terminalMidiFor(mood, fn);
    if (!generatePitchWalk(mood, length, terminalMidi, rng, midi, midiCount)) {
      return false;
    }
  }

  RhythmNote rn[kMaxPhraseNotes];
  uint8_t rnCount = 0;
  makeRhythm(mood, midi, midiCount, rng, rn, rnCount);
  mergeRepeatedFinal(rn, rnCount);

  // Rebuild the reported midi list to match the merged rhythm.
  for (uint8_t i = 0; i < rnCount; ++i) out.midi[i] = rn[i].midi;
  out.midiCount = rnCount;
  out.bodyLength = rnCount;
  out.terminalMidi = terminalMidi;
  out.confusedEnding = ConfusedEnding::Rise;  // unused
  materialiseNotes(rn, rnCount, out);
  return true;
}

// ---------------------------------------------------------------------------
// Signature hashing. Matches Python's `Phrase.signature`: interval sequence
// + percentage-binned rhythm, folded into a 64-bit hash.
// ---------------------------------------------------------------------------
uint64_t fnv1a(uint64_t hash, uint64_t value) {
  for (int b = 0; b < 8; ++b) {
    hash ^= static_cast<uint8_t>(value & 0xffu);
    hash *= 0x100000001b3ULL;
    value >>= 8;
  }
  return hash;
}

}  // namespace

// --------------------------------------------------------------------------
// Public API implementations.
// --------------------------------------------------------------------------

bool moodForName(const char* name, Mood& out) {
  if (name == nullptr) return false;
  if (strcmp(name, "happy") == 0) { out = Mood::Happy; return true; }
  if (strcmp(name, "sad") == 0) { out = Mood::Sad; return true; }
  if (strcmp(name, "confused") == 0) { out = Mood::Confused; return true; }
  return false;
}

const char* moodName(Mood m) {
  switch (m) {
    case Mood::Happy: return "happy";
    case Mood::Sad: return "sad";
    case Mood::Confused: return "confused";
  }
  return "";
}

uint64_t phraseSignature(const Phrase& phrase) {
  uint64_t hash = 0xcbf29ce484222325ULL;

  // Interval sequence.
  for (uint8_t i = 1; i < phrase.midiCount; ++i) {
    const int32_t interval = phrase.midi[i] - phrase.midi[i - 1];
    hash = fnv1a(hash, static_cast<uint64_t>(static_cast<int64_t>(interval)));
  }

  // Percentage-binned rhythm from the Player note stream. Reconstruct
  // (gate, gap) pairs by walking notes[] two at a time; the pattern is
  // "tone [rest]" so a rest immediately after a tone is that tone's gap.
  uint32_t total = phrase.totalMs;
  if (total == 0) return hash;

  uint16_t gates[kMaxPhraseNotes];
  uint16_t gaps[kMaxPhraseNotes];
  uint8_t pairs = 0;
  uint8_t i = 0;
  while (i < phrase.noteCount && pairs < kMaxPhraseNotes) {
    const Melody::Note& tone = phrase.notes[i++];
    if (tone.hz == 0) continue;  // shouldn't happen for a well-formed phrase
    gates[pairs] = tone.ms;
    if (i < phrase.noteCount && phrase.notes[i].hz == 0) {
      gaps[pairs] = phrase.notes[i].ms;
      ++i;
    } else {
      gaps[pairs] = 0;
    }
    ++pairs;
  }

  for (uint8_t p = 0; p < pairs; ++p) {
    // Percentage bins absorb tempo-scale quantisation noise while retaining
    // the phrase's gate/gap pattern for deduplication.
    const uint32_t gatePct =
        static_cast<uint32_t>((static_cast<uint64_t>(gates[p]) * 100 + total / 2) / total);
    const uint32_t gapPct =
        static_cast<uint32_t>((static_cast<uint64_t>(gaps[p]) * 100 + total / 2) / total);
    hash = fnv1a(hash, gatePct);
    hash = fnv1a(hash, gapPct);
  }
  return hash;
}

bool generate(Mood mood, uint64_t seed, Phrase& out) {
  return generateOnce(mood, seed, out);
}

bool generateFresh(Mood mood, uint64_t seed, Phrase& out, History& hist) {
  Phrase attempt;
  Phrase best;
  bool haveBest = false;
  for (uint32_t k = 0; k <= kHistoryRetries; ++k) {
    const uint64_t attemptSeed =
        k == 0 ? seed : splitmix64(seed ^ (0x9e3779b97f4a7c15ULL * (k + 1)));
    if (!generateOnce(mood, attemptSeed, attempt)) continue;
    const uint64_t sig = phraseSignature(attempt);
    if (!hist.seen(sig)) {
      hist.remember(sig);
      out = attempt;
      return true;
    }
    best = attempt;
    haveBest = true;
  }
  if (!haveBest) return false;
  hist.remember(phraseSignature(best));
  out = best;
  return true;
}

// History ring buffer.
History::History() : count_(0) {
  for (uint8_t i = 0; i < kCapacity; ++i) entries_[i] = 0;
}

bool History::seen(uint64_t signature) const {
  for (uint8_t i = 0; i < count_; ++i) {
    if (entries_[i] == signature) return true;
  }
  return false;
}

void History::remember(uint64_t signature) {
  // If already present, move to the end (freshest slot). Matches Python's
  // deque behaviour of removing and re-appending an already-seen key.
  for (uint8_t i = 0; i < count_; ++i) {
    if (entries_[i] == signature) {
      for (uint8_t j = i; j + 1 < count_; ++j) entries_[j] = entries_[j + 1];
      entries_[count_ - 1] = signature;
      return;
    }
  }
  if (count_ < kCapacity) {
    entries_[count_++] = signature;
  } else {
    // Evict the oldest (index 0), shift down.
    for (uint8_t j = 0; j + 1 < kCapacity; ++j) entries_[j] = entries_[j + 1];
    entries_[kCapacity - 1] = signature;
  }
}

void History::clear() { count_ = 0; }

}  // namespace MelodyGen
