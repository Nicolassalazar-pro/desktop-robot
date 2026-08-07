// Host tests for the gyro-based turn measurement.
//
// The point of splitting this out of the I2C driver is that everything here
// can be wrong in a way that produces a PLAUSIBLE number, which is far more
// dangerous than an obvious failure. A clipped gyro, an uncorrected bias, or
// the wrong axis all yield a confident, wrong answer.

#include "turn_measure.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "movement_expression.h"

using namespace TurnMeasure;

namespace {

bool near(float actual, float expected, float tolerance) {
  return std::fabs(actual - expected) <= tolerance;
}

// Feeds a constant-rate spin through the integrator at a fixed sample period.
Integrator spinAt(float dps, uint32_t durationMs, uint32_t periodUs,
                  uint32_t startUs = 0) {
  Integrator integrator;
  const uint32_t totalUs = durationMs * 1000U;
  for (uint32_t t = 0; t <= totalUs; t += periodUs) {
    integrator.addSample(dps, startUs + t);
  }
  return integrator;
}

void testYawAxisComesFromGravityNotAnAssumption() {
  // Board lying flat: gravity on Z.
  assert(yawAxisFromGravity(0.02f, -0.01f, 1.0f) == Axis::Z);
  assert(yawAxisFromGravity(0.0f, 0.0f, -1.0f) == Axis::Z);
  // Board upright with the screen facing the user — the actual mounting — so
  // yaw is NOT Z. This is the case that makes hardcoding Z wrong.
  assert(yawAxisFromGravity(0.03f, -0.98f, 0.05f) == Axis::Y);
  assert(yawAxisFromGravity(0.99f, 0.02f, -0.04f) == Axis::X);
}

void testComponentForSelectsTheRightAxis() {
  assert(componentFor(Axis::X, 1.0f, 2.0f, 3.0f) == 1.0f);
  assert(componentFor(Axis::Y, 1.0f, 2.0f, 3.0f) == 2.0f);
  assert(componentFor(Axis::Z, 1.0f, 2.0f, 3.0f) == 3.0f);
  assert(strcmp(axisName(Axis::X), "X") == 0);
  assert(strcmp(axisName(Axis::Y), "Y") == 0);
  assert(strcmp(axisName(Axis::Z), "Z") == 0);
}

void testSaturationIsDetected() {
  // A clipped sample under-reads while still looking like data. If this ever
  // stops firing, a too-small full-scale range silently produces a confident
  // wrong calibration.
  assert(isSaturated(32767));
  assert(isSaturated(-32768));
  assert(isSaturated(kSaturationRaw));
  assert(!isSaturated(kSaturationRaw - 1));
  assert(!isSaturated(0));
  assert(!isSaturated(-(kSaturationRaw - 1)));
}

void testBiasEstimatorAverages() {
  BiasEstimator bias;
  assert(!bias.ready());
  assert(bias.bias() == 0.0f);
  for (uint32_t i = 0; i < BiasEstimator::kMinSamples; ++i) {
    bias.addSample(2.0f);
  }
  assert(bias.ready());
  assert(near(bias.bias(), 2.0f, 0.001f));

  bias.reset();
  assert(!bias.ready());
  assert(bias.sampleCount() == 0);
  // Symmetric noise around a real offset must average to the offset.
  for (uint32_t i = 0; i < 100; ++i) {
    bias.addSample(i % 2 == 0 ? 1.5f : -0.5f);
  }
  assert(near(bias.bias(), 0.5f, 0.001f));
}

void testIntegratesAConstantRateExactly() {
  // 360 deg/s for exactly 1000 ms is exactly one turn.
  Integrator spin = spinAt(360.0f, 1000, 1000);
  assert(near(spin.totalDegrees(), 360.0f, 0.01f));
  assert(spin.usable());
  assert(!spin.saturated());
  assert(spin.spanUs() == 1000000U);
  assert(near(spin.meanIntervalUs(), 1000.0f, 1.0f));
}

void testSampleRateDoesNotChangeAConstantRateResult() {
  // Trapezoidal integration against real elapsed time, so a slow or jittery
  // main loop must not bias a steady spin. This is what makes the non-blocking
  // sampling approach defensible.
  const float fast = spinAt(600.0f, 1200, 1000).totalDegrees();
  const float slow = spinAt(600.0f, 1200, 20000).totalDegrees();
  assert(near(fast, 720.0f, 0.01f));
  assert(near(slow, 720.0f, 0.01f));
}

void testTrapezoidHandlesARampWithoutSystematicBias() {
  // Rate ramps 0 -> 600 deg/s over 500 ms, then holds. A rectangular
  // (last-value) integrator would over- or under-count the ramp; the trapezoid
  // gets it right, which matters because a real spin accelerates.
  Integrator integrator;
  const uint32_t periodUs = 2000;
  for (uint32_t t = 0; t <= 500000U; t += periodUs) {
    const float dps = 600.0f * (static_cast<float>(t) / 500000.0f);
    integrator.addSample(dps, t);
  }
  // Area under a 0->600 ramp over 0.5 s is 150 degrees.
  assert(near(integrator.totalDegrees(), 150.0f, 0.5f));
}

void testMicrosRolloverDoesNotCorruptTheResult() {
  // micros() wraps every ~71 minutes. Unsigned subtraction makes the wrap a
  // non-event; if someone "fixes" it with signed math this fails.
  const uint32_t start = 0xFFFFF000UL;
  Integrator spin = spinAt(360.0f, 1000, 1000, start);
  assert(near(spin.totalDegrees(), 360.0f, 0.01f));
  assert(spin.spanUs() == 1000000U);
}

void testWorstGapIsReported() {
  Integrator integrator;
  integrator.addSample(100.0f, 0);
  integrator.addSample(100.0f, 1000);
  integrator.addSample(100.0f, 51000);  // a 50 ms stall
  integrator.addSample(100.0f, 52000);
  assert(integrator.worstGapUs() == 50000U);
}

void testTooFewSamplesIsNotUsable() {
  Integrator integrator;
  for (uint32_t i = 0; i < Integrator::kMinSamples - 1; ++i) {
    integrator.addSample(360.0f, i * 1000);
  }
  assert(!integrator.usable());
  integrator.addSample(360.0f, Integrator::kMinSamples * 1000);
  assert(integrator.usable());
}

void testSaturatedMeasurementIsNeverUsable() {
  Integrator spin = spinAt(360.0f, 1000, 1000);
  assert(spin.usable());
  spin.markSaturated();
  assert(!spin.usable());
  assert(spin.saturated());
}

void testResetClearsEverything() {
  Integrator spin = spinAt(360.0f, 1000, 1000);
  spin.markSaturated();
  spin.reset();
  assert(spin.sampleCount() == 0);
  assert(spin.totalDegrees() == 0.0f);
  assert(spin.spanUs() == 0);
  assert(spin.worstGapUs() == 0);
  assert(!spin.saturated());
  assert(!spin.usable());
}

void testCorrectionScalesTowardsAFullTurn() {
  // Under-rotated: 600 ms produced only 270 degrees, so it needs longer.
  // 600 * 360/270 = 800.
  assert(correctedTurnMs(600, 270.0f) == 800);
  // Over-rotated: 600 ms produced 450 degrees, so it needs less.
  // 600 * 360/450 = 480.
  assert(correctedTurnMs(600, 450.0f) == 480);
  // Already correct.
  assert(correctedTurnMs(600, 360.0f) == 600);
  // Direction of the spin must not matter, only magnitude.
  assert(correctedTurnMs(600, -270.0f) == 800);
}

void testCorrectionSnapsToTheTickGrid() {
  // Anything off-grid would be rejected by the pattern builders, so the
  // correction must never produce one.
  for (uint16_t tested = MovementExpression::kSpinMinMs;
       tested <= MovementExpression::kSpinMaxMs;
       tested += MovementExpression::kStepQuantumMs) {
    for (float degrees = 60.0f; degrees <= 1000.0f; degrees += 7.3f) {
      const uint16_t corrected = correctedTurnMs(tested, degrees);
      if (corrected == 0) continue;
      assert(MovementExpression::isValidFullTurnMs(corrected));
    }
  }
}

void testCorrectionRefusesUnusableMeasurements() {
  // A barely-moved robot must not produce a confident correction.
  assert(correctedTurnMs(600, 0.0f) == 0);
  assert(correctedTurnMs(600, 5.0f) == 0);
  assert(correctedTurnMs(600, kMinUsableDegrees - 0.1f) == 0);
  assert(correctedTurnMs(600, -kMinUsableDegrees + 0.1f) == 0);
  // Nor an implausible one.
  assert(correctedTurnMs(600, kMaxUsableDegrees + 1.0f) == 0);
  // NaN must be refused rather than propagating into the staged value.
  assert(correctedTurnMs(600, std::nanf("")) == 0);
  // A tested duration outside the calibratable range is not a valid basis.
  assert(correctedTurnMs(0, 360.0f) == 0);
  assert(correctedTurnMs(MovementExpression::kSpinMaxMs + 10, 360.0f) == 0);
  assert(correctedTurnMs(605, 360.0f) == 0);
}

void testCorrectionClampsIntoTheCalibratableRange() {
  // A wildly under-rotating turn would ideally need longer than the range
  // allows. Clamp to something buildable rather than returning a value the
  // builders reject.
  const uint16_t corrected = correctedTurnMs(2000, 50.0f);
  assert(corrected == MovementExpression::kSpinMaxMs);
  assert(MovementExpression::isValidFullTurnMs(corrected));

  // And the opposite end.
  const uint16_t fast = correctedTurnMs(200, 1800.0f);
  assert(fast == MovementExpression::kSpinMinMs);
  assert(MovementExpression::isValidFullTurnMs(fast));
}

void testCorrectionConvergesWhenIterated() {
  // The correction assumes rotation is roughly linear in time. It is applied
  // iteratively for exactly that reason, so prove the loop actually settles
  // against a deliberately NON-linear chassis: a fixed 80 ms of acceleration
  // during which only half the steady rate is achieved.
  const float steadyDps = 500.0f;
  const uint32_t rampMs = 80;
  auto degreesFor = [&](uint16_t ms) {
    if (ms <= rampMs) {
      return 0.5f * steadyDps * (static_cast<float>(ms) / 1000.0f);
    }
    const float rampDeg =
        0.5f * steadyDps * (static_cast<float>(rampMs) / 1000.0f);
    const float restDeg =
        steadyDps * (static_cast<float>(ms - rampMs) / 1000.0f);
    return rampDeg + restDeg;
  };

  uint16_t guess = 600;
  for (int iteration = 0; iteration < 6; ++iteration) {
    const uint16_t next = correctedTurnMs(guess, degreesFor(guess));
    assert(next != 0);
    if (next == guess) break;
    guess = next;
  }
  // Settled within one grid step of a true 360.
  const float finalDegrees = degreesFor(guess);
  assert(near(finalDegrees, 360.0f,
              static_cast<float>(MovementExpression::kStepQuantumMs) *
                  steadyDps / 1000.0f));
}

void testEndToEndAMeasuredSpinProducesABuildableExcited() {
  // The whole chain: bias-corrected samples -> integral -> correction ->
  // a turn the gesture builder accepts.
  BiasEstimator bias;
  for (uint32_t i = 0; i < 64; ++i) bias.addSample(1.75f);
  assert(bias.ready());

  // Chassis truly turns at 500 deg/s; the gyro reports it with the bias added.
  Integrator integrator;
  const uint16_t tested = 600;
  for (uint32_t t = 0; t <= tested * 1000U; t += 1000) {
    integrator.addSample(500.0f + 1.75f - bias.bias(), t);
  }
  assert(integrator.usable());
  assert(near(integrator.totalDegrees(), 300.0f, 0.5f));

  const uint16_t corrected = correctedTurnMs(tested, integrator.totalDegrees());
  assert(corrected == 720);  // 600 * 360/300

  MovementExpression::Step storage[MovementExpression::kExcitedStepCap];
  MovementExpression::Pattern built{};
  assert(MovementExpression::buildExcited(
      corrected, storage, MovementExpression::kExcitedStepCap, built));
  assert(MovementExpression::validate(built));
}

void testUncorrectedBiasWouldHaveChangedTheAnswer() {
  // Guards the bias step itself: if someone removes it, this shows the error
  // is large enough to matter rather than being academic.
  const uint16_t tested = 1200;
  Integrator withBias;
  Integrator without;
  for (uint32_t t = 0; t <= tested * 1000U; t += 1000) {
    withBias.addSample(300.0f + 3.0f, t);
    without.addSample(300.0f, t);
  }
  assert(near(without.totalDegrees(), 360.0f, 0.5f));
  assert(near(withBias.totalDegrees(), 363.6f, 0.5f));
  // 3 deg/s of unremoved bias is a real, visible shift in the staged value.
  assert(correctedTurnMs(tested, without.totalDegrees()) !=
         correctedTurnMs(tested, withBias.totalDegrees()));
}

void testTruncatedSpinIsRejected() {
  // A spin aborted by an emergency or by the joystick taking the drive leaves
  // a partial integral that looks like a good measurement. Catching it by
  // elapsed span is what stops a confident wrong correction.
  assert(spanCoversCommandedSpin(600000U, 600));   // full
  assert(spanCoversCommandedSpin(850000U, 600));   // full plus coast
  assert(spanCoversCommandedSpin(540000U, 600));   // 90%, the tolerance edge
  assert(!spanCoversCommandedSpin(539999U, 600));
  assert(!spanCoversCommandedSpin(300000U, 600));  // cut in half
  assert(!spanCoversCommandedSpin(0U, 600));
  assert(!spanCoversCommandedSpin(600000U, 0));
  // Must not overflow at the top of the range.
  assert(spanCoversCommandedSpin(2000000U, 2000));
  assert(!spanCoversCommandedSpin(1000000U, 2000));
}

void testClampedCorrectionIsNotMistakenForConvergence() {
  // Observed on hardware: a chassis that never completed a turn produced
  // "converged at 2000 ms", because clamping to the range limit twice in a row
  // returns the same value and the caller read equality as a fixed point.
  // A clamp is a failure to represent the answer, not agreement with it.
  const Correction low = correctTurn(2000, 50.0f);
  assert(low.ms == MovementExpression::kSpinMaxMs);
  assert(low.clamped);

  const Correction high = correctTurn(200, 1800.0f);
  assert(high.ms == MovementExpression::kSpinMinMs);
  assert(high.clamped);

  // A genuine fixed point must NOT be flagged as clamped.
  const Correction settled = correctTurn(600, 360.0f);
  assert(settled.ms == 600);
  assert(!settled.clamped);

  // The exact case that fooled it: measuring at the cap and being pinned there.
  const Correction pinned =
      correctTurn(MovementExpression::kSpinMaxMs, 101.1f);
  assert(pinned.ms == MovementExpression::kSpinMaxMs);
  assert(pinned.clamped);
  assert(pinned.ms == MovementExpression::kSpinMaxMs && pinned.clamped);

  // An unusable measurement is neither corrected nor clamped.
  const Correction none = correctTurn(600, 1.0f);
  assert(none.ms == 0);
  assert(!none.clamped);
}

void testWindowedRateExcludesRampAndTail() {
  // The window is the whole point: a mean taken across the ramp reads low, and
  // that error goes straight into every prediction made from it.
  WindowedRate rate;
  rate.reset(/*skipUs=*/400000, /*holdUs=*/1200000);
  for (uint32_t t = 0; t <= 1500000U; t += 1000) {
    // 0 -> 500 dps over the first 400 ms, then steady 500, then a coast.
    float dps;
    if (t < 400000U) {
      dps = 500.0f * (static_cast<float>(t) / 400000.0f);
    } else if (t < 1200000U) {
      dps = 500.0f;
    } else {
      dps = 120.0f;  // coasting after the command ended
    }
    rate.addSample(dps, t);
  }
  assert(rate.ready());
  // Only the steady section should have been averaged.
  assert(near(rate.meanDps(), 500.0f, 1.0f));
}

void testWindowedRateNeedsEnoughSamples() {
  WindowedRate rate;
  rate.reset(0, 1000000);
  for (uint32_t i = 0; i < WindowedRate::kMinSamples - 1; ++i) {
    rate.addSample(300.0f, i * 1000);
  }
  assert(!rate.ready());
  rate.addSample(300.0f, WindowedRate::kMinSamples * 1000);
  assert(rate.ready());
}

void testRateModelFitsAndPredicts() {
  // A chassis with a deadband: 100 -> 300 dps, 55 -> 150 dps.
  // slope = (300-150)/(100-55) = 3.3333, intercept = 300 - 333.33 = -33.33
  const RateSample samples[] = {{55, 150.0f}, {100, 300.0f}};
  const RateModel model = fitRateModel(samples, 2);
  assert(model.valid);
  assert(near(model.slope, 3.3333f, 0.01f));
  assert(near(model.intercept, -33.33f, 0.5f));
  assert(near(predictRateDps(model, 100), 300.0f, 0.5f));
  assert(near(predictRateDps(model, 55), 150.0f, 0.5f));

  // Extrapolating below the deadband must clamp at zero rather than go
  // negative and produce a nonsense duration.
  assert(predictRateDps(model, 5) == 0.0f);
}

void testRateModelUsesMagnitudeSoDirectionDoesNotMatter() {
  // Probes alternate direction, so half the samples are negative.
  const RateSample forward[] = {{55, 150.0f}, {100, 300.0f}};
  const RateSample reverse[] = {{55, -150.0f}, {100, -300.0f}};
  const RateModel a = fitRateModel(forward, 2);
  const RateModel b = fitRateModel(reverse, 2);
  assert(a.valid && b.valid);
  assert(near(a.slope, b.slope, 0.001f));
  assert(near(a.intercept, b.intercept, 0.001f));
}

void testRateModelRejectsUnusableProbes() {
  const RateSample single[] = {{100, 300.0f}};
  assert(!fitRateModel(single, 1).valid);
  assert(!fitRateModel(nullptr, 2).valid);

  // Same speed twice carries no slope information.
  const RateSample duplicate[] = {{100, 300.0f}, {100, 310.0f}};
  assert(!fitRateModel(duplicate, 2).valid);

  // Faster wheels producing no more rotation is a restrained or slipping
  // robot, not a chassis to extrapolate from. This is the exact signature of
  // the USB-tether problem, so it must be refused rather than modelled.
  const RateSample stuck[] = {{55, 90.0f}, {100, 85.0f}};
  assert(!fitRateModel(stuck, 2).valid);

  const RateSample flat[] = {{55, 100.0f}, {100, 100.0f}};
  assert(!fitRateModel(flat, 2).valid);
}

void testDurationPredictionIsBallparkCorrect() {
  const RateSample samples[] = {{55, 150.0f}, {100, 300.0f}};
  const RateModel model = fitRateModel(samples, 2);

  // At 300 dps a full turn takes 1200 ms of steady rotation, plus half of a
  // 300 ms ramp = 1350 ms.
  const uint16_t predicted = predictDurationMs(model, 100, 360.0f, 300);
  assert(near(static_cast<float>(predicted), 1350.0f, 5.0f));

  // Slower wheels must predict a longer turn.
  assert(predictDurationMs(model, 55, 360.0f, 300) > predicted);

  // No ramp allowance means pure steady-state time.
  assert(near(static_cast<float>(predictDurationMs(model, 100, 360.0f, 0)),
              1200.0f, 5.0f));
}

void testDurationPredictionRefusesWhenItCannotKnow() {
  const RateModel invalid{0.0f, 0.0f, false};
  assert(predictDurationMs(invalid, 100, 360.0f, 300) == 0);

  const RateSample samples[] = {{55, 150.0f}, {100, 300.0f}};
  const RateModel model = fitRateModel(samples, 2);
  assert(predictDurationMs(model, 100, 0.0f, 300) == 0);
  assert(predictDurationMs(model, 100, -360.0f, 300) == 0);
  // A speed below the deadband predicts zero rate, so no duration exists.
  assert(predictDurationMs(model, 5, 360.0f, 300) == 0);
}

void testPredictionBeatsBlindIterationOnASlowChassis() {
  // The measured robot: roughly 70 deg per 600 ms at full speed. Blind halving
  // or doubling wanders; the model answers in one step, and answers honestly
  // that a full turn is out of range rather than pinning at the cap.
  const RateSample samples[] = {{55, 60.0f}, {100, 130.0f}};
  const RateModel model = fitRateModel(samples, 2);
  assert(model.valid);
  const uint16_t predicted = predictDurationMs(model, 100, 360.0f, 300);
  // 360/130 s = 2769 ms + 150 = ~2919 ms. Beyond kSpinMaxMs, and knowing that
  // number is far more useful than repeatedly clamping to 2000.
  assert(predicted > MovementExpression::kSpinMaxMs);
  assert(near(static_cast<float>(predicted), 2919.0f, 20.0f));

  // And the largest angle it CAN do inside the range is worth knowing.
  const float reachable =
      predictRateDps(model, 100) *
      (static_cast<float>(MovementExpression::kSpinMaxMs) / 1000.0f);
  assert(reachable > 200.0f && reachable < 300.0f);
}

void testClosedLoopTurnStopsAtTheTarget() {
  TurnController turn;
  assert(turn.begin(360.0f, 2000, 0));
  assert(turn.active());
  assert(!turn.reachedTarget());

  // 285 dps is this chassis's measured rate; a full turn takes ~1263 ms.
  uint32_t t = 0;
  while (t <= 2000000U && !turn.reachedTarget()) {
    turn.addSample(285.0f, t);
    t += 1000;
  }
  assert(turn.reachedTarget());
  assert(near(turn.progressDegrees(), 360.0f, 1.0f));
  // Stopped close to the predicted duration rather than running the timeout.
  assert(near(static_cast<float>(t / 1000), 1263.0f, 20.0f));
}

void testClosedLoopTurnIsDirectionAgnostic() {
  // Counter-clockwise integrates negative; progress is magnitude.
  TurnController turn;
  assert(turn.begin(180.0f, 2000, 0));
  for (uint32_t t = 0; t <= 2000000U && !turn.reachedTarget(); t += 1000) {
    turn.addSample(-285.0f, t);
  }
  assert(turn.reachedTarget());
  assert(turn.turnedDegrees() < 0.0f);
  assert(near(turn.progressDegrees(), 180.0f, 1.0f));
}

void testClosedLoopTurnTimesOutOnAStuckChassis() {
  // The exact tethered failure: the robot barely rotates. The controller must
  // give up on the clock rather than drive forever waiting for an angle that
  // is never coming.
  TurnController turn;
  assert(turn.begin(360.0f, 2000, 0));
  uint32_t ms = 0;
  for (uint32_t t = 0; t <= 5000000U; t += 1000) {
    ms = t / 1000;
    if (turn.timedOut(ms)) break;
    turn.addSample(40.0f, t);  // ~40 dps, nowhere near enough
  }
  assert(turn.timedOut(ms));
  assert(!turn.reachedTarget());
  assert(ms >= 2000);
  // And it still reports how far it actually got, which is the diagnostic.
  assert(turn.progressDegrees() > 50.0f);
  assert(turn.progressDegrees() < 120.0f);
}

void testClosedLoopTurnRefusesUnsafeParameters() {
  TurnController turn;
  // An unbounded turn is the one thing that must never be allowed: a wedged
  // gyro would drive the motors indefinitely.
  assert(!turn.begin(360.0f, 0, 0));
  assert(!turn.active());

  assert(!turn.begin(0.0f, 2000, 0));
  assert(!turn.begin(-90.0f, 2000, 0));
  assert(!turn.begin(std::nanf(""), 2000, 0));
  assert(!turn.begin(kMaxUsableDegrees + 1.0f, 2000, 0));
  assert(!turn.active());
}

void testClosedLoopTurnIgnoresSamplesWhenInactive() {
  TurnController turn;
  turn.addSample(285.0f, 0);
  assert(turn.progressDegrees() == 0.0f);
  assert(!turn.timedOut(999999));

  assert(turn.begin(90.0f, 2000, 0));
  turn.stop();
  turn.addSample(285.0f, 1000);
  turn.addSample(285.0f, 2000);
  assert(turn.progressDegrees() == 0.0f);
}

void testClosedLoopBeatsTimedTurnWhenTheChassisChanges() {
  // The reason for closing the loop at all. A timed turn calibrated at 285 dps
  // is 1263 ms. If the chassis then runs 20% slower (low battery, carpet, cold
  // servos), that fixed duration under-rotates badly, while the closed loop
  // simply takes longer and still lands on 360.
  const float slowerDps = 285.0f * 0.8f;

  const float timedResult = slowerDps * (1263.0f / 1000.0f);
  assert(timedResult < 300.0f);  // ~288 deg: visibly short of a full turn

  TurnController turn;
  assert(turn.begin(360.0f, 2500, 0));
  uint32_t t = 0;
  while (t <= 2500000U && !turn.reachedTarget()) {
    turn.addSample(slowerDps, t);
    t += 1000;
  }
  assert(turn.reachedTarget());
  assert(near(turn.progressDegrees(), 360.0f, 1.0f));
}

void testGravityProjectionRecoversTheFullYawRate() {
  // The measured resting gravity on this robot. The board is tilted, so the
  // largest axis alone does NOT carry the whole yaw rate.
  const float gx = 1.01f, gy = 0.10f, gz = -0.10f;

  assert(yawAxisFromGravity(gx, gy, gz) == Axis::X);
  const float fraction = singleAxisYawFraction(Axis::X, gx, gy, gz);
  assert(near(fraction, 0.99034f, 0.0005f));
  // Which is exactly the 3.48 degrees per 360 that motivated this.
  assert(near(360.0f * (1.0f - fraction), 3.48f, 0.05f));

  // A body rotating purely about the gravity direction at 285 deg/s: the gyro
  // vector is along g_hat, scaled by the rate.
  const float mag = std::sqrt((gx * gx) + (gy * gy) + (gz * gz));
  const float rate = 285.0f;
  const float wx = rate * gx / mag;
  const float wy = rate * gy / mag;
  const float wz = rate * gz / mag;

  // Single axis under-reads...
  assert(near(componentFor(Axis::X, wx, wy, wz), rate * fraction, 0.5f));
  // ...the projection recovers it exactly.
  assert(near(yawRateFromVector(wx, wy, wz, gx, gy, gz), rate, 0.01f));
}

void testProjectionIsExactWhenPerfectlyAligned() {
  // Degenerate case: if the board were axis-aligned, the projection must agree
  // with the single-axis reading, or this change would break a level robot.
  assert(near(singleAxisYawFraction(Axis::Z, 0.0f, 0.0f, 1.0f), 1.0f, 1e-6f));
  assert(near(yawRateFromVector(0.0f, 0.0f, 200.0f, 0.0f, 0.0f, 1.0f), 200.0f,
              1e-3f));
  // And an inverted board flips the sign, as the dot product should.
  assert(near(yawRateFromVector(0.0f, 0.0f, 200.0f, 0.0f, 0.0f, -1.0f),
              -200.0f, 1e-3f));
}

void testProjectionRejectsRotationAboutOtherAxes() {
  // Rotation perpendicular to gravity is tilt, not yaw, and must contribute
  // nothing. A single-axis reading cannot make this distinction at all.
  const float gx = 1.0f, gy = 0.0f, gz = 0.0f;
  assert(near(yawRateFromVector(0.0f, 300.0f, 0.0f, gx, gy, gz), 0.0f, 1e-3f));
  assert(near(yawRateFromVector(0.0f, 0.0f, 300.0f, gx, gy, gz), 0.0f, 1e-3f));
  // Mixed: only the gravity-aligned part counts.
  assert(near(yawRateFromVector(120.0f, 300.0f, -50.0f, gx, gy, gz), 120.0f,
              1e-3f));
}

void testProjectionSurvivesADegenerateGravityRead() {
  // A failed accelerometer read must not become an enormous phantom rate.
  assert(yawRateFromVector(100.0f, 100.0f, 100.0f, 0.0f, 0.0f, 0.0f) == 0.0f);
  assert(singleAxisYawFraction(Axis::X, 0.0f, 0.0f, 0.0f) == 0.0f);
}

void testProjectionAcceptsUnnormalisedGravity() {
  // Callers pass raw accelerometer g-values, which are never exactly unit
  // length; scaling the gravity vector must not change the answer.
  const float a = yawRateFromVector(50.0f, 10.0f, -5.0f, 1.01f, 0.10f, -0.10f);
  const float b = yawRateFromVector(50.0f, 10.0f, -5.0f, 2.02f, 0.20f, -0.20f);
  const float c = yawRateFromVector(50.0f, 10.0f, -5.0f, 0.101f, 0.010f,
                                    -0.010f);
  assert(near(a, b, 1e-3f));
  assert(near(a, c, 1e-3f));
}

void testProjectionErrorGrowsWithTilt() {
  // Sanity on the relationship the whole change rests on: more tilt, more loss
  // from the single-axis shortcut.
  const float level = singleAxisYawFraction(Axis::X, 1.0f, 0.0f, 0.0f);
  const float slight = singleAxisYawFraction(Axis::X, 1.01f, 0.10f, -0.10f);
  const float bad = singleAxisYawFraction(Axis::X, 1.0f, 0.5f, 0.3f);
  assert(level > slight);
  assert(slight > bad);
  assert(near(level, 1.0f, 1e-6f));
  // A 30 degree tilt loses ~13% of the rate — 47 degrees per 360.
  assert(bad < 0.9f);
}

void testAxisEstimatorRecoversAKnownSpinAxis() {
  // The whole point: measure the rotation axis from the gyro itself, so an
  // accelerometer zero-g offset cannot masquerade as physical tilt.
  AxisEstimator est;
  assert(!est.ready());

  // A spin about a tilted axis, with realistic per-sample noise. The noise is
  // made exactly zero-mean over the loop so the test checks the estimator, not
  // the residue of its own test fixture.
  const float tx = 0.990f, ty = 0.098f, tz = -0.098f;
  for (uint32_t i = 0; i < 400; ++i) {
    const float wobble = (i % 2 == 0) ? 1.2f : -1.2f;
    est.addSample((285.0f * tx) + wobble, (285.0f * ty) - wobble,
                  (285.0f * tz) + wobble);
  }
  assert(est.ready());
  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  assert(est.axis(ax, ay, az));
  assert(near(ax, tx, 0.01f));
  assert(near(ay, ty, 0.01f));
  assert(near(az, tz, 0.01f));
  assert(near(est.meanMagnitude(), 285.0f, 2.0f));

  // Agrees with a gravity vector pointing the same way...
  assert(near(est.angleToDegrees(1.01f, 0.10f, -0.10f), 0.0f, 0.6f));
  // ...and disagrees measurably with a wrong one. This is the number that
  // settles whether an apparent tilt is real or accelerometer bias.
  const float wrong = est.angleToDegrees(1.0f, 0.0f, 0.0f);
  assert(wrong > 5.0f && wrong < 12.0f);
}

void testAxisEstimatorIsSignAgnostic() {
  // A counter-clockwise spin measures the same axis line as a clockwise one.
  AxisEstimator cw;
  AxisEstimator ccw;
  for (uint32_t i = 0; i < 200; ++i) {
    cw.addSample(280.0f, 28.0f, -28.0f);
    ccw.addSample(-280.0f, -28.0f, 28.0f);
  }
  assert(cw.ready() && ccw.ready());
  const float a = cw.angleToDegrees(1.0f, 0.1f, -0.1f);
  const float b = ccw.angleToDegrees(1.0f, 0.1f, -0.1f);
  assert(near(a, b, 0.2f));
  assert(a < 1.0f);
}

void testAxisEstimatorRefusesWithoutASustainedSpin() {
  // Stationary noise must not be mistaken for an axis — averaging noise gives
  // a direction that is pure nonsense, and it would be a confident one.
  AxisEstimator est;
  for (uint32_t i = 0; i < 500; ++i) {
    const float n = (i % 2 == 0) ? 1.0f : -1.0f;
    est.addSample(n, -n, n * 0.5f);
  }
  assert(est.sampleCount() == 500);
  assert(!est.ready());
  float x = 1.0f, y = 1.0f, z = 1.0f;
  assert(!est.axis(x, y, z));
  // Outputs untouched on refusal.
  assert(x == 1.0f && y == 1.0f && z == 1.0f);
  assert(est.angleToDegrees(1.0f, 0.0f, 0.0f) < 0.0f);

  // Too few samples is also a refusal even at a good rate.
  AxisEstimator few;
  for (uint32_t i = 0; i < AxisEstimator::kMinSamples - 1; ++i) {
    few.addSample(285.0f, 0.0f, 0.0f);
  }
  assert(!few.ready());
}

void testAxisEstimatorDistinguishesRealTiltFromAccelBias() {
  // The exact ambiguity found on this robot. Gravity read (1.01, 0.10, -0.10),
  // but the turn data showed X:Z ~ 11:1 (consistent with real tilt) while
  // X:Y ~ 50:1 (NOT consistent — suggesting the Y term is accel bias).
  // Reproduce that gyro signature and confirm the estimator reports an axis
  // that agrees with gravity in Z but not in Y.
  AxisEstimator est;
  for (uint32_t i = 0; i < 300; ++i) {
    est.addSample(-85.2f, -1.7f, 7.6f);
  }
  assert(est.ready());
  float ax = 0.0f, ay = 0.0f, az = 0.0f;
  assert(est.axis(ax, ay, az));

  // Z tilt is real: the measured axis has a Z component near the gravity one.
  assert(near(az / ax, 7.6f / -85.2f, 0.005f));
  // Y is not: the measured Y fraction is far smaller than gravity implies.
  const float gravityYFraction = 0.10f / 1.01f;      // ~0.099
  const float measuredYFraction = ay / ax;           // ~0.020
  assert(measuredYFraction < gravityYFraction * 0.5f);

  // And the disagreement with the full gravity vector is several degrees —
  // large enough to matter, which is exactly why it must be measured.
  const float angle = est.angleToDegrees(1.01f, 0.10f, -0.10f);
  assert(angle > 2.0f);
}

void testLeadCompensationStopsEarlyByThePredictedCoast() {
  // The dominant error on this robot: 7-9 degrees of coast after the motors
  // cut, at 285 deg/s. That is ~0.03 s of equivalent rotation.
  const float rate = 285.0f;
  const float leadSec = 0.030f;

  TurnController turn;
  assert(turn.begin(360.0f, 3000, 0));
  turn.setLeadSeconds(leadSec);
  assert(near(turn.leadSeconds(), leadSec, 1e-6f));

  uint32_t t = 0;
  while (t <= 3000000U && !turn.reachedTarget()) {
    turn.addSample(rate, t);
    t += 1000;
  }
  assert(turn.reachedTarget());

  // It should have stopped short of the target by exactly the predicted coast.
  const float predicted = rate * leadSec;   // 8.55 deg
  assert(near(turn.progressDegrees(), 360.0f - predicted, 1.5f));
  assert(near(turn.predictedCoastDegrees(), predicted, 0.01f));

  // So the FINAL resting angle, after coasting that far, lands on target.
  const float finalAngle = turn.progressDegrees() + predicted;
  assert(near(finalAngle, 360.0f, 1.5f));
}

void testLeadScalesWithRateSoSlowTurnsAreNotShortchanged() {
  // Coast is proportional to speed, so a slow turn must lead LESS. A fixed
  // angular lead would make slow turns systematically undershoot.
  const float leadSec = 0.030f;

  TurnController fast;
  fast.begin(180.0f, 4000, 0);
  fast.setLeadSeconds(leadSec);
  uint32_t t = 0;
  while (t <= 4000000U && !fast.reachedTarget()) {
    fast.addSample(285.0f, t);
    t += 1000;
  }
  const float fastLead = 180.0f - fast.progressDegrees();

  TurnController slow;
  slow.begin(180.0f, 6000, 0);
  slow.setLeadSeconds(leadSec);
  t = 0;
  while (t <= 6000000U && !slow.reachedTarget()) {
    slow.addSample(90.0f, t);
    t += 1000;
  }
  const float slowLead = 180.0f - slow.progressDegrees();

  assert(fastLead > slowLead);
  assert(near(fastLead / slowLead, 285.0f / 90.0f, 0.4f));
}

void testZeroLeadIsTheOldBehaviour() {
  // Lead must be opt-in: with it disabled the controller stops exactly on the
  // measured angle, as before.
  TurnController turn;
  turn.begin(90.0f, 2000, 0);
  turn.setLeadSeconds(0.0f);
  uint32_t t = 0;
  while (t <= 2000000U && !turn.reachedTarget()) {
    turn.addSample(285.0f, t);
    t += 1000;
  }
  assert(near(turn.progressDegrees(), 90.0f, 0.5f));
  assert(turn.predictedCoastDegrees() == 0.0f);
}

void testNegativeLeadIsRefused() {
  // A negative lead would stop LATE and compound the very overshoot it exists
  // to remove — clamp rather than trust the caller.
  TurnController turn;
  turn.begin(90.0f, 2000, 0);
  turn.setLeadSeconds(-0.05f);
  assert(turn.leadSeconds() == 0.0f);
}

void testLeadCannotCauseAnInstantStop() {
  // Guard against a pathological lead: at the very first sample the rate is
  // whatever was just measured, and a huge lead could satisfy the target
  // immediately, producing a turn that never moves.
  TurnController turn;
  turn.begin(90.0f, 2000, 0);
  turn.setLeadSeconds(1.0f);          // absurd: 285 deg of predicted coast
  turn.addSample(285.0f, 0);
  // It WILL report reached immediately — which is why the caller must bound
  // the lead. Pin the behaviour so the hazard is documented rather than
  // discovered on hardware.
  assert(turn.reachedTarget());
  assert(turn.predictedCoastDegrees() > 90.0f);
}

void testLeadUsesASmoothedRateNotASingleNoisySample() {
  // A single sample carries the full gyro noise. The lead must be driven by a
  // short average so one spike cannot move the stop decision.
  TurnController turn;
  turn.begin(360.0f, 3000, 0);
  turn.setLeadSeconds(0.030f);

  // Steady 200 dps, then one absurd spike.
  turn.addSample(200.0f, 0);
  turn.addSample(200.0f, 1000);
  turn.addSample(200.0f, 2000);
  turn.addSample(1000.0f, 3000);

  // Raw last sample is the spike; the smoothed rate is not.
  assert(near(turn.lastRateDps(), 1000.0f, 0.01f));
  const float smoothed = turn.smoothedRateDps();
  assert(smoothed < 500.0f);
  assert(smoothed > 200.0f);
  // Prediction follows the smoothed rate, so the spike is attenuated ~4x.
  assert(near(turn.predictedCoastDegrees(), smoothed * 0.030f, 0.01f));
}

void testSmoothedRateIsZeroBeforeAnySample() {
  TurnController turn;
  turn.begin(90.0f, 2000, 0);
  turn.setLeadSeconds(0.030f);
  assert(turn.smoothedRateDps() == 0.0f);
  assert(turn.predictedCoastDegrees() == 0.0f);
  // With no motion and no progress the target cannot already be satisfied.
  assert(!turn.reachedTarget());
}

void testBiasSpreadSeparatesDriftedBiasFromMotion() {
  // The whole point of the spread gate: a steady non-zero rate (bias that has
  // drifted) must stay ACCEPTABLE, while a varying rate (being nudged or still
  // coasting) must be rejected. A magnitude threshold cannot tell them apart.
  BiasEstimator drifted;
  for (int i = 0; i < 64; ++i) {
    drifted.addSample(5.0f + ((i % 2) ? 0.05f : -0.05f));   // big mean, tiny spread
  }
  assert(drifted.ready());
  assert(near(drifted.bias(), 5.0f, 0.02f));
  assert(drifted.spreadDps() < 0.5f);

  BiasEstimator moving;
  for (int i = 0; i < 64; ++i) {
    moving.addSample((i % 2) ? 6.0f : -6.0f);   // mean ~0, huge spread
  }
  assert(moving.ready());
  assert(near(moving.bias(), 0.0f, 0.5f));
  assert(moving.spreadDps() > 10.0f);

  // A pure magnitude test on the MEAN would accept the moving case (mean ~0)
  // and could reject the drifted one. Spread inverts both, correctly.
  assert(moving.spreadDps() > drifted.spreadDps());
}

void testBiasSpreadIsZeroBeforeAndAfterReset() {
  BiasEstimator b;
  assert(b.spreadDps() == 0.0f);
  b.addSample(3.0f);
  assert(b.spreadDps() == 0.0f);   // one sample has no spread
  b.addSample(-1.0f);
  assert(near(b.spreadDps(), 4.0f, 1e-5f));
  b.reset();
  assert(b.spreadDps() == 0.0f);
  assert(b.sampleCount() == 0);
}

void testBiasSpreadTracksExtremesNotRecency() {
  // A single early excursion must still be visible at the end of the window;
  // otherwise a nudge at the start of a ZUPT gets absorbed.
  BiasEstimator b;
  b.addSample(20.0f);
  for (int i = 0; i < 63; ++i) b.addSample(0.5f);
  assert(b.ready());
  assert(b.spreadDps() > 19.0f);
}

void testProjectionMakesEveryAxisBiasReachHeading() {
  // Regression for a real bug measured on the robot: heading drifted 3.39 deg
  // over 65 s after the gravity projection went live, versus 0.05 deg before.
  //
  // Cause: the projection is a weighted sum of ALL THREE gyro axes, so all
  // three biases reach heading, weighted by the gravity unit vector. The
  // zero-velocity update was only re-estimating the dominant axis, so the other
  // two leaked in uncorrected. A single-axis integrator never read Y or Z at
  // all, which is why the bug only appeared once the projection was added.
  //
  // Board tilt as measured: g = (0.97, 0.08, -0.27).
  const float gx = 0.97f, gy = 0.08f, gz = -0.27f;
  const float mag = sqrtf(gx * gx + gy * gy + gz * gz);

  // A pure Z-axis bias, with X and Y perfectly corrected.
  const float biasZ = 0.2f;
  const float leaked = yawRateFromVector(0.0f, 0.0f, biasZ, gx, gy, gz);

  // It does NOT vanish: it arrives scaled by the Z component of gravity.
  const float expected = biasZ * (gz / mag);
  assert(near(leaked, expected, 1e-4f));
  assert(fabsf(leaked) > 0.04f);   // ~0.052 dps, matching the observed drift

  // Over 65 seconds that is degrees, not hundredths of a degree - which is
  // exactly what was measured.
  const float driftOver65s = fabsf(leaked) * 65.0f;
  assert(driftOver65s > 3.0f);

  // Correcting all three axes removes it entirely.
  const float corrected =
      yawRateFromVector(0.0f - 0.0f, 0.0f - 0.0f, biasZ - biasZ, gx, gy, gz);
  assert(near(corrected, 0.0f, 1e-6f));
}

void testQuadraticCoastFitsTheMeasuredBenchData() {
  // Regression against real numbers from two supervised untethered bench runs.
  // A linear model CANNOT fit these: coast/rate ranges 0.0368 -> 0.0616 s
  // across the speed range, a 1.7x spread.
  //
  // alpha is 2600, chosen from the closed-loop A/B rather than the block means:
  // running live at 2100 measured a -4.49 deg error, i.e. it OVER-led and
  // undershot. At 2600 the model deliberately under-predicts the measured
  // coast, because under-leading leaves a recoverable overshoot while
  // over-leading leaves an undershoot smaller than the servo deadband can fix.
  struct Case { float rate; float observedCoast; };
  const Case cases[] = {
      {283.2f, 17.40f},   // speed 100, run 002
      {195.0f,  8.76f},   // speed 75
      {136.1f,  5.36f},   // speed 55
  };
  for (const Case& c : cases) {
    TurnController t;
    t.begin(360.0f, 4000, 0);
    t.setCoastAlpha(2600.0f);
    for (int i = 0; i < 8; ++i) t.addSample(c.rate, i * 1000);
    const float predicted = t.predictedCoastDegrees();
    // Under-predicts at every measured speed - the safe direction.
    assert(predicted < c.observedCoast);
    // But not so far under that the compensation stops being worth having.
    assert(predicted > c.observedCoast - 2.5f);
  }
}

void testQuadraticModelUnderPredictsAtLowSpeedWhichIsTheSafeDirection() {
  // Under-leading leaves a small OVERSHOOT; over-leading leaves an undershoot
  // that would need a second nudge smaller than the servo deadband can
  // deliver. Run 002 proved this matters: at alpha 2100 the compensation
  // over-led and flipped the error negative (-4.49 deg).
  TurnController turn;
  turn.begin(360.0f, 4000, 0);
  turn.setCoastAlpha(2600.0f);
  for (int i = 0; i < 8; ++i) turn.addSample(136.1f, i * 1000);
  const float predicted = turn.predictedCoastDegrees();
  assert(predicted < 5.36f);   // under-predicts, i.e. stops later
  assert(predicted > 3.0f);    // but is not wildly off
}

void testCoastAlphaScalesQuadraticallyNotLinearly() {
  // The property that makes this model different from the old one: doubling
  // the rate must roughly QUADRUPLE the predicted coast.
  TurnController slow;
  slow.begin(360.0f, 4000, 0);
  slow.setCoastAlpha(2100.0f);
  for (int i = 0; i < 8; ++i) slow.addSample(130.0f, i * 1000);

  TurnController fast;
  fast.begin(360.0f, 4000, 0);
  fast.setCoastAlpha(2100.0f);
  for (int i = 0; i < 8; ++i) fast.addSample(260.0f, i * 1000);

  const float ratio = fast.predictedCoastDegrees() / slow.predictedCoastDegrees();
  assert(near(ratio, 4.0f, 0.05f));
}

void testNegativeAlphaDisablesRatherThanInverts() {
  // A negative deceleration would produce a negative predicted coast, i.e. a
  // lead that stops the turn LATE. Refuse it.
  TurnController turn;
  turn.begin(360.0f, 4000, 0);
  turn.setCoastAlpha(-500.0f);
  assert(turn.coastAlpha() == 0.0f);
  for (int i = 0; i < 4; ++i) turn.addSample(265.0f, i * 1000);
  assert(turn.predictedCoastDegrees() == 0.0f);
}

void testLinearAndQuadraticTermsAdd() {
  // Both terms are kept so the model can be degraded to the old behaviour if
  // the quadratic fit ever stops holding. They must compose, not override.
  TurnController turn;
  turn.begin(360.0f, 4000, 0);
  turn.setLeadSeconds(0.010f);
  turn.setCoastAlpha(2100.0f);
  for (int i = 0; i < 8; ++i) turn.addSample(200.0f, i * 1000);
  const float expected = 200.0f * 0.010f + (200.0f * 200.0f) / (2.0f * 2100.0f);
  assert(near(turn.predictedCoastDegrees(), expected, 0.01f));
}

void testHeadingHoldTrimsAgainstTheError() {
  // The only feedback available for driving straight: two hobby servos never
  // match, there are no wheel encoders, so the gyro's heading change is the
  // sole observable that the robot is curving.
  HeadingHold hold;
  hold.begin(/*gain=*/2.0f, /*maxTrim=*/25);

  // Dead on course: no trim, so a straight run is not perturbed by the
  // controller itself.
  assert(hold.trimFor(0.0f) == 0);

  // Drifted left (positive heading): trim must push the OTHER way.
  const int8_t left = hold.trimFor(5.0f);
  const int8_t right = hold.trimFor(-5.0f);
  assert(left < 0);
  assert(right > 0);
  assert(left == -right);
  assert(left == -10);   // 5 deg * gain 2
}

void testHeadingHoldClampsSoACorrectionNeverBecomesAPivot() {
  // An unbounded trim would stall or reverse a wheel on a large error, turning
  // a course correction into a pivot - which destroys the straight-line
  // assumption the distance estimate depends on.
  HeadingHold hold;
  hold.begin(2.0f, 25);
  assert(hold.trimFor(90.0f) == -25);
  assert(hold.trimFor(-90.0f) == 25);
  assert(hold.trimFor(1000.0f) == -25);
}

void testHeadingHoldDisabledUntilBegun() {
  HeadingHold hold;
  assert(hold.trimFor(20.0f) == 0);
  // And a nonsensical configuration disables rather than misbehaves.
  hold.begin(-1.0f, 25);
  assert(hold.trimFor(20.0f) == 0);
  hold.begin(2.0f, -5);
  assert(hold.trimFor(20.0f) == 0);
}

void testDisplacementRecoversACleanTrapezoidalProfile() {
  // Accelerate, cruise, decelerate to a stop over 5 s — the shape of one
  // survey leg. With no sensor error the answer must be exact.
  DisplacementEstimator est;
  est.begin(0.0f, 0.0f, 1.0f);   // resting: 1 g on Z, no bias

  const float dtS = 0.002f;      // 500 Hz, the IMU's real rate
  const float accel = 0.2f;      // m/s^2
  const float accelPhase = 1.0f; // s
  const float cruise = 3.0f;
  uint32_t us = 0;
  float t = 0.0f;
  while (t < 5.0f - 1e-6f) {
    float a = 0.0f;
    if (t < accelPhase) a = accel;
    else if (t >= accelPhase + cruise) a = -accel;
    // Reported in g, with gravity present on Z as the sensor would see it.
    est.addSample(a / 9.80665f, 0.0f, 1.0f, us);
    us += static_cast<uint32_t>(dtS * 1e6f);
    t += dtS;
  }

  // v peaks at 0.2 m/s; distance = 0.2*1/2 + 0.2*3 + 0.2*1/2 = 0.8 m
  assert(near(est.rawDistanceMetres(), 0.8f, 0.01f));
  assert(near(est.residualSpeedMetresPerSec(), 0.0f, 0.005f));
  assert(near(est.correctedDistanceMetres(), 0.8f, 0.01f));
}

void testZeroVelocityConstraintRemovesAccelBias() {
  // The property the whole approach depends on. A constant acceleration error
  // of 1 mg over 5 s puts 0.12 m of pure error into the raw integral; because
  // the robot is KNOWN to be stopped at the end, that error is observable in
  // the leftover velocity and can be removed.
  DisplacementEstimator est;
  est.begin(0.0f, 0.0f, 1.0f);

  const float dtS = 0.002f;
  const float bias = 0.001f;   // 1 mg, in g
  uint32_t us = 0;
  for (float t = 0.0f; t < 5.0f - 1e-6f; t += dtS) {
    est.addSample(bias, 0.0f, 1.0f, us);   // stationary robot, biased sensor
    us += static_cast<uint32_t>(dtS * 1e6f);
  }

  // Raw integration invents motion that never happened.
  assert(est.rawDistanceMetres() > 0.08f);
  // The leftover velocity exposes it...
  assert(est.residualSpeedMetresPerSec() > 0.03f);
  // ...and correcting for it recovers the truth: the robot did not move.
  assert(near(est.correctedDistanceMetres(), 0.0f, 0.01f));
}

void testResidualSpeedIsTheConfidenceMeasure() {
  // A large leftover velocity means the leg's result should not be trusted,
  // so it has to be reported rather than swallowed by the correction.
  DisplacementEstimator clean;
  clean.begin(0.0f, 0.0f, 1.0f);
  DisplacementEstimator biased;
  biased.begin(0.0f, 0.0f, 1.0f);

  const float dtS = 0.002f;
  uint32_t us = 0;
  for (float t = 0.0f; t < 3.0f - 1e-6f; t += dtS) {
    clean.addSample(0.0f, 0.0f, 1.0f, us);
    biased.addSample(0.005f, 0.0f, 1.0f, us);   // 5 mg
    us += static_cast<uint32_t>(dtS * 1e6f);
  }
  assert(clean.residualSpeedMetresPerSec() <
         biased.residualSpeedMetresPerSec());
}

void testDisplacementIgnoresAbsurdTimeGaps() {
  // A stalled loop must not inject a fictitious displacement, which at 0.5*a*t^2
  // could be metres from a single bad sample.
  DisplacementEstimator est;
  est.begin(0.0f, 0.0f, 1.0f);
  est.addSample(0.1f, 0.0f, 1.0f, 0);
  est.addSample(0.1f, 0.0f, 1.0f, 2000);          // 2 ms, fine
  const float before = est.rawDistanceMetres();
  est.addSample(0.1f, 0.0f, 1.0f, 2000 + 3000000); // 3 s gap, rejected
  assert(near(est.rawDistanceMetres(), before, 1e-6f));
}

void testRestingVectorRemovesStaticTiltAndBiasTogether() {
  // A tilted, biased sensor sitting still must read zero distance. Static tilt
  // and static bias are not separable from one stationary reading, and they do
  // not need to be — subtracting the resting vector cancels both.
  DisplacementEstimator est;
  est.begin(0.087f, 0.0f, 0.996f);   // ~5 degrees of tilt, plus whatever bias
  const float dtS = 0.002f;
  uint32_t us = 0;
  for (float t = 0.0f; t < 5.0f - 1e-6f; t += dtS) {
    est.addSample(0.087f, 0.0f, 0.996f, us);
    us += static_cast<uint32_t>(dtS * 1e6f);
  }
  assert(near(est.rawDistanceMetres(), 0.0f, 0.001f));
  assert(near(est.correctedDistanceMetres(), 0.0f, 0.001f));
}

}  // namespace

