// Host tests for bump / shake / pick-up detection.
//
// All three come from the same sensor, so the tests that matter are the ones
// that prove they are told APART. A detector that fires "bump" on a shake, or
// "lifted" on an impact, is worse than no detector: it would drive the robot's
// reactions from a wrong story about what just happened.

#include "motion_events.h"

#include <cassert>
#include <cmath>
#include <cstdio>

namespace {

using MotionEvents::Detector;
using MotionEvents::Event;

constexpr float kRest[3] = {0.0f, 0.0f, 1.0f};
constexpr uint32_t kSampleUs = 2000;   // 500 Hz, the real IMU rate

// Feeds `ms` of perfectly still samples, returning any event seen.
Event feedStill(Detector& d, uint32_t& us, uint32_t ms, bool drive = false) {
  Event seen = Event::None;
  const uint32_t samples = (ms * 1000) / kSampleUs;
  for (uint32_t i = 0; i < samples; ++i) {
    const Event e = d.addSample(kRest[0], kRest[1], kRest[2], us, drive);
    if (e != Event::None && seen == Event::None) seen = e;
    us += kSampleUs;
  }
  return seen;
}

void testStillRobotReportsNothing() {
  // The most important negative: a robot sitting on a desk must never
  // spontaneously claim to have been bumped or picked up.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  assert(feedStill(d, us, 3000) == Event::None);
  assert(!d.lifted());
  assert(d.bumpCount() == 0);
  assert(d.shakeCount() == 0);
  assert(d.liftCount() == 0);
}

void testBumpWhileDriving() {
  // 20 cm/s into a wall, stopping in ~50 ms, is 0.41 g.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  feedStill(d, us, 500, /*drive=*/true);

  Event seen = Event::None;
  for (int i = 0; i < 25; ++i) {   // 50 ms of deceleration
    const Event e = d.addSample(-0.41f, 0.0f, 1.0f, us, /*drive=*/true);
    if (e != Event::None && seen == Event::None) seen = e;
    us += kSampleUs;
  }
  assert(seen == Event::Bump);
  assert(d.bumpCount() == 1);
  // A bump must NOT be mistaken for a pick-up.
  assert(!d.lifted());
  assert(d.liftCount() == 0);
}

void testStationaryKnockIsNotABump() {
  // Requiring driveActive is what makes a bump a COLLISION. A robot sitting
  // still that gets tapped did not drive into anything.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  feedStill(d, us, 500);

  Event seen = Event::None;
  for (int i = 0; i < 25; ++i) {
    const Event e = d.addSample(-0.41f, 0.0f, 1.0f, us, /*drive=*/false);
    if (e != Event::None && seen == Event::None) seen = e;
    us += kSampleUs;
  }
  assert(seen != Event::Bump);
  assert(d.bumpCount() == 0);
}

void testServoVibrationIsNotABump() {
  // The threshold exists to clear the vibration floor. Driving with the
  // servos running must not produce a stream of phantom collisions.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  for (int i = 0; i < 1000; ++i) {
    const float wobble = (i % 2) ? 0.04f : -0.04f;   // 0.04 g, well under 0.25
    assert(d.addSample(wobble, 0.0f, 1.0f + wobble, us, /*drive=*/true) ==
           Event::None);
    us += kSampleUs;
  }
  assert(d.bumpCount() == 0);
}

void testShakeIsDetectedAndNotReportedAsABump() {
  // 5 Hz hand shake. The discriminator is ALTERNATION, not amplitude, which
  // is why this works even though a real shake saturates the +/-2 g range.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  feedStill(d, us, 300);

  Event seen = Event::None;
  const float freq = 5.0f;
  for (int i = 0; i < 750; ++i) {   // 1.5 s
    const float t = static_cast<float>(i) * 0.002f;
    const float a = 1.5f * sinf(2.0f * 3.14159265f * freq * t);
    const Event e = d.addSample(a, 0.0f, 1.0f, us, /*drive=*/false);
    if (e == Event::Shake && seen == Event::None) seen = e;
    us += kSampleUs;
  }
  assert(seen == Event::Shake);
  assert(d.shakeCount() >= 1);
}

void testShakeSurvivesSaturation() {
  // A vigorous shake exceeds +/-2 g and clips. A clipped waveform still
  // alternates at the same rate, which is exactly why detection keys on
  // alternation rather than magnitude.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  feedStill(d, us, 300);

  Event seen = Event::None;
  for (int i = 0; i < 750; ++i) {
    const float t = static_cast<float>(i) * 0.002f;
    float a = 3.0f * sinf(2.0f * 3.14159265f * 5.0f * t);
    if (a > 2.0f) a = 2.0f;      // clip, as the hardware would
    if (a < -2.0f) a = -2.0f;
    const Event e = d.addSample(a, 0.0f, 1.0f, us, /*drive=*/false);
    if (e == Event::Shake && seen == Event::None) seen = e;
    us += kSampleUs;
  }
  assert(seen == Event::Shake);
}

void testSingleImpactIsNotAShake() {
  // One spike is a bump. Reporting it as a shake would be wrong about what
  // happened, and would suppress the collision response.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  feedStill(d, us, 500, /*drive=*/true);
  for (int i = 0; i < 25; ++i) {
    d.addSample(-0.5f, 0.0f, 1.0f, us, /*drive=*/true);
    us += kSampleUs;
  }
  feedStill(d, us, 1000, /*drive=*/true);
  assert(d.shakeCount() == 0);
  assert(d.bumpCount() >= 1);
}

