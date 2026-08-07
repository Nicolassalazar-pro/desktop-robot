#include "motion_events.h"

#include <math.h>

namespace MotionEvents {
namespace {

constexpr float kRadToDeg = 57.2957795f;

float absf(float value) { return value < 0.0f ? -value : value; }

float magnitude3(float x, float y, float z) {
  return sqrtf(x * x + y * y + z * z);
}

}  // namespace

const char* eventName(Event event) {
  switch (event) {
    case Event::Bump: return "bump";
    case Event::Shake: return "shake";
    case Event::Lifted: return "lifted";
    case Event::Placed: return "placed";
    case Event::None:
    default: return "none";
  }
}

Detector::Detector() {
  begin(0.0f, 0.0f, 1.0f);
  bumpCount_ = 0;
  shakeCount_ = 0;
  liftCount_ = 0;
}

void Detector::begin(float restX, float restY, float restZ) {
  rest_[0] = restX;
  rest_[1] = restY;
  rest_[2] = restZ;
  restMagnitude_ = magnitude3(restX, restY, restZ);
  started_ = false;

  lastSign_ = 0;
  alternationNext_ = 0;
  alternationCount_ = 0;
  for (uint8_t i = 0; i < kAlternationRing; ++i) alternationsUs_[i] = 0;

  disturbed_ = false;
  disturbedSinceUs_ = 0;
  lifted_ = false;
  quietSinceUs_ = 0;
  quiet_ = false;

  lastBumpUs_ = 0;
  lastShakeUs_ = 0;
  haveFiredBump_ = false;
  haveFiredShake_ = false;

  lastTilt_ = 0.0f;
  lastMagDev_ = 0.0f;
  lastLinear_ = 0.0f;
}

void Detector::pushAlternation(uint32_t nowUs) {
  alternationsUs_[alternationNext_] = nowUs;
  alternationNext_ =
      static_cast<uint8_t>((alternationNext_ + 1) % kAlternationRing);
  if (alternationCount_ < kAlternationRing) ++alternationCount_;
}

uint8_t Detector::countAlternations(uint32_t nowUs) const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < alternationCount_; ++i) {
    // Unsigned subtraction, so clock wraparound is harmless.
    if (static_cast<uint32_t>(nowUs - alternationsUs_[i]) <= kShakeWindowUs) {
      ++count;
    }
  }
  return count;
}

Event Detector::addSample(float ax, float ay, float az, uint32_t nowUs,
                          bool driveActive) {
  if (!started_) {
    started_ = true;
    quietSinceUs_ = nowUs;
    quiet_ = true;
  }

  const float magnitude = magnitude3(ax, ay, az);
  lastMagDev_ = magnitude - 1.0f;

  // Linear acceleration: whatever is left once the resting vector is removed.
  // That subtraction cancels gravity and sensor bias together, which is all
  // that matters here — they are not separable from one stationary reading.
  const float lx = ax - rest_[0];
  const float ly = ay - rest_[1];
  const float lz = az - rest_[2];
  const float linear = magnitude3(lx, ly, lz);
  lastLinear_ = linear;

  // Tilt: the angle between where gravity is now and where it was at rest.
  // Guarded against the degenerate case of a zero-length reference, and the
  // dot product is clamped because floating point can push it just outside
  // [-1, 1] and make acosf return NaN.
  if (restMagnitude_ > 0.01f && magnitude > 0.01f) {
    float cosine = (ax * rest_[0] + ay * rest_[1] + az * rest_[2]) /
                   (magnitude * restMagnitude_);
    if (cosine > 1.0f) cosine = 1.0f;
    if (cosine < -1.0f) cosine = -1.0f;
    lastTilt_ = acosf(cosine) * kRadToDeg;
  } else {
    lastTilt_ = 0.0f;
  }

  // --- Alternation tracking, which is what identifies a shake -------------
  // Sign of the dominant linear axis. Only counted when the motion is big
  // enough to be deliberate, so sensor noise around zero cannot manufacture
  // alternations.
  if (linear > kShakeG) {
    float dominant = lx;
    if (absf(ly) > absf(dominant)) dominant = ly;
    if (absf(lz) > absf(dominant)) dominant = lz;
    const int8_t sign = dominant >= 0.0f ? 1 : -1;
    if (lastSign_ != 0 && sign != lastSign_) {
      pushAlternation(nowUs);
    }
    lastSign_ = sign;
  }
  const uint8_t alternations = countAlternations(nowUs);

  // --- Disturbance tracking, which is what identifies a lift --------------
  const bool disturbedNow = (lastTilt_ > kLiftTiltDegrees) ||
                            (absf(lastMagDev_) > kLiftMagnitudeG) ||
                            (linear > kLiftMagnitudeG);
  if (disturbedNow) {
    if (!disturbed_) {
      disturbed_ = true;
      disturbedSinceUs_ = nowUs;
    }
    quiet_ = false;
  } else {
    disturbed_ = false;
    if (!quiet_) {
      quiet_ = true;
      quietSinceUs_ = nowUs;
    }
  }

  // --- Decide, most specific first ---------------------------------------

  // SHAKE outranks BUMP: a shake contains many spikes, and reporting the
  // first of them as a bump would be wrong about what is happening.
  if (alternations >= kShakeAlternations) {
    const bool cooled =
        !haveFiredShake_ ||
        static_cast<uint32_t>(nowUs - lastShakeUs_) > kEventCooldownUs;
    if (cooled) {
      lastShakeUs_ = nowUs;
      haveFiredShake_ = true;
      ++shakeCount_;
      return Event::Shake;
    }
  }

  // PLACED: was lifted, and has now been undisturbed long enough to trust.
  if (lifted_ && quiet_ &&
      static_cast<uint32_t>(nowUs - quietSinceUs_) >= kPlacedQuietUs) {
    lifted_ = false;
    return Event::Placed;
  }

  // LIFTED: a disturbance that PERSISTS. The hold time is what separates this
  // from a bump, which is over in tens of milliseconds.
  if (!lifted_ && disturbed_ &&
      static_cast<uint32_t>(nowUs - disturbedSinceUs_) >= kLiftHoldUs) {
    lifted_ = true;
    ++liftCount_;
    return Event::Lifted;
  }

  // BUMP: a sharp spike while the robot is under power. Requiring
  // driveActive is what makes it a collision rather than an unexplained
  // knock — a stationary robot being tapped is not something it drove into.
  if (driveActive && !lifted_ && linear > kBumpG) {
    const bool cooled =
        !haveFiredBump_ ||
        static_cast<uint32_t>(nowUs - lastBumpUs_) > kEventCooldownUs;
    if (cooled) {
      lastBumpUs_ = nowUs;
      haveFiredBump_ = true;
      ++bumpCount_;
      return Event::Bump;
    }
  }

  return Event::None;
}

bool Detector::lifted() const { return lifted_; }
float Detector::lastTiltDegrees() const { return lastTilt_; }
float Detector::lastMagnitudeDeviationG() const { return lastMagDev_; }
float Detector::lastLinearG() const { return lastLinear_; }
uint8_t Detector::alternationsInWindow() const { return alternationCount_; }
uint16_t Detector::bumpCount() const { return bumpCount_; }
uint16_t Detector::shakeCount() const { return shakeCount_; }
uint16_t Detector::liftCount() const { return liftCount_; }

}  // namespace MotionEvents