int main() {
  testYawAxisComesFromGravityNotAnAssumption();
  testGravityProjectionRecoversTheFullYawRate();
  testProjectionIsExactWhenPerfectlyAligned();
  testProjectionRejectsRotationAboutOtherAxes();
  testProjectionSurvivesADegenerateGravityRead();
  testProjectionAcceptsUnnormalisedGravity();
  testProjectionErrorGrowsWithTilt();
  testAxisEstimatorRecoversAKnownSpinAxis();
  testAxisEstimatorIsSignAgnostic();
  testAxisEstimatorRefusesWithoutASustainedSpin();
  testAxisEstimatorDistinguishesRealTiltFromAccelBias();
  testComponentForSelectsTheRightAxis();
  testSaturationIsDetected();
  testBiasEstimatorAverages();
  testIntegratesAConstantRateExactly();
  testSampleRateDoesNotChangeAConstantRateResult();
  testTrapezoidHandlesARampWithoutSystematicBias();
  testMicrosRolloverDoesNotCorruptTheResult();
  testWorstGapIsReported();
  testTooFewSamplesIsNotUsable();
  testSaturatedMeasurementIsNeverUsable();
  testResetClearsEverything();
  testCorrectionScalesTowardsAFullTurn();
  testCorrectionSnapsToTheTickGrid();
  testCorrectionRefusesUnusableMeasurements();
  testCorrectionClampsIntoTheCalibratableRange();
  testCorrectionConvergesWhenIterated();
  testEndToEndAMeasuredSpinProducesABuildableExcited();
  testUncorrectedBiasWouldHaveChangedTheAnswer();
  testTruncatedSpinIsRejected();
  testClampedCorrectionIsNotMistakenForConvergence();
  testWindowedRateExcludesRampAndTail();
  testWindowedRateNeedsEnoughSamples();
  testRateModelFitsAndPredicts();
  testRateModelUsesMagnitudeSoDirectionDoesNotMatter();
  testRateModelRejectsUnusableProbes();
  testDurationPredictionIsBallparkCorrect();
  testDurationPredictionRefusesWhenItCannotKnow();
  testPredictionBeatsBlindIterationOnASlowChassis();
  testClosedLoopTurnStopsAtTheTarget();
  testClosedLoopTurnIsDirectionAgnostic();
  testClosedLoopTurnTimesOutOnAStuckChassis();
  testClosedLoopTurnRefusesUnsafeParameters();
  testClosedLoopTurnIgnoresSamplesWhenInactive();
  testClosedLoopBeatsTimedTurnWhenTheChassisChanges();
  testLeadCompensationStopsEarlyByThePredictedCoast();
  testLeadScalesWithRateSoSlowTurnsAreNotShortchanged();
  testZeroLeadIsTheOldBehaviour();
  testNegativeLeadIsRefused();
  testLeadCannotCauseAnInstantStop();
  testLeadUsesASmoothedRateNotASingleNoisySample();
  testSmoothedRateIsZeroBeforeAnySample();
  testBiasSpreadSeparatesDriftedBiasFromMotion();
  testBiasSpreadIsZeroBeforeAndAfterReset();
  testBiasSpreadTracksExtremesNotRecency();
  testProjectionMakesEveryAxisBiasReachHeading();
  testQuadraticCoastFitsTheMeasuredBenchData();
  testQuadraticModelUnderPredictsAtLowSpeedWhichIsTheSafeDirection();
  testCoastAlphaScalesQuadraticallyNotLinearly();
  testNegativeAlphaDisablesRatherThanInverts();
  testLinearAndQuadraticTermsAdd();
  testHeadingHoldTrimsAgainstTheError();
  testHeadingHoldClampsSoACorrectionNeverBecomesAPivot();
  testHeadingHoldDisabledUntilBegun();
  testDisplacementRecoversACleanTrapezoidalProfile();
  testZeroVelocityConstraintRemovesAccelBias();
  testResidualSpeedIsTheConfidenceMeasure();
  testDisplacementIgnoresAbsurdTimeGaps();
  testRestingVectorRemovesStaticTiltAndBiasTogether();
  printf("turn-measure tests passed\n");
  return 0;
}
