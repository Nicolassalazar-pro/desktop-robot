#pragma once

// ============================================================================
// movement_expression.h — emotional movement vocabulary for the drive motors.
//
// Movements are expressed as a sequence of (left, right, duration) steps in
// the same -100..100 command space the WebSocket joystick uses, so they feed
// the existing canonical drive output and inherit every safety check already
// in place: emergency latches, pause, calibration eligibility, range clamping,
// the 300 ms hardware lease and the 1 s neutral idle power-down.
//
// DESIGN RULE — NET ZERO.
// Every pattern must integrate to zero on both wheels, so the robot finishes
// where it started. Two reasons, and the second matters more:
//   1. A desk robot that travels can reach an edge.
//   2. Emotion lives in the QUALITY of motion, not in displacement. A shiver,
//      a double-take and an excited wiggle all read clearly without the robot
//      going anywhere. Travelling would actually dilute them.
// `netDisplacement()` exists so this is checked rather than assumed.
//
// Kept free of Arduino headers so the patterns can be unit-tested on the host.
// ============================================================================

#include <stdint.h>

namespace MovementExpression {

// Matches DriveSession::kCommandMin/Max.
constexpr int8_t kCommandMin = -100;
constexpr int8_t kCommandMax = 100;

// The canonical output must be refreshed inside DriveSession::kControlTimeoutMs
// (300 ms) or the independent safety timer cuts the motor rail. The sequencer
// is ticked far faster than that; this is the ceiling a single step may hold
// without an intermediate refresh.
constexpr uint16_t kMaxStepMs = 250;

// Step durations are multiples of this so that sampling the pattern on the
// firmware's tick introduces no drift. Without it, a step of e.g. 85 ms
// sampled every 10 ms accumulates a rounding residue and the "returns to
// start" guarantee stops holding in practice even though it holds on paper.
constexpr uint16_t kStepQuantumMs = 10;

// A whole gesture is bounded so a stuck sequencer cannot drive indefinitely.
// Sized so `excited` still fits at the longest calibratable full turn
// (2 x kSpinMaxMs + 1620 ms of middle and tail = 5620 ms), and so a single
// slow measurement/turn spin still fits (kSpinPatternMaxMs + tail). The real
// protection against a runaway is the 300 ms hardware lease, the emergency
// latch and the tick; this cap is the coarse backstop behind them.
constexpr uint32_t kMaxPatternMs = 8000;

struct Step {
  int8_t left;
  int8_t right;
  uint16_t durationMs;
};

struct Pattern {
  const char* name;
  const Step* steps;
  uint8_t stepCount;
};

// A gesture must integrate to zero (see the design rule above). A calibration
// spin deliberately must not: it turns one way to be measured. It is still
// zero TRANSLATION — counter-rotation pivots in place — so only the heading
// changes, which is the entire point of measuring it. Defaulting to NetZero
// keeps the guarantee for everything that is not explicitly a measurement.
enum class Balance : uint8_t {
  NetZero,
  Unbalanced,
};

enum class Mood : uint8_t {
  Frustrated,
  Excited,
  Scared,
};

// ---------------------------------------------------------------------------
// FRUSTRATED is parametric.
//
// The right run length is a property of the chassis — wheel diameter, track
// width, surface grip — not something derivable on paper. It is however long
// the body takes to swing through roughly half a turn. Exposing it means it
// can be dialled on the bench in seconds instead of through a reflash cycle.
// ---------------------------------------------------------------------------
constexpr uint16_t kFrustratedDefaultRunMs = 300;
constexpr uint16_t kFrustratedMinRunMs = 60;
constexpr uint16_t kFrustratedMaxRunMs = 900;
constexpr uint8_t kFrustratedRuns = 4;  // even, so the heading nets out
constexpr uint16_t kFrustratedTailMs = 100;

// Bench-measured on this chassis: 300 ms of full-speed counter-rotation sweeps
// the body through roughly half a turn.
constexpr uint16_t kHalfTurnMs = kFrustratedDefaultRunMs;

// ---------------------------------------------------------------------------
// FULL TURN — calibrated, not derived.
//
// This was previously written as kHalfTurnMs * 2, which is wrong twice over.
// It was never measured, and a full turn is not two half turns: both pay the
// same standing-start acceleration, so doubling the half-turn systematically
// UNDER-rotates. The 600 ms below is only the fallback used until the value is
// measured on the bench with the `spin` command and saved.
//
// Calibrate a single continuous turn from a standstill. That is exactly what
// the OPENING turn does — the rail is off and the wheels start at rest. The
// closing turn is entered from the last bob ({-85,-85}), so the right wheel
// reverses fully while the left barely changes; it accelerates differently and
// will rotate slightly less for the same duration. Net-zero is unaffected,
// because that is a property of matched DURATIONS, not of actual rotation.
//
// Do not derive the value from a multi-turn run either: consecutive turns
// amortise one acceleration between them, so ms-per-turn measured that way is
// precise about a different quantity again.
// ---------------------------------------------------------------------------
constexpr uint16_t kSpinDefaultMs = 600;
constexpr uint16_t kFullTurnMs = kSpinDefaultMs;

constexpr uint16_t kSpinMinMs = 200;
constexpr uint16_t kSpinMaxMs = 2000;
constexpr int8_t kSpinSpeed = kCommandMax;
// Below this a continuous-rotation servo is in its deadband and the chassis
// may not move at all, which would read as "infinitely slow" rather than as
// the invalid probe it is.
constexpr int8_t kSpinMinSpeed = 30;
constexpr uint16_t kSpinTailMs = 100;

// A single spin PATTERN may run longer than the calibratable full-turn range.
// The two are different quantities: kSpinMaxMs bounds "how long a full turn at
// full speed may be", while this bounds "how long one spin pattern may last".
// A deliberately slow turn sweeps the same 360 degrees over much more time, so
// it needs the larger bound without widening what counts as a valid
// calibration value.
//
// Raised from 6000 to accommodate the multi-revolution scale check. Scale is
// the one gyro error invisible from onboard data, and measuring it needs
// several revolutions so the error accumulates well past the precision of
// reading a mark on the floor. Three revolutions at the measured 1260 ms/turn
// is ~3.8 s of rotation, which with the standing-start allowance and the 1.6x
// timeout headroom needs ~6.7 s.
//
// Kept below kMaxPatternMs (8000) minus the tail, so a duration that passes
// this check cannot then fail pattern validation — two caps that disagree
// would reject a request only at the moment of firing, which for an
// untethered test means a wasted trip.
//
// This is NOT a safety bound. The 300 ms drive lease, the independent hardware
// cutoff and the TurnController timeout all still apply, unchanged.
constexpr uint16_t kSpinPatternMaxMs = 7500;

// Chunk counts differ for the same reason: excited's bookend turns come from
// the full-turn range, a measurement or slow turn from the pattern range.
constexpr uint8_t kFullTurnMaxChunks =
    static_cast<uint8_t>((kSpinMaxMs + kMaxStepMs - 1) / kMaxStepMs);
constexpr uint8_t kSpinMaxChunks =
    static_cast<uint8_t>((kSpinPatternMaxMs + kMaxStepMs - 1) / kMaxStepMs);
constexpr uint8_t kSpinStepCap = static_cast<uint8_t>(kSpinMaxChunks + 1);

// True if the value is a usable calibrated full turn: in range and on the
// tick grid, so it cannot reintroduce sampling drift.
bool isValidFullTurnMs(uint16_t fullTurnMs);

// True if the value is a usable single spin pattern duration. Wider than a
// full-turn value, because a slow turn takes longer to sweep the same angle.
bool isValidSpinDurationMs(uint16_t durationMs);

// Writes a single-direction measurement spin into caller storage: `fullTurnMs`
// of counter-rotation at `speed`, then a neutral tail so the wheels are
// commanded to stop before the rail is powered down. NOT net-zero by design —
// validate it with Balance::Unbalanced.
//
// Speed is a parameter because the chassis is characterised by measuring yaw
// rate at more than one wheel command and fitting a line through them.
bool buildSpin(uint16_t fullTurnMs, bool clockwise, int8_t speed,
               Step* storage, uint8_t capacity, Pattern& out);

// Drives both wheels the same way for `durationMs`, so the robot translates
// instead of pivoting.
//
// Deliberately Unbalanced: unlike every other pattern here, this one does NOT
// return the robot to where it started — that is the entire point. It exists so
// the robot can survey by moving between positions, which is the only motion
// that produces any RSSI information at all (rotating in place changes range to
// nothing).
//
// Heading is NOT held by the pattern. Two continuous-rotation servos never match
// exactly, so an open-loop "both wheels forward" curves. The caller is expected
// to close the loop on the gyro and trim the differential live.
bool buildStraight(uint16_t durationMs, bool forward, int8_t speed,
                   Step* storage, uint8_t capacity, Pattern& out);

// A straight run may be several seconds, which is far longer than a turn.
constexpr uint16_t kStraightMinMs = 200;
constexpr uint16_t kStraightMaxMs = 8000;
constexpr uint8_t kStraightStepCap =
    static_cast<uint8_t>((kStraightMaxMs + kMaxStepMs - 1) / kMaxStepMs + 1);

// Worst case: every run split into ceil(max/kMaxStepMs) sub-steps, plus tail.
constexpr uint8_t kFrustratedStepCap =
    static_cast<uint8_t>(kFrustratedRuns *
                             ((kFrustratedMaxRunMs + kMaxStepMs - 1) / kMaxStepMs) +
                         1);

// EXCITED is parametric in the one quantity that has to be measured: the full
// turn that bookends it. The bouncy middle is fixed and self-cancelling.
constexpr uint8_t kExcitedMiddleSteps = 8;
constexpr uint8_t kExcitedStepCap = static_cast<uint8_t>(
    (2 * kFullTurnMaxChunks) + kExcitedMiddleSteps + 1);

// Writes an excited pattern whose opening and closing turns each last
// `fullTurnMs`. The two turns are opposite, so the heading still nets out
// whatever the calibrated value is. Returns false on a bad duration or too
// little storage, leaving `out` untouched.
bool buildExcited(uint16_t fullTurnMs, Step* storage, uint8_t capacity,
                  Pattern& out);

// Writes a frustrated pattern of the requested per-run duration into caller
// storage. Runs longer than kMaxStepMs are split into equal sub-steps so the
// per-step cap still holds. Returns false on a bad duration or too little
// storage, leaving `out` untouched.
bool buildFrustrated(uint16_t runMs, Step* storage, uint8_t capacity,
                     Pattern& out);

// Returns nullptr for an unknown name.
const Pattern* patternFor(Mood mood);
const Pattern* patternForName(const char* name);

// Enumerates the movement vocabulary. The web app is the only surface that
// exposes all of it, so it indexes THIS table rather than the LCD menu's
// button list — that list is a 3-item fallback plus a conditional test slot,
// and indexing it from the network would both cap the vocabulary at 3 and put
// a non-movement ("run test") inside the movement index space.
uint8_t patternCount();
const char* patternNameAt(uint8_t index);

uint32_t totalDurationMs(const Pattern& pattern);

// Signed sum of command x duration per wheel. Zero means the wheel ends where
// it began, which is what keeps the robot in place.
void netDisplacement(const Pattern& pattern, long& left, long& right);

// Every step within command range, no step longer than the lease allows, the
// pattern within the global duration cap, and — unless explicitly measuring —
// net displacement zero.
bool validate(const Pattern& pattern, Balance balance = Balance::NetZero);

// Walks a pattern against a millisecond clock.
class Sequencer {
 public:
  Sequencer();

  // Ignores an invalid pattern rather than driving from bad data. Balance
  // defaults to NetZero so a gesture can never opt out of returning to start
  // by accident; only a deliberate measurement passes Unbalanced.
  bool start(const Pattern* pattern, uint32_t nowMs,
             Balance balance = Balance::NetZero);
  void stop();

  bool active() const;
  const Pattern* pattern() const;

  // Current command for this instant. Returns false once the pattern is done,
  // with left/right set to zero so the caller always has a safe value.
  bool sample(uint32_t nowMs, int& left, int& right);

  uint32_t elapsedMs(uint32_t nowMs) const;

 private:
  const Pattern* pattern_;
  uint32_t startedMs_;
  bool active_;
};

}  // namespace MovementExpression
