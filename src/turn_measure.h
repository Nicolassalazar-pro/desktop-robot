#pragma once

// ============================================================================
// turn_measure.h — turning a stream of gyro samples into "how far did it
// actually rotate", and into a corrected full-turn duration.
//
// WHY A GYRO AND NOT A COMPASS.
// The QMI8658C is a 6-axis IMU: accelerometer + gyroscope, no magnetometer.
// That is not a limitation here — a magnetometer would be the WORSE instrument
// for this job. Two DC motors, a LiPo under load and steel gearing sit inches
// away, and all three wreck a magnetic heading. A gyro measures angular RATE
// directly, and over the one or two seconds a spin lasts, integration drift is
// negligible.
//
// Everything in this file is deliberately free of Arduino and I2C so the part
// that can actually be wrong — axis choice, bias handling, integration,
// saturation, the correction arithmetic — is unit-tested on the host. The
// driver that talks to the chip stays thin on purpose.
// ============================================================================

#include <stdint.h>

namespace TurnMeasure {

enum class Axis : uint8_t {
  X,
  Y,
  Z,
};

const char* axisName(Axis axis);
float componentFor(Axis axis, float x, float y, float z);

// At rest the only sustained acceleration is gravity, so the axis with the
// largest magnitude is vertical — and the robot yaws about vertical. This is
// measured rather than assumed because the board is mounted upright in the
// body (screen facing the user), so yaw is NOT simply Z.
//
// Still useful for reporting and for a coarse sanity check, but the projection
// below is the accurate way to extract yaw rate.
Axis yawAxisFromGravity(float ax, float ay, float az);

// Extracts the yaw rate from a full gyro vector using the measured gravity
// direction, rather than taking a single axis.
//
// WHY THIS MATTERS. Taking one axis is only correct if that axis is exactly
// aligned with gravity. Measured on this robot, resting gravity is
// (1.01, 0.10, -0.10) g, so the largest axis carries just 99.03% of the true
// yaw rate — every 360 understates by about 3.5 degrees. Projecting the gyro
// vector onto the gravity unit vector recovers it:
//
//     omega_yaw = omega . g_hat
//
// The sign convention follows g_hat, so a robot resting with gravity on -X
// reports the opposite sign to one resting on +X. Callers that only care about
// magnitude are unaffected; callers that care about direction should establish
// the convention once from a known rotation.
//
// `gravity` need not be normalised — this normalises it. Returns 0 for a
// degenerate (zero-length) gravity vector rather than dividing by zero, which
// would otherwise turn a bad accelerometer read into an enormous phantom rate.
float yawRateFromVector(float gyroX, float gyroY, float gyroZ,
                        float gravityX, float gravityY, float gravityZ);

// The fraction of the true yaw rate that a single-axis reading captures, given
// a gravity vector: |g_axis| / |g|. 1.0 when perfectly aligned. Exposed so the
// cost of the single-axis shortcut can be reported rather than assumed.
float singleAxisYawFraction(Axis axis, float gravityX, float gravityY,
                            float gravityZ);


// A gyro reading this close to full scale has probably been clipped. A clipped
// spin silently under-reads, which looks like a plausible measurement — the
// worst possible failure — so it is detected and reported instead.
constexpr int16_t kSaturationRaw = 32000;
bool isSaturated(int16_t raw);

// Below this, whatever was measured is not a turn: noise, a nudge, or a spin
// that never started. Refusing is better than "correcting" from it.
constexpr float kMinUsableDegrees = 45.0f;
// Above this the reading is not credible for a single commanded turn.
constexpr float kMaxUsableDegrees = 2000.0f;

// Zero-rate offset. A gyro at rest does not read zero, and an uncorrected
// offset integrates straight into the answer: 2 deg/s of bias over a 1.2 s
// spin is 2.4 degrees of pure error.
class BiasEstimator {
 public:
  BiasEstimator();
  void reset();
  void addSample(float dps);
  uint32_t sampleCount() const;
  bool ready() const;
  float bias() const;

  // Peak-to-peak spread of the samples collected so far.
  //
  // This is what separates the two situations a single magnitude threshold
  // conflates. A robot sitting still with a drifted bias reads a steady
  // non-zero rate: large mean, TINY spread — and it urgently needs a new bias
  // estimate. A robot being picked up, nudged or still coasting reads a
  // varying rate: large spread — and must be rejected.
  //
  // Gating on spread therefore closes the bias-poisoning hole without the
  // failure mode of simply tightening the magnitude threshold, which would
  // lock out the zero-velocity update precisely when bias drift has made it
  // most necessary.
  float spreadDps() const;

