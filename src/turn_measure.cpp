#include "turn_measure.h"

#include <math.h>

#include "movement_expression.h"

namespace TurnMeasure {
namespace {

float absf(float value) {
  return value < 0.0f ? -value : value;
}

}  // namespace

const char* axisName(Axis axis) {
  switch (axis) {
    case Axis::X:
      return "X";
    case Axis::Y:
      return "Y";
    case Axis::Z:
      return "Z";
  }
  return "?";
}

float componentFor(Axis axis, float x, float y, float z) {
  switch (axis) {
    case Axis::X:
      return x;
    case Axis::Y:
      return y;
    case Axis::Z:
      return z;
  }
  return 0.0f;
}

Axis yawAxisFromGravity(float ax, float ay, float az) {
  const float magX = absf(ax);
  const float magY = absf(ay);
  const float magZ = absf(az);
  if (magX >= magY && magX >= magZ) return Axis::X;
  if (magY >= magZ) return Axis::Y;
  return Axis::Z;
}

AxisEstimator::AxisEstimator()
    : sumX_(0.0), sumY_(0.0), sumZ_(0.0), count_(0) {}

void AxisEstimator::reset() {
  sumX_ = 0.0;
  sumY_ = 0.0;
  sumZ_ = 0.0;
  count_ = 0;
}

void AxisEstimator::addSample(float x, float y, float z) {
  sumX_ += static_cast<double>(x);
  sumY_ += static_cast<double>(y);
  sumZ_ += static_cast<double>(z);
  ++count_;
}

uint32_t AxisEstimator::sampleCount() const {
  return count_;
}

float AxisEstimator::meanMagnitude() const {
  if (count_ == 0) return 0.0f;
  const double n = static_cast<double>(count_);
  const double mx = sumX_ / n;
  const double my = sumY_ / n;
  const double mz = sumZ_ / n;
  return static_cast<float>(sqrt((mx * mx) + (my * my) + (mz * mz)));
}

bool AxisEstimator::ready() const {
  return count_ >= kMinSamples && meanMagnitude() >= kMinMeanDps;
}

bool AxisEstimator::axis(float& x, float& y, float& z) const {
  if (!ready()) return false;
  const double n = static_cast<double>(count_);
  const double mx = sumX_ / n;
  const double my = sumY_ / n;
  const double mz = sumZ_ / n;
  const double mag = sqrt((mx * mx) + (my * my) + (mz * mz));
  if (!(mag > 1e-6)) return false;
  x = static_cast<float>(mx / mag);
  y = static_cast<float>(my / mag);
  z = static_cast<float>(mz / mag);
  return true;
}

float AxisEstimator::angleToDegrees(float vx, float vy, float vz) const {
  float ax = 0.0f;
  float ay = 0.0f;
  float az = 0.0f;
  if (!axis(ax, ay, az)) return -1.0f;
  const float vmagSq = (vx * vx) + (vy * vy) + (vz * vz);
  if (!(vmagSq > 1e-12f)) return -1.0f;
  const float inv = 1.0f / sqrtf(vmagSq);
  // Sign-agnostic: the spin may run either way along the axis, and we only
  // care how far apart the two directions are.
  float dot = ((ax * vx) + (ay * vy) + (az * vz)) * inv;
  if (dot < 0.0f) dot = -dot;
  if (dot > 1.0f) dot = 1.0f;
  return static_cast<float>(acos(static_cast<double>(dot)) * 180.0 /
                            3.14159265358979323846);
}

float yawRateFromVector(float gyroX, float gyroY, float gyroZ,
                        float gravityX, float gravityY, float gravityZ) {
  const float magnitudeSq = (gravityX * gravityX) + (gravityY * gravityY) +
                            (gravityZ * gravityZ);
  // A zero-length gravity vector means the accelerometer read is unusable.
  // Returning 0 keeps a bad read from becoming an enormous phantom rate.
  if (!(magnitudeSq > 1e-6f)) return 0.0f;
  const float inverse = 1.0f / sqrtf(magnitudeSq);
  const float dot = (gyroX * gravityX) + (gyroY * gravityY) +
                    (gyroZ * gravityZ);
  return dot * inverse;
}

float singleAxisYawFraction(Axis axis, float gravityX, float gravityY,
                            float gravityZ) {
  const float magnitudeSq = (gravityX * gravityX) + (gravityY * gravityY) +
                            (gravityZ * gravityZ);
  if (!(magnitudeSq > 1e-6f)) return 0.0f;
  const float component = absf(componentFor(axis, gravityX, gravityY,
                                            gravityZ));
  return component / sqrtf(magnitudeSq);
}

bool isSaturated(int16_t raw) {
  return raw >= kSaturationRaw || raw <= -kSaturationRaw;
}

BiasEstimator::BiasEstimator()
    : sum_(0.0), count_(0), min_(0.0f), max_(0.0f) {}

void BiasEstimator::reset() {
  sum_ = 0.0;
  count_ = 0;
  min_ = 0.0f;
  max_ = 0.0f;
}

void BiasEstimator::addSample(float dps) {
  if (count_ == 0) {
    min_ = dps;
    max_ = dps;
  } else {
    if (dps < min_) min_ = dps;
    if (dps > max_) max_ = dps;
  }
  sum_ += static_cast<double>(dps);
  ++count_;
}

float BiasEstimator::spreadDps() const {
  if (count_ == 0) return 0.0f;
  return max_ - min_;
}

uint32_t BiasEstimator::sampleCount() const {
  return count_;
}

bool BiasEstimator::ready() const {
  return count_ >= kMinSamples;
}

float BiasEstimator::bias() const {
  if (count_ == 0) return 0.0f;
  return static_cast<float>(sum_ / static_cast<double>(count_));
}

Integrator::Integrator()
    : has_(false),
      saturated_(false),
      lastDps_(0.0f),
      lastUs_(0),
      firstUs_(0),
      worstGapUs_(0),
      count_(0),
      degrees_(0.0) {}

void Integrator::reset() {
  has_ = false;
  saturated_ = false;
  lastDps_ = 0.0f;
  lastUs_ = 0;
  firstUs_ = 0;
  worstGapUs_ = 0;
  count_ = 0;
  degrees_ = 0.0;
}

void Integrator::addSample(float dps, uint32_t nowUs) {
  if (!has_) {
    has_ = true;
    firstUs_ = nowUs;
    lastUs_ = nowUs;
    lastDps_ = dps;
    count_ = 1;
    return;
  }

  // Unsigned subtraction, so a micros() rollover mid-spin costs nothing.
  const uint32_t deltaUs = nowUs - lastUs_;
  if (deltaUs > worstGapUs_) worstGapUs_ = deltaUs;

  const double seconds = static_cast<double>(deltaUs) / 1000000.0;
  degrees_ += (static_cast<double>(lastDps_) + static_cast<double>(dps)) *
              0.5 * seconds;

  lastDps_ = dps;
  lastUs_ = nowUs;
  ++count_;
}

void Integrator::markSaturated() {
  saturated_ = true;
}

float Integrator::totalDegrees() const {
  return static_cast<float>(degrees_);
}

uint32_t Integrator::sampleCount() const {
  return count_;
}

uint32_t Integrator::spanUs() const {
  if (count_ < 2) return 0;
  return lastUs_ - firstUs_;
}

uint32_t Integrator::worstGapUs() const {
  return worstGapUs_;
}

bool Integrator::saturated() const {
  return saturated_;
}

float Integrator::meanIntervalUs() const {
  if (count_ < 2) return 0.0f;
  return static_cast<float>(spanUs()) / static_cast<float>(count_ - 1);
}

bool Integrator::usable() const {
  return !saturated_ && count_ >= kMinSamples && spanUs() > 0;
}

bool spanCoversCommandedSpin(uint32_t spanUs, uint16_t commandedMs) {
  if (commandedMs == 0) return false;
  const uint64_t requiredUs =
      (static_cast<uint64_t>(commandedMs) * 1000ULL * 9ULL) / 10ULL;
  return static_cast<uint64_t>(spanUs) >= requiredUs;
}

WindowedRate::WindowedRate()
    : has_(false),
      firstUs_(0),
      skipUs_(0),
      holdUs_(0),
      sum_(0.0),
      count_(0) {}

void WindowedRate::reset(uint32_t skipUs, uint32_t holdUs) {
  has_ = false;
  firstUs_ = 0;
  skipUs_ = skipUs;
  holdUs_ = holdUs;
  sum_ = 0.0;
  count_ = 0;
}

void WindowedRate::addSample(float dps, uint32_t nowUs) {
  if (!has_) {
    has_ = true;
    firstUs_ = nowUs;
  }
  const uint32_t sinceStart = nowUs - firstUs_;
  if (sinceStart < skipUs_) return;   // still accelerating
  if (sinceStart >= holdUs_) return;  // past the commanded spin
  sum_ += static_cast<double>(dps);
  ++count_;
}

bool WindowedRate::ready() const {
  return count_ >= kMinSamples;
}

float WindowedRate::meanDps() const {
  if (count_ == 0) return 0.0f;
  return static_cast<float>(sum_ / static_cast<double>(count_));
}

uint32_t WindowedRate::sampleCount() const {
  return count_;
}

RateModel fitRateModel(const RateSample* samples, uint8_t count) {
  RateModel model{0.0f, 0.0f, false};
  if (samples == nullptr || count < 2) return model;

  double sumX = 0.0;
  double sumY = 0.0;
  double sumXX = 0.0;
  double sumXY = 0.0;
  for (uint8_t i = 0; i < count; ++i) {
    const double x = static_cast<double>(samples[i].speed);
    const double y = static_cast<double>(absf(samples[i].dps));
    sumX += x;
    sumY += y;
    sumXX += x * x;
    sumXY += x * y;
  }
  const double n = static_cast<double>(count);
  const double denominator = (n * sumXX) - (sumX * sumX);
  // Every probe at the same speed carries no slope information.
  if (denominator > -1e-6 && denominator < 1e-6) return model;

  const double slope = ((n * sumXY) - (sumX * sumY)) / denominator;
  // A non-positive slope means faster wheels did not turn the body faster,
  // which is not a chassis — it is a stuck, slipping or restrained robot.
  if (!(slope > 0.0)) return model;

  model.slope = static_cast<float>(slope);
  model.intercept = static_cast<float>((sumY - (slope * sumX)) / n);
  model.valid = true;
  return model;
}

float predictRateDps(const RateModel& model, int8_t speed) {
  if (!model.valid) return 0.0f;
  const float rate = (model.slope * static_cast<float>(speed)) +
                     model.intercept;
  return rate > 0.0f ? rate : 0.0f;
}

uint16_t predictDurationMs(const RateModel& model, int8_t speed, float degrees,
                           uint16_t rampMs) {
  if (!model.valid) return 0;
  if (!(degrees > 0.0f)) return 0;
  const float rate = predictRateDps(model, speed);
  if (!(rate > 1.0f)) return 0;

  const double steadyMs =
      (static_cast<double>(degrees) / static_cast<double>(rate)) * 1000.0;
  // Half the ramp is lost rotation, so give it back as time.
  const double total = steadyMs + (static_cast<double>(rampMs) / 2.0);
  if (total <= 0.0 || total > 65535.0) return 0;
  return static_cast<uint16_t>(total + 0.5);
}

constexpr float kGravityMetresPerSecSq = 9.80665f;

DisplacementEstimator::DisplacementEstimator()
    : lastUs_(0), count_(0), elapsed_(0.0f), started_(false) {
  for (int i = 0; i < 3; ++i) {
    rest_[i] = 0.0f;
    velocity_[i] = 0.0f;
    position_[i] = 0.0f;
  }
}

void DisplacementEstimator::begin(float restX, float restY, float restZ) {
  rest_[0] = restX;
  rest_[1] = restY;
  rest_[2] = restZ;
  for (int i = 0; i < 3; ++i) {
    velocity_[i] = 0.0f;
    position_[i] = 0.0f;
  }
  lastUs_ = 0;
  count_ = 0;
  elapsed_ = 0.0f;
  started_ = false;
}

void DisplacementEstimator::addSample(float ax, float ay, float az,
                                      uint32_t nowUs) {
  if (!started_) {
    lastUs_ = nowUs;
    started_ = true;
    ++count_;
    return;
  }
  const uint32_t deltaUs = nowUs - lastUs_;   // unsigned: wrap is harmless
  lastUs_ = nowUs;
  if (deltaUs == 0) return;
  const float dt = static_cast<float>(deltaUs) * 1e-6f;
  // Reject absurd gaps rather than integrating them: a stalled loop would
  // otherwise inject a large fictitious displacement.
  if (dt > 0.5f) return;

  elapsed_ += dt;
  ++count_;

  // Subtracting the resting vector removes gravity and static bias together.
  // They are not separable from a single stationary reading anyway, and for a
  // leg driven at constant tilt they enter identically.
  const float a[3] = {(ax - rest_[0]) * kGravityMetresPerSecSq,
                      (ay - rest_[1]) * kGravityMetresPerSecSq,
                      (az - rest_[2]) * kGravityMetresPerSecSq};

  // Trapezoidal in position, rectangular in velocity: position is what is
  // being reported, so it gets the better rule.
  for (int i = 0; i < 3; ++i) {
    position_[i] += velocity_[i] * dt + 0.5f * a[i] * dt * dt;
    velocity_[i] += a[i] * dt;
  }
}

float DisplacementEstimator::rawDistanceMetres() const {
  return sqrtf(position_[0] * position_[0] + position_[1] * position_[1] +
               position_[2] * position_[2]);
}

float DisplacementEstimator::residualSpeedMetresPerSec() const {
  return sqrtf(velocity_[0] * velocity_[0] + velocity_[1] * velocity_[1] +
               velocity_[2] * velocity_[2]);
}

float DisplacementEstimator::correctedDistanceMetres() const {
  if (elapsed_ <= 0.0f) return 0.0f;
  // The robot is stopped, so the velocity still on the books is pure error.
  // Assume it accumulated linearly (i.e. from a constant acceleration error)
  // and remove the displacement that error contributed: integral of
  // v_err*(t/T) over [0,T] is v_err*T/2.
  //
  // This is what makes the whole thing viable: it also removes the average
  // acceleration error that produced the velocity error, so what remains is
  // bias INSTABILITY rather than bias.
  float corrected[3];
  for (int i = 0; i < 3; ++i) {
    corrected[i] = position_[i] - 0.5f * velocity_[i] * elapsed_;
  }
  return sqrtf(corrected[0] * corrected[0] + corrected[1] * corrected[1] +
               corrected[2] * corrected[2]);
}

uint32_t DisplacementEstimator::sampleCount() const { return count_; }
float DisplacementEstimator::elapsedSeconds() const { return elapsed_; }

HeadingHold::HeadingHold() : gain_(0.0f), maxTrim_(0) {}
void HeadingHold::begin(float gain, int8_t maxTrim) {
  gain_ = gain > 0.0f ? gain : 0.0f;
  maxTrim_ = maxTrim > 0 ? maxTrim : 0;
}

int8_t HeadingHold::trimFor(float headingErrorDegrees) const {
  if (gain_ <= 0.0f || maxTrim_ <= 0) return 0;
  float trim = -headingErrorDegrees * gain_;
  // Clamp hard. An unbounded trim would stall or reverse one wheel on a large
  // error, converting a course correction into a pivot - which loses the
  // straight-line assumption the distance estimate depends on.
  const float limit = static_cast<float>(maxTrim_);
  if (trim > limit) trim = limit;
  if (trim < -limit) trim = -limit;
  return static_cast<int8_t>(trim);
}

float HeadingHold::gain() const { return gain_; }
int8_t HeadingHold::maxTrim() const { return maxTrim_; }

TurnController::TurnController()
    : target_(0.0f),
      leadSeconds_(0.0f),
      coastAlpha_(0.0f),
      lastDps_(0.0f),
      rateWindowCount_(0),
      rateWindowNext_(0),
      startMs_(0),
      timeoutMs_(0),
      active_(false) {
  for (uint8_t i = 0; i < kRateWindow; ++i) rateWindow_[i] = 0.0f;
}

bool TurnController::begin(float targetDegrees, uint32_t timeoutMs,
                           uint32_t nowMs) {
  if (!(targetDegrees > 0.0f)) return false;   // also rejects NaN
  if (targetDegrees > kMaxUsableDegrees) return false;
  if (timeoutMs == 0) return false;            // an unbounded turn is not safe
  integrator_.reset();
  target_ = targetDegrees;
  lastDps_ = 0.0f;
  rateWindowCount_ = 0;
  rateWindowNext_ = 0;
  for (uint8_t i = 0; i < kRateWindow; ++i) rateWindow_[i] = 0.0f;
  startMs_ = nowMs;
  timeoutMs_ = timeoutMs;
  active_ = true;
  return true;
}

void TurnController::setLeadSeconds(float seconds) {
  // Negative lead would stop LATE, compounding the overshoot it exists to fix.
  leadSeconds_ = seconds > 0.0f ? seconds : 0.0f;
}

float TurnController::leadSeconds() const {
  return leadSeconds_;
}

void TurnController::setCoastAlpha(float alphaDegPerSecSquared) {
  // Alpha is a deceleration, so it must be positive to mean anything; a
  // negative or zero value simply disables the quadratic term rather than
  // producing a nonsensical negative or infinite coast.
  coastAlpha_ =
      alphaDegPerSecSquared > 0.0f ? alphaDegPerSecSquared : 0.0f;
}

float TurnController::coastAlpha() const {
  return coastAlpha_;
}

float TurnController::smoothedRateDps() const {
  if (rateWindowCount_ == 0) return 0.0f;
  float sum = 0.0f;
  for (uint8_t i = 0; i < rateWindowCount_; ++i) sum += rateWindow_[i];
  return sum / static_cast<float>(rateWindowCount_);
}

float TurnController::predictedCoastDegrees() const {
  const float rate = absf(smoothedRateDps());
  float predicted = rate * leadSeconds_;
  if (coastAlpha_ > 0.0f) {
    predicted += (rate * rate) / (2.0f * coastAlpha_);
  }
  return predicted;
}

float TurnController::lastRateDps() const {
  return lastDps_;
}

void TurnController::stop() {
  active_ = false;
}

void TurnController::addSample(float dps, uint32_t nowUs) {
  if (!active_) return;
  lastDps_ = dps;
  rateWindow_[rateWindowNext_] = dps;
  rateWindowNext_ = static_cast<uint8_t>((rateWindowNext_ + 1) % kRateWindow);
  if (rateWindowCount_ < kRateWindow) ++rateWindowCount_;
  integrator_.addSample(dps, nowUs);
}

bool TurnController::active() const {
  return active_;
}

bool TurnController::reachedTarget() const {
  // Stop early by however far the chassis is predicted to coast, so the angle
  // it comes to REST at is the target — rather than the angle at which the
  // motors were cut.
  return (progressDegrees() + predictedCoastDegrees()) >= target_;
}

bool TurnController::timedOut(uint32_t nowMs) const {
  if (!active_) return false;
  return (nowMs - startMs_) >= timeoutMs_;
}

float TurnController::turnedDegrees() const {
  return integrator_.totalDegrees();
}

float TurnController::progressDegrees() const {
  return absf(integrator_.totalDegrees());
}

float TurnController::targetDegrees() const {
  return target_;
}

uint32_t TurnController::sampleCount() const {
  return integrator_.sampleCount();
}

Correction correctTurn(uint16_t testedMs, float measuredDegrees) {
  Correction result{0, false};
  if (!MovementExpression::isValidFullTurnMs(testedMs)) return result;

  const float magnitude = absf(measuredDegrees);
  if (!(magnitude >= kMinUsableDegrees)) return result;  // also rejects NaN
  if (magnitude > kMaxUsableDegrees) return result;

  const double ideal =
      static_cast<double>(testedMs) * 360.0 / static_cast<double>(magnitude);

  // Snap to the tick grid before clamping, so the clamp cannot be undone by
  // rounding back outside the range.
  const double quantum =
      static_cast<double>(MovementExpression::kStepQuantumMs);
  double snapped = floor((ideal / quantum) + 0.5) * quantum;

  if (snapped < static_cast<double>(MovementExpression::kSpinMinMs)) {
    snapped = static_cast<double>(MovementExpression::kSpinMinMs);
    result.clamped = true;
  }
  if (snapped > static_cast<double>(MovementExpression::kSpinMaxMs)) {
    snapped = static_cast<double>(MovementExpression::kSpinMaxMs);
    result.clamped = true;
  }

  const uint16_t value = static_cast<uint16_t>(snapped);
  if (!MovementExpression::isValidFullTurnMs(value)) return result;
  result.ms = value;
  return result;
}

uint16_t correctedTurnMs(uint16_t testedMs, float measuredDegrees) {
  return correctTurn(testedMs, measuredDegrees).ms;
}

}  // namespace TurnMeasure
