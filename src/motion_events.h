#pragma once

// ============================================================================
// motion_events.h — bump, shake and pick-up detection from the accelerometer.
//
// All three are the same sensor. What separates them is SIGNATURE, not
// magnitude, and getting that distinction right is the whole design:
//
//   BUMP    one sharp spike, ~20-100 ms, while the robot is driving.
//           Hitting a wall at 20 cm/s and stopping in 50 ms is 0.41 g.
//           Servo vibration is under 0.05 g, so 0.25 g separates them with
//           about 4x margin.
//
//   SHAKE   sustained ALTERNATION. Hand shaking runs 3-8 Hz, which is 8-16
//           sign changes per second; a bump gives one or two. Detection keys
//           on the alternation rate rather than amplitude, which matters
//           because a vigorous shake reaches 2-4 g and the accelerometer is
//           configured for +/-2 g, so it saturates. A clipped waveform still
//           alternates at the same rate, and we only need to know THAT it is
//           being shaken.
//
//   LIFTED  the gravity vector changes and STAYS changed. A hand is not a
//           level table, so tilt almost always shifts; and the lift itself
//           takes 300-1000 ms against a bump's ~50 ms. DURATION is the primary
//           discriminator between a bump and a pick-up.
//
// PLACED matters as much as LIFTED: once the robot is set down, its gravity
// reference and yaw axis are stale, because it may be facing anywhere and
// sitting at a different tilt. Announcing that is what lets the rest of the
// firmware re-establish itself instead of silently integrating from a
// reference that no longer describes reality.
//
// Kept free of Arduino headers so every threshold and transition is unit-
// tested on the host rather than tuned by guesswork on hardware.
// ============================================================================

#include <stdint.h>

namespace MotionEvents {

enum class Event : uint8_t {
  None = 0,
  Bump,     // sharp impact
  Shake,    // sustained alternating motion
  Lifted,   // picked up off the surface
  Placed,   // set back down and settled
};

const char* eventName(Event event);

// Impact threshold in g, above the servo-vibration floor by ~4x.
constexpr float kBumpG = 0.25f;
// Alternations within the window that mean "being shaken" rather than knocked.
constexpr uint8_t kShakeAlternations = 6;
constexpr uint32_t kShakeWindowUs = 1000000;
// Motion this large counts as an alternation candidate at all.
constexpr float kShakeG = 0.30f;
// Tilt change that indicates the robot is no longer flat on its surface.
constexpr float kLiftTiltDegrees = 15.0f;
// Sustained departure from 1 g total.
constexpr float kLiftMagnitudeG = 0.15f;
// How long a disturbance must persist to be a lift rather than a bump.
constexpr uint32_t kLiftHoldUs = 300000;
// How long it must be quiet again to count as placed.
constexpr uint32_t kPlacedQuietUs = 600000;
// After firing, ignore the same event for this long so one impact does not
// report as a burst.
constexpr uint32_t kEventCooldownUs = 300000;

class Detector {
 public:
  Detector();

  // `restX/Y/Z` in g, sampled at standstill. Carries gravity AND sensor bias;
  // subtracting it removes both, which is all that is needed here.
  void begin(float restX, float restY, float restZ);

  // Feed every accelerometer sample. `driveActive` tells the detector whether
  // the robot is commanding motion, which is what makes a spike a BUMP rather
  // than an unexplained knock.
  //
  // Returns the event detected on this sample, or None.
  Event addSample(float ax, float ay, float az, uint32_t nowUs,
                  bool driveActive);

  // True between Lifted and Placed.
  bool lifted() const;

  // Diagnostics — reported so a misfiring threshold can be seen rather than
  // guessed at.
  float lastTiltDegrees() const;
  float lastMagnitudeDeviationG() const;
  float lastLinearG() const;
  uint8_t alternationsInWindow() const;

  // Counts since begin(), for a quick "is it detecting anything" check.
  uint16_t bumpCount() const;
  uint16_t shakeCount() const;
  uint16_t liftCount() const;

 private:
  void pushAlternation(uint32_t nowUs);
  uint8_t countAlternations(uint32_t nowUs) const;

  static constexpr uint8_t kAlternationRing = 16;

  float rest_[3];
  float restMagnitude_;
  bool started_;

  int8_t lastSign_;
  uint32_t alternationsUs_[kAlternationRing];
  uint8_t alternationNext_;
  uint8_t alternationCount_;

  // Disturbance tracking for the lift decision.
  bool disturbed_;
  uint32_t disturbedSinceUs_;
  bool lifted_;
  uint32_t quietSinceUs_;
  bool quiet_;

  uint32_t lastBumpUs_;
  uint32_t lastShakeUs_;
  bool haveFiredBump_;
  bool haveFiredShake_;

  float lastTilt_;
  float lastMagDev_;
  float lastLinear_;

  uint16_t bumpCount_;
  uint16_t shakeCount_;
  uint16_t liftCount_;
};

}  // namespace MotionEvents
