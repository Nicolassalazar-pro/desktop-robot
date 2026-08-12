// Host tests for the sheet-transition physics that live in
// src/sheet_transition.{h,cpp}.
//
// The header (src/sheet_transition.h) is the fixed contract. These tests
// exercise the REAL shipped translation unit -- src/sheet_transition.cpp is
// linked into this binary directly. That is the whole point of pulling the
// sheet physics out of main.cpp: unlike test/ws_owner_reaper_test.cpp, which
// has to model the reaper policy in its own file because the real policy is
// buried in main.cpp, this test pins the real code.
//
// SHIPPED DEFECTS THAT WOULD HAVE FAILED HERE
//
// PROJECT-HISTORY R46 was an unsigned-underflow bug in a millis()-based
// silence check: `uint32_t dt = now - lastMs` read as 4,294,967,295 across
// the cross-task race and cut drive power. The header calls this out
// explicitly ("Timestamps must be handled rollover-safely: subtract into a
// SIGNED type") because the same shape appears in Controller::onMove --
// velocity is `(y - lastSampleY) * 1000 / dt`. testMillisRolloverMidDrag
// below is the guard against a repeat here.
//
// A position-only release policy (ignore velocity, decide from y alone)
// visually reads as a dead sheet: a deliberate flick from well below
// threshold does not open, and a finger that pulled back past threshold
// still opens despite the reversal. The header calls that out ("Release
// decides on VELOCITY, not position"). The two headline tests below --
// testFastFlickBelowThresholdCommits and
// testReversingReleaseAboveThresholdCloses -- each INDEPENDENTLY prove
// projection is doing something a position-only comparator cannot. Half 1
// flips a below-threshold position to open on downward velocity. Half 2
// flips an above-threshold position to close on upward velocity. They are
// deliberately kept as separate top-level tests so a maintainer who
// simplifies onRelease() to `target = y >= threshold ? h : 0;` sees BOTH
// go red, not just the first that aborted.
//
// A stationary release at any position above the threshold MUST commit --
// that is the correct behaviour, and any test claiming otherwise would
// fail against correct code and send a maintainer chasing a phantom bug.
//
// takeSettledOpen() is a one-shot. Get its edge wrong and the caller either
// commits repeatedly on every frame (paired-account created N times) or
// never (silently ignored release). testSettledOpenIsOneShot pins both
// directions.

#include "../src/sheet_transition.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG
#error "Host safety tests require assertions enabled"
#endif

using SheetTransition::Controller;
using SheetTransition::State;
using SheetTransition::chooseTarget;
using SheetTransition::kMinSampleMs;
using SheetTransition::kProjectMs;
using SheetTransition::kRubberDivisor;
using SheetTransition::kSettlePercent;
using SheetTransition::kThresholdPct;
using SheetTransition::project;
using SheetTransition::rubberBand;
using SheetTransition::settleStep;