void testLiftIsSustainedAndNotABump() {
  // A pick-up tilts the robot and KEEPS it tilted. Duration is the primary
  // discriminator: a bump is over in tens of milliseconds.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  feedStill(d, us, 500);

  // ~25 degrees of tilt, held.
  const float tx = 0.42f, tz = 0.91f;
  Event seen = Event::None;
  for (int i = 0; i < 400; ++i) {   // 800 ms
    const Event e = d.addSample(tx, 0.0f, tz, us, /*drive=*/false);
    if (e != Event::None && seen == Event::None) seen = e;
    us += kSampleUs;
  }
  assert(seen == Event::Lifted);
  assert(d.lifted());
  assert(d.liftCount() == 1);
  assert(d.lastTiltDegrees() > MotionEvents::kLiftTiltDegrees);
}

void testBriefTiltIsNotALift() {
  // A short disturbance - knocked, or driven over a cable - must not be
  // reported as being picked up.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  feedStill(d, us, 500);
  for (int i = 0; i < 50; ++i) {   // 100 ms only, under the 300 ms hold
    d.addSample(0.42f, 0.0f, 0.91f, us, /*drive=*/false);
    us += kSampleUs;
  }
  feedStill(d, us, 200);
  assert(!d.lifted());
  assert(d.liftCount() == 0);
}

void testPlacedFollowsLiftedOnceSettled() {
  // Placed matters as much as Lifted: after being set down the gravity
  // reference and yaw axis are stale, and something has to say so.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  feedStill(d, us, 300);
  for (int i = 0; i < 400; ++i) {
    d.addSample(0.42f, 0.0f, 0.91f, us, /*drive=*/false);
    us += kSampleUs;
  }
  assert(d.lifted());

  // Set down flat and left alone.
  const Event seen = feedStill(d, us, 1200);
  assert(seen == Event::Placed);
  assert(!d.lifted());
}

void testPlacedRequiresRealQuietNotAMomentaryPause() {
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  feedStill(d, us, 300);
  for (int i = 0; i < 400; ++i) {
    d.addSample(0.42f, 0.0f, 0.91f, us, /*drive=*/false);
    us += kSampleUs;
  }
  assert(d.lifted());
  // A brief still moment mid-carry must not read as being put down.
  assert(feedStill(d, us, 200) == Event::None);
  assert(d.lifted());
}

void testCooldownStopsOneImpactReportingAsABurst() {
  // Without this a single collision would fire dozens of times as the spike
  // rings out, and any reaction driven by it would stutter.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  feedStill(d, us, 500, /*drive=*/true);
  for (int i = 0; i < 100; ++i) {   // 200 ms of sustained hard deceleration
    d.addSample(-0.6f, 0.0f, 1.0f, us, /*drive=*/true);
    us += kSampleUs;
  }
  assert(d.bumpCount() == 1);
}

void testTiltReferenceIsWhateverBeginWasGiven() {
  // The robot's board is not axis-aligned - measured resting gravity is
  // about (0.98, 0.08, -0.26). Tilt must be relative to THAT, not to an
  // assumed flat orientation, or a healthy robot would look permanently
  // tipped over.
  Detector d;
  d.begin(0.98f, 0.08f, -0.26f);
  uint32_t us = 0;
  for (int i = 0; i < 500; ++i) {
    const Event e = d.addSample(0.98f, 0.08f, -0.26f, us, /*drive=*/false);
    assert(e == Event::None);
    us += kSampleUs;
  }
  assert(d.lastTiltDegrees() < 1.0f);
  assert(!d.lifted());
}

void testFreeFallIsDetectedAsALift() {
  // Dropping or snatching the robot produces a magnitude departure from 1 g
  // even with no tilt change at all.
  Detector d;
  d.begin(kRest[0], kRest[1], kRest[2]);
  uint32_t us = 0;
  feedStill(d, us, 300);
  Event seen = Event::None;
  for (int i = 0; i < 400; ++i) {
    // 0.6 g total: being accelerated upward-ish, no tilt.
    const Event e = d.addSample(0.0f, 0.0f, 0.6f, us, /*drive=*/false);
    if (e != Event::None && seen == Event::None) seen = e;
    us += kSampleUs;
  }
  assert(seen == Event::Lifted);
}

}  // namespace

int main() {
  testStillRobotReportsNothing();
  testBumpWhileDriving();
  testStationaryKnockIsNotABump();
  testServoVibrationIsNotABump();
  testShakeIsDetectedAndNotReportedAsABump();
  testShakeSurvivesSaturation();
  testSingleImpactIsNotAShake();
  testLiftIsSustainedAndNotABump();
  testBriefTiltIsNotALift();
  testPlacedFollowsLiftedOnceSettled();
  testPlacedRequiresRealQuietNotAMomentaryPause();
  testCooldownStopsOneImpactReportingAsABurst();
  testTiltReferenceIsWhateverBeginWasGiven();
  testFreeFallIsDetectedAsALift();
  printf("motion-events tests passed\n");
  return 0;
}