  static constexpr uint32_t kMinSamples = 32;

 private:
  double sum_;
  uint32_t count_;
  float min_;
  float max_;
};

// Trapezoidal integration of angular rate against real elapsed time, so a
// jittery or variable sample interval does not bias the result.
class Integrator {
 public:
  Integrator();
  void reset();

  // `dps` must already be bias-corrected. `nowUs` is a free-running
  // microsecond clock; unsigned subtraction makes wraparound harmless.
  void addSample(float dps, uint32_t nowUs);
  void markSaturated();

  float totalDegrees() const;
  // NOT named degrees(). <Arduino.h> defines a function-like macro
  // `degrees(rad)`, so any `x.degrees()` call site silently macro-expands into
  // garbage and the compiler reports the error inside Arduino.h rather than at
  // the call. Host tests never include Arduino.h, so this only breaks the
  // firmware build — cost an entire confusing build failure to find once.
  uint32_t sampleCount() const;
  uint32_t spanUs() const;
  uint32_t worstGapUs() const;
  bool saturated() const;
  // 0 when fewer than two samples have been seen.
  float meanIntervalUs() const;

  // Enough samples, spread over enough time, with nothing clipped. Reported
  // rather than silently assumed, because a slow main loop degrades this
  // quietly.
  bool usable() const;

  static constexpr uint32_t kMinSamples = 24;

 private:
  bool has_;
  bool saturated_;
  float lastDps_;
  uint32_t lastUs_;
  uint32_t firstUs_;
  uint32_t worstGapUs_;
  uint32_t count_;
  double degrees_;
};

// A spin that was cut short — by an emergency, the joystick taking the drive,
// or a pause — still leaves a partial integral behind. That integral looks
// like a perfectly good measurement and would produce a confident, wrong
// correction, so the elapsed span is checked against what was commanded.
// Tolerance is generous because the coast after the last step is included and
// sampling may start a moment late; it only has to catch truncation.
bool spanCoversCommandedSpin(uint32_t spanUs, uint16_t commandedMs);


// Estimates the actual axis of rotation from the gyro itself, by averaging the
// gyro vector over a sustained one-directional spin.
//
// WHY NOT JUST USE GRAVITY. The accelerometer defines "down", and for a robot
// pivoting on a flat surface that should equal the spin axis. But a consumer
// accelerometer has a zero-g offset spec of the same order as the tilt being
// measured (±100 mg on this part), so a gravity vector like (1.01, 0.10, -0.10)
// cannot distinguish 8 degrees of real tilt from 100 mg of sensor bias. Getting
// that wrong does not merely lose the correction — it applies one of the same
// size in the wrong direction.
//
// The gyro has no such ambiguity: during a pure yaw spin the gyro vector points
// along the rotation axis by definition. Averaging it over the steady part of a
// spin measures the axis directly, and cross-checking it against gravity says
// how much of the apparent tilt is real.
class AxisEstimator {
 public:
  AxisEstimator();

  void reset();
  // Bias-corrected gyro vector, in any consistent units.
  void addSample(float x, float y, float z);

  uint32_t sampleCount() const;
  bool ready() const;

  // Mean rotation rate magnitude. Small means there was no sustained spin and
  // the direction is meaningless.
  float meanMagnitude() const;

  // Normalised mean direction. False if there was no usable rotation, in which
  // case the outputs are untouched.
  bool axis(float& x, float& y, float& z) const;

  // Angle in degrees between this measured axis and a supplied vector
  // (typically the gravity direction). Negative if not computable. This is the
  // number that says whether an apparent tilt is real.
  float angleToDegrees(float vx, float vy, float vz) const;

  static constexpr uint32_t kMinSamples = 64;
  // Below this mean rate there was no meaningful spin to take a direction from.
  static constexpr float kMinMeanDps = 20.0f;

 private:
  double sumX_;
  double sumY_;
  double sumZ_;
  uint32_t count_;
};


//
// This is the measurement that makes prediction possible. Total degrees over a
// whole spin conflates three different things — ramp-up, steady rotation and
// coast — so it can only ever be corrected by trial and error. A steady-state
// rate is a property of the chassis at a given wheel command, and once two of
// them are known the duration for any angle can be computed instead of guessed.
class WindowedRate {
 public:
  WindowedRate();

  // `skipUs` after the first sample is ignored (ramp), and sampling stops
  // `holdUs` after that first sample.
  void reset(uint32_t skipUs, uint32_t holdUs);
  void addSample(float dps, uint32_t nowUs);

  bool ready() const;
  float meanDps() const;
  uint32_t sampleCount() const;

  static constexpr uint32_t kMinSamples = 24;