namespace {

// Standard travel height for the drag tests. Big enough that a threshold of
// 40% (80 px) is well clear of the initial rubber-band range, small enough
// that a realistic finger movement of a few px/ms crosses meaningful
// projected distances inside kProjectMs.
constexpr int16_t kH = 200;
constexpr int16_t kThreshold =
    static_cast<int16_t>((kH * kThresholdPct) / 100);  // 80 for kH=200

// Drive the controller through settling until it stops moving or we run out
// of ticks. The header promises settleStep() moves at least 1 px per call
// even when the percentage rounds to 0, so this MUST terminate within a
// bounded budget -- a percentage-of-remaining that ever crawls forever is
// itself the bug.
int settleUntilDone(Controller& c) {
  for (int i = 0; i < 2048; ++i) {
    if (!c.tick(kSettlePercent)) return i;
  }
  fprintf(stderr, "settle did not terminate in 2048 ticks\n");
  abort();
}

// -----------------------------------------------------------------------------
// rubberBand
// -----------------------------------------------------------------------------

void testRubberBandInsideRangeIsExactlyOneToOne() {
  // The header calls this out: "0 <= raw <= h -> returned unchanged (true 1:1
  // tracking)". No damping, no rounding, no smoothing inside the visible
  // range. If this ever drifts to `raw * something / something` you can feel
  // it on a phone -- the card lags the finger by a couple of pixels.
  for (int16_t raw = 0; raw <= kH; ++raw) {
    assert(rubberBand(raw, kH) == raw);
  }
}

void testRubberBandOvershootBelowZeroIsDamped() {
  // raw < 0 means the finger is above the top stop. Header contract:
  // "damped by kRubberDivisor". The operation is division-by-divisor;
  // that damping is what pulls the "roughly [-h/3, ...]" bound the
  // header describes for realistic screen-sized overshoots.
  const int16_t r = rubberBand(-30, kH);
  // Damped: magnitude much smaller than the raw overshoot.
  assert(r < 0);
  assert(r > -30);
  // Realistic overshoot stays inside the header's rough [-h/3, ...] bound.
  assert(r >= -static_cast<int16_t>(kH / kRubberDivisor) - 2);

  // A larger overshoot must have its magnitude damped, not linearly tracked.
  // A well-behaved damper crushes the input by roughly kRubberDivisor, so a
  // -90 raw ends up nearer -30 than -90.
  const int16_t rBig = rubberBand(-90, kH);
  assert(rBig < 0);
  assert(-rBig <= 90 / static_cast<int16_t>(kRubberDivisor) + 4);
  // And not linearly tracked -- a broken damper that returns raw unchanged
  // would fail here.
  assert(-rBig < 90);
}

void testRubberBandOvershootPastHIsDamped() {
  const int16_t r = rubberBand(kH + 30, kH);
  assert(r > kH);
  assert(r < kH + 30);
  assert(r <= kH + static_cast<int16_t>(kH / kRubberDivisor) + 2);

  const int16_t rBig = rubberBand(kH + 90, kH);
  assert(rBig > kH);
  assert(rBig - kH <= 90 / static_cast<int16_t>(kRubberDivisor) + 4);
  assert(rBig - kH < 90);
}

void testRubberBandBoundariesAreExactlyOneToOne() {
  // Both endpoints are inside the "0 <= raw <= h" range and must come back
  // untouched. A greedy `>` check on the top boundary would ruin the feel
  // right at fully-open.
  assert(rubberBand(0, kH) == 0);
  assert(rubberBand(kH, kH) == kH);
}

void testRubberBandZeroHeightDoesNotDivideByZero() {
  // The header calls this out: "h == 0 does not divide by zero or return
  // nonsense". A degenerate viewport (no travel) must still return a small,
  // bounded value rather than trap on integer divide.
  const int16_t rZero = rubberBand(0, 0);
  assert(rZero == 0);
  // Non-zero raw with h==0 is 100% overshoot. Result must be finite and
  // small. Loose bound: the damper cannot amplify.
  const int16_t rPos = rubberBand(5, 0);
  assert(rPos >= 0 && rPos <= 5);
  const int16_t rNeg = rubberBand(-5, 0);
  assert(rNeg <= 0 && rNeg >= -5);
}

// -----------------------------------------------------------------------------
// project
// -----------------------------------------------------------------------------

void testProjectZeroVelocityReturnsY() {
  // With no velocity there is nothing to project -- the sheet stops where
  // the finger left it.
  assert(project(50, 0, kProjectMs) == 50);
  assert(project(0, 0, kProjectMs) == 0);
  assert(project(kH, 0, kProjectMs) == kH);
}

void testProjectVelocityDirectionMatchesSign() {
  // Positive velocity (finger moving down / opening) projects further down.
  const int32_t forward = project(100, 2000, kProjectMs);
  assert(forward > 100);
  // Negative velocity (finger moving up / closing) projects up.
  const int32_t backward = project(100, -2000, kProjectMs);
  assert(backward < 100);
}

void testProjectMagnitudeMatchesFormula() {
  // Header formula: projected = y + velocity * decay / 1000. A realistic
  // flick of 2 px/ms over 120 ms displaces 240 px. Assert the impl is doing
  // that arithmetic and not some smoothed/scaled variant.
  const int32_t p = project(0, 2000, 120);
  assert(p == 240);
}

void testProjectHandlesLargeVelocityWithoutInt16Overflow() {
  // The header warns about int16 overflow. Velocity is int32_t and the
  // return type is int32_t -- an impl that truncates through int16_t
  // anywhere on the path (say, an accidental static_cast<int16_t>()) wraps
  // and yields nonsense.
  //
  // Realistic worst case: a very stiff finger flick captured over a long
  // projection horizon. 5 px/ms * 500 ms = 2500 px displacement -- well
  // outside int16 range if any intermediate is int16.
  const int32_t p = project(200, 5000, 500);
  assert(p == 200 + 2500);
  // And a big negative flick, symmetric.
  const int32_t pNeg = project(200, -5000, 500);
  assert(pNeg == 200 - 2500);
}

// -----------------------------------------------------------------------------
// chooseTarget
// -----------------------------------------------------------------------------

void testChooseTargetBoundaryIsAtOrAboveThreshold() {
  // Header: "below threshold -> 0, at/above -> h". Test the boundary
  // exactly. A ">" instead of ">=" (or vice versa) is a one-character bug
  // and this is where we catch it.
  assert(chooseTarget(kThreshold - 1, kH, kThreshold) == 0);
  assert(chooseTarget(kThreshold,     kH, kThreshold) == kH);
  assert(chooseTarget(kThreshold + 1, kH, kThreshold) == kH);
}

void testChooseTargetFarAwayEndpoints() {
  // Well past either end, obvious answers -- catches a swapped-branch bug.
  assert(chooseTarget(-1000, kH, kThreshold) == 0);
  assert(chooseTarget(0, kH, kThreshold) == 0);
  assert(chooseTarget(kH * 5, kH, kThreshold) == kH);
}

// -----------------------------------------------------------------------------
// settleStep
// -----------------------------------------------------------------------------

void testSettleStepConvergesUpwardWithoutOvershoot() {
  int16_t y = 0;
  const int16_t target = 200;
  // Bounded iteration budget -- the "at least 1 px per tick" clause in the
  // header MUST make this terminate. If a maintainer removes the
  // min-progress guarantee, the loop crawls asymptotically and this
  // deadlines out.
  int steps = 0;
  while (y != target) {
    const int16_t next = settleStep(y, target, kSettlePercent);
    // Never overshoots. A "moved 25% + at least 1" implementation that
    // forgets to cap at target lands on 201, 202, ... and the sheet
    // visibly bumps.
    assert(next <= target);
    // Monotone forward progress (>= 1 px).
    assert(next > y);
    y = next;
    if (++steps > 512) {
      fprintf(stderr, "settleStep did not converge upward in 512 steps\n");
      abort();
    }
  }
}

void testSettleStepConvergesDownwardWithoutOvershoot() {
  int16_t y = 200;
  const int16_t target = 0;
  int steps = 0;
  while (y != target) {
    const int16_t next = settleStep(y, target, kSettlePercent);
    assert(next >= target);
    assert(next < y);
    y = next;
    if (++steps > 512) {
      fprintf(stderr, "settleStep did not converge downward in 512 steps\n");
      abort();
    }
  }
}

void testSettleStepAlreadyAtTargetStaysThere() {
  // A settle that has arrived does not wander. Otherwise a caller who
  // ticks past-completion will see visible jitter.
  assert(settleStep(0, 0, kSettlePercent) == 0);
  assert(settleStep(200, 200, kSettlePercent) == 200);
  assert(settleStep(137, 137, kSettlePercent) == 137);
}

void testSettleStepGuaranteesOnePixelProgress() {
  // The header's teeth-baring claim: "Guarantees forward progress of at
  // least 1 px so it always terminates rather than asymptotically
  // crawling". At y = target - 1 with a 1% step, 1% of 1 rounds to 0. If
  // the impl returns y (no progress), the animation is frozen and the
  // pairing card never quite settles. This is the entire reason the
  // header spelled out the "at least 1 px" clause.
  assert(settleStep(199, 200, 1) == 200);
  assert(settleStep(1, 0, 1) == 0);
}

// -----------------------------------------------------------------------------
// Controller
// -----------------------------------------------------------------------------

void testOnPressOnPartlyOpenSheetDoesNotJumpToFinger() {
  // Regression: a naive `onPress(sy, ms) { y = sy; }` would snap the card
  // to the touch point, which reads as a violent yank on the very first
  // touch of a partly-open sheet. The dragBaseY / dragOriginY split exists
  // precisely so the finger can land anywhere without the card moving --
  // motion starts on the first onMove(), not on onPress().
  Controller c;
  c.y = 137;                          // partially open somehow
  c.state = State::Open;              // presumed committed-open resting
  c.velocity = 0;
  c.lastSampleY = 137;

  c.onPress(50, 1000);

  assert(c.y == 137);                 // did NOT jump to the finger
  assert(c.dragBaseY == 137);         // captured current sheet travel
  assert(c.dragOriginY == 50);        // captured finger origin
  assert(c.state == State::Dragging);
}

void testNothingCommitsMidDrag() {
  // "State changes ONLY on settle. Nothing commits mid-drag." A
  // takeSettledOpen() that fires during Dragging would double-commit
  // pairing on every intermediate frame.
  Controller c;
  c.onPress(100, 0);
  assert(c.state == State::Dragging);
  assert(!c.takeSettledOpen());

  // Drag deep into open (past full travel, into rubber-band territory).
  for (uint32_t t = 10; t <= 100; t += 10) {
    const int16_t sy = static_cast<int16_t>(100 + t * 3);  // fast pull
    c.onMove(sy, t, kH);
    assert(c.state == State::Dragging);
    assert(!c.takeSettledOpen());
  }
}

void testReleaseBelowThresholdSettlesBackToClosed() {
  // A shy drag that stops short of the threshold and holds still must
  // roll back to Closed and never report open.
  Controller c;
  c.onPress(100, 0);
  // Move to 30% of h (below the 40% threshold) with essentially zero
  // final velocity -- steady, decelerating drag.
  c.onMove(120, 10, kH);   // y=20
  c.onMove(140, 40, kH);   // y=40 (slowing dt)
  c.onMove(150, 90, kH);   // y=50 (very slow)
  c.onMove(160, 200, kH);  // y=60, ~30% of kH, low velocity
  c.onRelease(kH, kThreshold);
  assert(c.state == State::Settling);

  const int settleTicks = settleUntilDone(c);
  (void)settleTicks;
  assert(c.state == State::Closed);
  assert(c.y == 0);
  assert(!c.takeSettledOpen());
}

// THE headline case, half 1. See the file-top block comment.
//
// Below threshold + fast downward flick -> COMMITS. A release with y well
// below the 40% threshold but with a strong downward velocity projects
// past the threshold and must open. Position alone would have closed it,
// so this test independently proves projection is doing work no y-only
// comparator could do.
void testFastFlickBelowThresholdCommits() {
  Controller c;
  c.onPress(100, 0);
  // Three consecutive samples of steady +1.5 px/ms downward drag land at
  // y = 45 (22.5% of kH, well below the 40% threshold). Consistent
  // samples mean any reasonable velocity accumulator (last-delta, EMA,
  // running mean) reports a strong positive velocity.
  c.onMove(115, 10, kH);   // y = 15, dt = 10ms
  c.onMove(130, 20, kH);   // y = 30, dt = 10ms
  c.onMove(145, 30, kH);   // y = 45, dt = 10ms
  assert(c.y == 45);
  assert(c.y < kThreshold);      // position alone would close this
  assert(c.velocity > 0);        // finger is clearly moving down

  c.onRelease(kH, kThreshold);
  assert(c.state == State::Settling);
  // Header formula: projected = y + v*decay/1000. With v ~ 1500 and
  // decay 120 the projected endpoint is around 225, comfortably past the
  // 80-px threshold. Target must be kH.
  assert(c.target == kH);

  settleUntilDone(c);
  assert(c.state == State::Open);
  assert(c.y == kH);
  assert(c.takeSettledOpen());
}

// THE headline case, half 2. See the file-top block comment.
//
// Above threshold + reversing pull-back -> CLOSES. A release with y above
// the 40% threshold but with a clear upward velocity (the finger reversed
// direction just before liftoff) projects BELOW the threshold, so the
// sheet closes despite y at release being "committed" by position alone.
// This test independently proves velocity projection can flip the verdict
// AWAY from what position would say. A stationary release at the SAME y
// with zero velocity would (correctly) commit -- this test does not
// assert otherwise.
void testReversingReleaseAboveThresholdCloses() {
  Controller c;
  c.onPress(100, 0);
  // Aggressive down to y ~ 150, then sustained slow retreat back to
  // y = 120 (60% of kH). Six retreat samples of -0.5 px/ms means even a
  // heavy EMA drifts negative by the last sample. Any reasonable
  // velocity model returns a clearly negative velocity at release, and
  // the projected endpoint drops below the 80-px threshold despite the
  // resting position being 120 (60% travel).
  c.onMove(150, 10, kH);   // y = 50,  v = +5000
  c.onMove(200, 20, kH);   // y = 100, v = +5000
  c.onMove(250, 30, kH);   // y = 150, v = +5000
  c.onMove(245, 40, kH);   // y = 145, v = -500
  c.onMove(240, 50, kH);   // y = 140, v = -500
  c.onMove(235, 60, kH);   // y = 135, v = -500
  c.onMove(230, 70, kH);   // y = 130, v = -500
  c.onMove(225, 80, kH);   // y = 125, v = -500
  c.onMove(220, 90, kH);   // y = 120, v = -500
  assert(c.y == 120);
  assert(c.y > kThreshold);      // position alone would commit this
  assert(c.velocity < 0);        // the finger is retreating

  c.onRelease(kH, kThreshold);
  assert(c.state == State::Settling);
  // Projected = 120 + v*120/1000. With last-sample velocity -500 the
  // projected endpoint is 60, below the 80-px threshold. Even a heavily
  // smoothed EMA that damped the recent negative samples still lands
  // under threshold on this shape. Target must be 0.
  assert(c.target == 0);

  settleUntilDone(c);
  assert(c.state == State::Closed);
  assert(c.y == 0);
  // Below-threshold release must NEVER report open. This pins the whole
  // "reversing release closes despite position saying open" story.
  assert(!c.takeSettledOpen());
}

void testSettledOpenIsOneShot() {
  // The header spells out the contract exactly: "True exactly once, on the
  // frame the sheet finishes settling OPEN." Get the edge wrong and you
  // either commit repeatedly (paired account created every frame) or
  // never (silently ignored release).
  Controller c;
  c.onPress(100, 0);
  c.onMove(115, 10, kH);
  c.onMove(130, 20, kH);
  c.onMove(145, 30, kH);
  c.onRelease(kH, kThreshold);
  settleUntilDone(c);
  assert(c.state == State::Open);

  // Fires exactly once...
  assert(c.takeSettledOpen());
  // ...and only once. Any subsequent poll before a new cycle is false.
  assert(!c.takeSettledOpen());
  assert(!c.takeSettledOpen());
  assert(!c.takeSettledOpen());

  // A brand-new open cycle from Open state re-arms the one-shot: press,
  // shove down (already open, so no motion), release with high velocity.
  // The flag must fire again exactly once on the next settle.
  c.onPress(100, 200);
  c.onMove(115, 210, kH);
  c.onMove(130, 220, kH);
  c.onMove(145, 230, kH);
  c.onRelease(kH, kThreshold);
  settleUntilDone(c);
  assert(c.state == State::Open);
  assert(c.takeSettledOpen());
  assert(!c.takeSettledOpen());
}

void testTakeSettledOpenIsFalseWhenSheetSettlesClosed() {
  // The counterpart to the one-shot test: settle CLOSED must NOT fire the
  // one-shot. If it does, we commit a pairing that the operator explicitly
  // dismissed.
  Controller c;
  c.onPress(100, 0);
  c.onMove(110, 10, kH);
  c.onMove(115, 30, kH);
  c.onMove(118, 100, kH);
  c.onRelease(kH, kThreshold);
  settleUntilDone(c);
  assert(c.state == State::Closed);
  assert(!c.takeSettledOpen());
}

void testMillisRolloverMidDrag() {
  // Header: "Timestamps must be handled rollover-safely: subtract into a
  // SIGNED type." PROJECT-HISTORY R46 shipped once already because the
  // fleet-wide equivalent was `uint32_t dt = now - lastMs`, which reads
  // as ~4.29e9 when now < lastMs. Repeat that here and Controller reports
  // velocity ~= 0 across every rollover, which stops projection working
  // right at 49.7 days uptime.
  //
  // Drive a normal-shape drag whose sample timestamps just happen to
  // straddle 0xFFFFFFFF. If the impl uses signed dt (correct), the
  // velocity is a sane +1500-ish milli-px/ms throughout. If the impl
  // uses unsigned and takes the wrong side of the wrap, velocity will
  // collapse or explode.
  Controller c;
  const uint32_t start = 0xFFFFFFF0U;  // ~16 ms before rollover
  c.onPress(100, start);
  c.onMove(115, start + 10U, kH);        // last valid sample before wrap
  c.onMove(130, start + 20U, kH);        // spans rollover (start+20 wraps)
  c.onMove(145, start + 30U, kH);        // fully past rollover
  c.onMove(160, start + 40U, kH);

  // Rollover math: start + 20U = 0x00000004 in uint32. The signed dt of
  // (uint32_t)(0x04 - 0xFFFFFFFA) is 10, which is what we expect. A broken
  // impl would produce a divide-by-huge and velocity would collapse to 0
  // (or worse, garbage). Assert the velocity remains in a plausible
  // window for a steady 1.5 px/ms drag.
  assert(c.velocity > 500);      // clearly still positive and moving
  assert(c.velocity < 10000);    // and not an unsigned-underflow explosion
  assert(c.y == 60);             // 1:1 tracking survived the rollover

  // And the release still projects correctly past kProjectMs -- if any
  // stored velocity is garbage, target flips.
  c.onRelease(kH, kThreshold);
  assert(c.target == kH);
  settleUntilDone(c);
  assert(c.state == State::Open);
}

void testTwoSamplesInSameMillisecondDoNotDivideByZero() {
  // dt in the denominator. Feed two moves at the same ms. The kMinSampleMs
  // guard is there specifically so this cannot fault, but a maintainer who
  // "helpfully" tightens the guard to `dt > 0` still divides by zero on
  // the boundary. Assert the controller survives and continues to track.
  Controller c;
  c.onPress(100, 500);
  c.onMove(110, 500, kH);      // same ms as press: dt=0, velocity sample rejected
  c.onMove(120, 500, kH);      // still same ms: dt=0
  c.onMove(130, 500, kH);      // still same ms: dt=0

  // 1:1 tracking (rubber-band pass-through inside [0, h]) must still work
  // for position regardless of the velocity path -- position updates
  // don't depend on dt.
  assert(c.y == 30);
  // Velocity from the same-ms samples must be finite. If a divide-by-zero
  // occurred, velocity would be trap-representation / INT_MIN / whatever
  // the platform did with UB, which almost certainly does NOT sit inside
  // a plausibly bounded window.
  assert(c.velocity >= -1000000 && c.velocity <= 1000000);

  // And subsequent samples at legitimate dt still work.
  c.onMove(150, 520, kH);      // dt=20, +30 in 20ms -> +1500
  assert(c.y == 50);
}

void testProgressReportsFractionOfTravel() {
  // progress = y / h as an 8-bit fraction. Every visual layer reads from
  // this single value -- header: "ONE number drives everything". Endpoint
  // sanity + a mid-point sanity is enough to catch a swapped numerator /
  // denominator.
  Controller c;
  c.y = 0;
  assert(c.progress(kH) == 0);
  c.y = kH;
  assert(c.progress(kH) == 255);
  c.y = kH / 2;
  const uint8_t mid = c.progress(kH);
  // Loose middle-ish window; some impls floor, some round.
  assert(mid >= 120 && mid <= 135);
}

void testResetReturnsToClosed() {
  Controller c;
  c.onPress(100, 0);
  c.onMove(115, 10, kH);
  c.onMove(200, 20, kH);
  c.onRelease(kH, kThreshold);
  c.reset();
  assert(c.state == State::Closed);
  assert(c.y == 0);
  assert(c.velocity == 0);
  assert(!c.takeSettledOpen());
}

}  // namespace

