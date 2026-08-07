#include "movement_expression.h"

#include <string.h>

namespace MovementExpression {
namespace {

// Counter-rotation spins in place; equal time each way returns the heading.
// Both wheels the same sign drives forward or back; equal time each way
// returns the position. Every pattern below is built from balanced pairs.

// FRUSTRATED — hard counter-rotation, swinging the body a long way each
// direction and reversing instantly, with no easing anywhere. The emotion is
// in the reversal: a smooth turn reads as looking around, an abrupt one at
// full amplitude reads as annoyed. 300 ms per swing was chosen on the bench,
// not on paper — it is roughly how long this chassis takes to sweep through
// half a turn, and that sweep is what makes it read as a body language rather
// than a vibration. Built at runtime by buildFrustrated() so the swing stays
// tunable; this table mirrors the default.
constexpr Step kFrustrated[] = {
    { 100, -100, 250},
    { 100, -100,  50},
    {-100,  100, 250},
    {-100,  100,  50},
    { 100, -100, 250},
    { 100, -100,  50},
    {-100,  100, 250},
    {-100,  100,  50},
    {   0,    0, 100},
};

// EXCITED — bookended by a full spin in each direction, with the bouncy
// middle between them. The two 360s cancel exactly, so despite being the
// biggest movement in the vocabulary it still finishes on its start heading.
// Each turn is split only to respect kMaxStepMs; it is one continuous spin,
// not four. Built at runtime by buildExcited() so the turn tracks the
// calibrated value; this table mirrors the uncalibrated default.
constexpr Step kExcited[] = {
    { 100, -100, 250},   // 360 one way (kFullTurnMs)
    { 100, -100, 250},
    { 100, -100, 100},
    {  90,  -90, 220},   // then the bouncy middle
    { -90,   90, 220},
    {  80,   80, 180},   // forward bob...
    { -80,  -80, 180},   // ...and back, so it stays put
    { -90,   90, 200},
    {  90,  -90, 200},
    {  85,   85, 150},   // a quicker bob to lift the tempo
    { -85,  -85, 150},
    {-100,  100, 250},   // 360 back the other way
    {-100,  100, 250},
    {-100,  100, 100},
    {   0,    0, 120},
};

// The fixed, self-cancelling middle of EXCITED. Kept in one place so
// buildExcited() and the table above cannot drift apart in the part that is
// not parametric. Every entry here is paired with its own inverse, which is
// what lets the turn length change without breaking net-zero.
constexpr Step kExcitedMiddle[kExcitedMiddleSteps] = {
    {  90,  -90, 220},
    { -90,   90, 220},
    {  80,   80, 180},
    { -80,  -80, 180},
    { -90,   90, 200},
    {  90,  -90, 200},
    {  85,   85, 150},
    { -85,  -85, 150},
};

constexpr uint16_t kExcitedTailMs = 120;

// Writes one continuous run of `totalMs` at (left,right), split into sub-steps
// no longer than kMaxStepMs. Returns the number of steps written, or 0 if it
// would not fit. Shared by the spin and excited builders so the chunking rule
// has exactly one implementation.
uint8_t writeRun(Step* storage, uint8_t capacity, uint8_t index, int8_t left,
                 int8_t right, uint16_t totalMs) {
  const uint16_t fullChunks = totalMs / kMaxStepMs;
  const uint16_t remainder = totalMs % kMaxStepMs;
  const uint16_t chunks = fullChunks + (remainder > 0 ? 1 : 0);
  if (chunks == 0) return 0;
  if (static_cast<uint16_t>(index) + chunks > capacity) return 0;

  for (uint16_t chunk = 0; chunk < chunks; ++chunk) {
    storage[index].left = left;
    storage[index].right = right;
    storage[index].durationMs = (chunk < fullChunks) ? kMaxStepMs : remainder;
    ++index;
  }
  return static_cast<uint8_t>(chunks);
}


// SCARED — flinch, freeze, shiver, then creep back. The freeze is the whole
// gesture: startle followed by stillness reads as fear, where continuous
// motion would read as agitation. The shivers are fast, but amplitude is
// raised as they shorten — a fast SMALL step is the one thing that reads as
// nothing at all, so speed has to be paid for with throw. The recovery is
// deliberately slow and low amplitude so it looks like the robot is edging
// back rather than darting.
constexpr Step kScared[] = {
    { -95,  -95, 160},   // hard flinch backwards
    {   0,    0, 150},   // freeze — the important part
    {   0,    0, 150},
    { -45,   45,  90},   // shiver: faster and harder than the flinch recovery
    {  45,  -45,  90},
    { -45,   45,  90},
    {  45,  -45,  90},
    { -45,   45,  90},
    {  45,  -45,  90},
    {   0,    0, 220},   // freeze again
    { -40,   40,  80},
    {  40,  -40,  80},
    { -40,   40,  80},
    {  40,  -40,  80},
    { -40,   40,  80},
    {  40,  -40,  80},
    {   0,    0, 180},
    {  40,   40, 190},   // creep back: same distance as the flinch, far
    {  40,   40, 190},   // slower, so it reads as cautious not eager
    {   0,    0, 150},
};

constexpr Pattern kPatterns[] = {
    {"frustrated", kFrustrated,
     static_cast<uint8_t>(sizeof(kFrustrated) / sizeof(kFrustrated[0]))},
    {"excited", kExcited,
     static_cast<uint8_t>(sizeof(kExcited) / sizeof(kExcited[0]))},
    {"scared", kScared,
     static_cast<uint8_t>(sizeof(kScared) / sizeof(kScared[0]))},
};

}  // namespace

bool isValidFullTurnMs(uint16_t fullTurnMs) {
  return fullTurnMs >= kSpinMinMs && fullTurnMs <= kSpinMaxMs &&
         (fullTurnMs % kStepQuantumMs) == 0;
}

bool isValidSpinDurationMs(uint16_t durationMs) {
  return durationMs >= kSpinMinMs && durationMs <= kSpinPatternMaxMs &&
         (durationMs % kStepQuantumMs) == 0;
}

bool buildSpin(uint16_t fullTurnMs, bool clockwise, int8_t speed,
               Step* storage, uint8_t capacity, Pattern& out) {
  if (storage == nullptr) return false;
  if (!isValidSpinDurationMs(fullTurnMs)) return false;
  if (speed < kSpinMinSpeed || speed > kCommandMax) return false;

  const int8_t left = clockwise ? speed : static_cast<int8_t>(-speed);
  const int8_t right = static_cast<int8_t>(-left);

  const uint8_t written = writeRun(storage, capacity, 0, left, right,
                                   fullTurnMs);
  if (written == 0) return false;

  uint8_t index = written;
  if (static_cast<uint16_t>(index) + 1 > capacity) return false;
  // Command a stop before the rail powers down, rather than letting the last
  // moving value be the final thing written.
  storage[index].left = 0;
  storage[index].right = 0;
  storage[index].durationMs = kSpinTailMs;
  ++index;

  out.name = "spin";
  out.steps = storage;
  out.stepCount = index;
  return validate(out, Balance::Unbalanced);
}

bool buildStraight(uint16_t durationMs, bool forward, int8_t speed,
                   Step* storage, uint8_t capacity, Pattern& out) {
  if (storage == nullptr) return false;
  if (durationMs < kStraightMinMs || durationMs > kStraightMaxMs) return false;
  if (durationMs % kStepQuantumMs != 0) return false;
  if (speed < kSpinMinSpeed || speed > kCommandMax) return false;

  // Both wheels the same sign. The drive is differential, so equal commands
  // mean translation; the caller trims this live against the gyro because two
  // hobby servos never match.
  const int8_t command = forward ? speed : static_cast<int8_t>(-speed);

  const uint8_t written =
      writeRun(storage, capacity, 0, command, command, durationMs);
  if (written == 0) return false;

  uint8_t index = written;
  if (static_cast<uint16_t>(index) + 1 > capacity) return false;
  storage[index].left = 0;
  storage[index].right = 0;
  storage[index].durationMs = kSpinTailMs;
  ++index;

  out.name = "straight";
  out.steps = storage;
  out.stepCount = index;
  return validate(out, Balance::Unbalanced);
}

bool buildExcited(uint16_t fullTurnMs, Step* storage, uint8_t capacity,
                  Pattern& out) {
  if (storage == nullptr) return false;
  if (!isValidFullTurnMs(fullTurnMs)) return false;

  uint8_t index = 0;

  // Opening turn.
  uint8_t written = writeRun(storage, capacity, index, kCommandMax,
                             kCommandMin, fullTurnMs);
  if (written == 0) return false;
  index = static_cast<uint8_t>(index + written);

  // Fixed middle.
  if (static_cast<uint16_t>(index) + kExcitedMiddleSteps > capacity) {
    return false;
  }
  for (uint8_t i = 0; i < kExcitedMiddleSteps; ++i) {
    storage[index] = kExcitedMiddle[i];
    ++index;
  }

  // Closing turn, opposite to the opening one so the heading nets out for any
  // calibrated value.
  written = writeRun(storage, capacity, index, kCommandMin, kCommandMax,
                     fullTurnMs);
  if (written == 0) return false;
  index = static_cast<uint8_t>(index + written);

  if (static_cast<uint16_t>(index) + 1 > capacity) return false;
  storage[index].left = 0;
  storage[index].right = 0;
  storage[index].durationMs = kExcitedTailMs;
  ++index;

  out.name = "excited";
  out.steps = storage;
  out.stepCount = index;
  return validate(out);
}

bool buildFrustrated(uint16_t runMs, Step* storage, uint8_t capacity,
                     Pattern& out) {
  if (storage == nullptr) return false;
  if (runMs < kFrustratedMinRunMs || runMs > kFrustratedMaxRunMs) return false;
  if (runMs % kStepQuantumMs != 0) return false;

  // Split a long run into sub-steps so no single step exceeds the cap. The
  // tick refreshes the hardware lease independently, but keeping the cap means
  // the pattern data on its own can never starve it. Both directions use the
  // identical decomposition, so the heading still nets out exactly.
  const uint16_t fullChunks = runMs / kMaxStepMs;
  const uint16_t remainder = runMs % kMaxStepMs;
  const uint16_t chunks = fullChunks + (remainder > 0 ? 1 : 0);

  const uint16_t needed = static_cast<uint16_t>(kFrustratedRuns) * chunks + 1;
  if (needed > capacity) return false;

  uint8_t index = 0;
  for (uint8_t run = 0; run < kFrustratedRuns; ++run) {
    const int8_t magnitude = (run % 2 == 0) ? kCommandMax : kCommandMin;
    for (uint16_t chunk = 0; chunk < chunks; ++chunk) {
      storage[index].left = magnitude;
      storage[index].right = static_cast<int8_t>(-magnitude);
      storage[index].durationMs =
          (chunk < fullChunks) ? kMaxStepMs : remainder;
      ++index;
    }
  }
  storage[index].left = 0;
  storage[index].right = 0;
  storage[index].durationMs = kFrustratedTailMs;
  ++index;

  out.name = "frustrated";
  out.steps = storage;
  out.stepCount = index;
  return validate(out);
}

const Pattern* patternFor(Mood mood) {
  const size_t index = static_cast<size_t>(mood);
  if (index >= sizeof(kPatterns) / sizeof(kPatterns[0])) {
    return nullptr;
  }
  return &kPatterns[index];
}

const Pattern* patternForName(const char* name) {
  if (name == nullptr) return nullptr;
  for (const Pattern& pattern : kPatterns) {
    if (strcmp(pattern.name, name) == 0) {
      return &pattern;
    }
  }
  return nullptr;
}

uint8_t patternCount() {
  return static_cast<uint8_t>(sizeof(kPatterns) / sizeof(kPatterns[0]));
}

const char* patternNameAt(uint8_t index) {
  if (index >= patternCount()) return nullptr;
  return kPatterns[index].name;
}

uint32_t totalDurationMs(const Pattern& pattern) {
  uint32_t total = 0;
  for (uint8_t i = 0; i < pattern.stepCount; ++i) {
    total += pattern.steps[i].durationMs;
  }
  return total;
}

void netDisplacement(const Pattern& pattern, long& left, long& right) {
  left = 0;
  right = 0;
  for (uint8_t i = 0; i < pattern.stepCount; ++i) {
    const Step& step = pattern.steps[i];
    left += static_cast<long>(step.left) * step.durationMs;
    right += static_cast<long>(step.right) * step.durationMs;
  }
}

bool validate(const Pattern& pattern, Balance balance) {
  if (pattern.steps == nullptr || pattern.stepCount == 0) {
    return false;
  }
  for (uint8_t i = 0; i < pattern.stepCount; ++i) {
    const Step& step = pattern.steps[i];
    if (step.left < kCommandMin || step.left > kCommandMax ||
        step.right < kCommandMin || step.right > kCommandMax) {
      return false;
    }
    if (step.durationMs == 0 || step.durationMs > kMaxStepMs) {
      return false;
    }
    // Off-grid durations reintroduce sampling drift, which quietly breaks the
    // net-zero guarantee once the pattern is actually played.
    if (step.durationMs % kStepQuantumMs != 0) {
      return false;
    }
  }
  if (totalDurationMs(pattern) > kMaxPatternMs) {
    return false;
  }
  if (balance == Balance::Unbalanced) {
    return true;
  }
  long left = 0;
  long right = 0;
  netDisplacement(pattern, left, right);
  return left == 0 && right == 0;
}

Sequencer::Sequencer()
    : pattern_(nullptr), startedMs_(0), active_(false) {}

bool Sequencer::start(const Pattern* pattern, uint32_t nowMs,
                      Balance balance) {
  if (pattern == nullptr || !validate(*pattern, balance)) {
    stop();
    return false;
  }
  pattern_ = pattern;
  startedMs_ = nowMs;
  active_ = true;
  return true;
}

void Sequencer::stop() {
  pattern_ = nullptr;
  startedMs_ = 0;
  active_ = false;
}

bool Sequencer::active() const {
  return active_;
}

const Pattern* Sequencer::pattern() const {
  return pattern_;
}

uint32_t Sequencer::elapsedMs(uint32_t nowMs) const {
  if (!active_) return 0;
  return nowMs - startedMs_;
}

bool Sequencer::sample(uint32_t nowMs, int& left, int& right) {
  left = 0;
  right = 0;
  if (!active_ || pattern_ == nullptr) {
    return false;
  }

  const uint32_t elapsed = nowMs - startedMs_;
  uint32_t boundary = 0;
  for (uint8_t i = 0; i < pattern_->stepCount; ++i) {
    boundary += pattern_->steps[i].durationMs;
    if (elapsed < boundary) {
      left = pattern_->steps[i].left;
      right = pattern_->steps[i].right;
      return true;
    }
  }

  // Past the end. Leave the caller holding neutral rather than the last
  // commanded value, so a missed stop cannot leave the wheels turning.
  stop();
  return false;
}

}  // namespace MovementExpression