 private:
  bool has_;
  uint32_t firstUs_;
  uint32_t skipUs_;
  uint32_t holdUs_;
  double sum_;
  uint32_t count_;
};

// One characterisation point: the steady yaw rate produced by a wheel command.
struct RateSample {
  int8_t speed;
  float dps;
};

// rate(speed) = slope * speed + intercept.
//
// Continuous-rotation servos have a deadband, so the line does not pass through
// the origin and a single point extrapolated from zero would be wrong. Two or
// more points are fitted by least squares.
struct RateModel {
  float slope;
  float intercept;
  bool valid;
};

RateModel fitRateModel(const RateSample* samples, uint8_t count);
float predictRateDps(const RateModel& model, int8_t speed);

// Duration to sweep `degrees` at `speed`, allowing for the standing start.
// During a ramp of `rampMs` the chassis averages roughly half its steady rate,
// so about half the ramp is lost rotation and is added back as time.
// Returns 0 when the model cannot support a prediction.
uint16_t predictDurationMs(const RateModel& model, int8_t speed, float degrees,
                           uint16_t rampMs);

// Closed-loop turn: drives until the gyro says the target angle has been swept.
//
// This is strictly better than a timed turn for accuracy — it is immune to
// battery level, surface grip, motor warmth and wheel slip, none of which a
// fixed duration can adapt to. The tradeoff is that net-zero stops being a
// compile-time property of the pattern and becomes a runtime property of the
// sensor, so the timeout is not optional: it is what stops a wedged or
// disconnected gyro from driving the motors indefinitely.
//
// Drift is a non-issue at this scale. One full turn takes about 1.3 s, so even
// a sloppy 0.5 dps residual bias contributes under a degree of error.
class TurnController {
 public:
  TurnController();

  // `targetDegrees` must be positive; direction is whichever way the caller
  // drives. `timeoutMs` bounds the drive regardless of what the sensor says.
  bool begin(float targetDegrees, uint32_t timeoutMs, uint32_t nowMs);
  void stop();

  void addSample(float dps, uint32_t nowUs);

  // Lead compensation: stop early so the angle the chassis comes to REST at is
  // the target, rather than the angle at which power was cut.
  //
  // TWO terms, because the measured data demanded it. A bench run at three
  // speeds showed the implied linear constant `coast/rate` varying 1.7x across
  // the range (0.0368 s at ~125 dps to 0.0616 s at ~265 dps) — so coast is NOT
  // proportional to rate, and a single lead time cannot fit it. Coulomb
  // friction predicts `coast = w^2/(2*alpha)`, and that fits the two fastest
  // blocks to within 3% with alpha ~2100 deg/s^2.
  //
  // The linear term is kept because it costs nothing and lets the model be
  // disabled (both zero) or degraded to the old behaviour if the quadratic fit
  // ever stops holding.
  //
  //   predicted coast = leadSeconds*|w| + w^2/(2*alpha)
  //
  // Coast is the largest error on this robot by a wide margin, and it is a
  // control problem rather than a sensing one.
  void setLeadSeconds(float seconds);
  float leadSeconds() const;

  // `alpha` in deg/s^2; zero disables the quadratic term.
  void setCoastAlpha(float alphaDegPerSecSquared);
  float coastAlpha() const;

  // The angle the turn is predicted to coast through if the motors were cut
  // right now, from the most recent rate. Reported so the compensation stays
  // visible rather than silently folded into the stop decision.
  float predictedCoastDegrees() const;

  // Yaw rate used for the lead prediction: a short moving average of the most
  // recent samples, not a single reading. A single sample carries the full gyro
  // noise, and at 285 deg/s a noise spike of a few dps moves the stop decision
  // by a fraction of a degree — small, but free to remove.
  float smoothedRateDps() const;

  // Yaw rate of the most recent raw sample, deg/s and signed.
  float lastRateDps() const;

  bool active() const;
  bool reachedTarget() const;
  bool timedOut(uint32_t nowMs) const;

  // Signed, so the caller can see which way it actually went.
  float turnedDegrees() const;
  // Absolute progress toward the target.
  float progressDegrees() const;
  float targetDegrees() const;
  uint32_t sampleCount() const;

 private:
  static constexpr uint8_t kRateWindow = 4;