int main() {
  testRubberBandInsideRangeIsExactlyOneToOne();
  testRubberBandOvershootBelowZeroIsDamped();
  testRubberBandOvershootPastHIsDamped();
  testRubberBandBoundariesAreExactlyOneToOne();
  testRubberBandZeroHeightDoesNotDivideByZero();

  testProjectZeroVelocityReturnsY();
  testProjectVelocityDirectionMatchesSign();
  testProjectMagnitudeMatchesFormula();
  testProjectHandlesLargeVelocityWithoutInt16Overflow();

  testChooseTargetBoundaryIsAtOrAboveThreshold();
  testChooseTargetFarAwayEndpoints();

  testSettleStepConvergesUpwardWithoutOvershoot();
  testSettleStepConvergesDownwardWithoutOvershoot();
  testSettleStepAlreadyAtTargetStaysThere();
  testSettleStepGuaranteesOnePixelProgress();

  testOnPressOnPartlyOpenSheetDoesNotJumpToFinger();
  testNothingCommitsMidDrag();
  testReleaseBelowThresholdSettlesBackToClosed();
  testFastFlickBelowThresholdCommits();
  testReversingReleaseAboveThresholdCloses();
  testSettledOpenIsOneShot();
  testTakeSettledOpenIsFalseWhenSheetSettlesClosed();
  testMillisRolloverMidDrag();
  testTwoSamplesInSameMillisecondDoNotDivideByZero();
  testProgressReportsFractionOfTravel();
  testResetReturnsToClosed();
  return 0;
}