  Integrator integrator_;
  float target_;
  float leadSeconds_;
  float coastAlpha_;
  float lastDps_;
  float rateWindow_[kRateWindow];
  uint8_t rateWindowCount_;
  uint8_t rateWindowNext_;
  uint32_t startMs_;
  uint32_t timeoutMs_;
  bool active_;
};

// Distance along a leg, by double-integrating acceleration between two stops.
//
// WHY THIS IS NOT HOPELESS, AND WHY IT IS ALSO NOT GREAT.
// Position error from a constant acceleration error is 0.5*a*t^2, so raw
// integration is catastrophic: this part's spec offset tolerance is +/-100 mg,
// which over a 5 s leg is 12 METRES of error on a 1 m leg. Worse, a tilt error
// leaks g*sin(theta) into horizontal acceleration and is fundamentally
// indistinguishable from real acceleration — 1 degree of tilt error is 2.1 m.
//
// What rescues it here is that the robot STOPS at both ends of every leg. The
// end-of-leg constraint v = 0 is known, so whatever velocity was integrated is
// pure accumulated error, and removing it linearly also removes the average
// acceleration error that produced it. That leaves bias INSTABILITY and tilt
// DRIFT rather than bias and tilt, which is roughly 10-30 cm on a 1 m leg.
//
// That is no better than time x calibrated speed. The reason to have it anyway
// is that it is INDEPENDENT: time x speed cannot see wheel slip, because
// spinning wheels and rolling wheels take the same time. This can. The
// disagreement between the two estimates is the slip measurement.
//
// Gravity is removed by subtracting the resting acceleration vector sampled
// immediately before the leg, which cancels static tilt and static bias
// together — they are not separable anyway.
class DisplacementEstimator {
 public:
  DisplacementEstimator();

  // `restX/Y/Z` is the accelerometer reading at standstill, in g. It carries
  // gravity AND bias, and subtracting it removes both.
  void begin(float restX, float restY, float restZ);

  // `ax/ay/az` in g, `nowUs` a free-running microsecond clock.
  void addSample(float ax, float ay, float az, uint32_t nowUs);

  // Distance before the zero-velocity correction. Reported so the size of the
  // correction stays visible rather than silently folded away.
  float rawDistanceMetres() const;

  // Distance after distributing the end-of-leg velocity error backwards.
  // Call once the robot is stopped.
  float correctedDistanceMetres() const;

  // Velocity left over at the end of the leg. The robot is stopped, so this is
  // pure error, and its size is the honest confidence measure for the result.
  float residualSpeedMetresPerSec() const;

  uint32_t sampleCount() const;
  float elapsedSeconds() const;

 private:
  float rest_[3];
  float velocity_[3];
  float position_[3];
  uint32_t lastUs_;
  uint32_t count_;
  float elapsed_;
  bool started_;
};
// Holds a straight course by trimming the differential against the gyro.
//
// WHY THIS IS NEEDED. Two continuous-rotation servos never match, and this
// chassis has no wheel encoders, so an open-loop "both wheels forward" curves
// by an amount nobody can predict. The gyro CAN see the resulting heading
// change, so it is the only available feedback for driving straight.
//
// WHAT IT CANNOT DO. It corrects heading, not position. A robot that is
// perfectly parallel to its intended course but displaced sideways looks
// identical to one on course, because integrating yaw says nothing about
// lateral translation. Distance travelled is likewise unobservable — it is
// estimated from time and a calibrated speed, and wheel slip corrupts that
// silently. Any survey built on this inherits those limits.
class HeadingHold {
 public:
  HeadingHold();

  // `gain` is command units per degree of error. `maxTrim` bounds the
  // differential so a large error cannot stall one wheel or reverse it, which
  // would turn a course correction into a pivot.
  void begin(float gain, int8_t maxTrim);

  // Signed trim to ADD to the left wheel and SUBTRACT from the right, given
  // how far off the intended heading the robot currently is.
  int8_t trimFor(float headingErrorDegrees) const;

  float gain() const;
  int8_t maxTrim() const;

 private:
  float gain_;
  int8_t maxTrim_;
};
// roughly linear in time near the operating point, which is why this is an
// iterative correction and not a one-shot formula — any residue is measured
// again on the next run.
//
// `clamped` matters: when the ideal value falls outside the calibratable
// range, the returned duration is pinned to the limit. Pinning twice in a row
// yields an identical value, which reads exactly like convergence while
// meaning the opposite — observed on hardware reporting "converged at 2000 ms"
// for a chassis that never completed a turn. Callers must tell them apart.
struct Correction {
  uint16_t ms;   // 0 when the measurement cannot justify a correction
  bool clamped;  // the real answer lies outside kSpinMinMs/kSpinMaxMs
};

Correction correctTurn(uint16_t testedMs, float measuredDegrees);

// Convenience wrapper returning only the corrected duration.
uint16_t correctedTurnMs(uint16_t testedMs, float measuredDegrees);

}  // namespace TurnMeasure
