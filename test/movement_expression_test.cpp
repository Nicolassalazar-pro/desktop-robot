// Host tests for the emotional movement vocabulary.

#include "movement_expression.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace MovementExpression;

namespace {

const char* kMoodNames[] = {"frustrated", "excited", "scared"};

void testEveryPatternValidates() {
  for (const char* name : kMoodNames) {
    const Pattern* pattern = patternForName(name);
    assert(pattern != nullptr);
    assert(validate(*pattern));
  }
}

void testPatternsReturnToStart() {
  // THE core safety and design property: a gesture must not leave the robot
  // somewhere else. Emotion lives in the quality of the motion, not in
  // displacement, so this costs nothing expressively and keeps the robot off
  // the desk edge.
  for (const char* name : kMoodNames) {
    const Pattern* pattern = patternForName(name);
    long left = 0;
    long right = 0;
    netDisplacement(*pattern, left, right);
    assert(left == 0);
    assert(right == 0);
  }
}

void testStepsRespectTheHardwareLease() {
  // No single step may outlast the 300 ms drive lease, or the independent
  // safety timer would cut the rail mid-gesture.
  for (const char* name : kMoodNames) {
    const Pattern* pattern = patternForName(name);
    for (uint8_t i = 0; i < pattern->stepCount; ++i) {
      assert(pattern->steps[i].durationMs > 0);
      assert(pattern->steps[i].durationMs <= kMaxStepMs);
      assert(kMaxStepMs < 300);
    }
  }
}

void testCommandsStayInRange() {
  for (const char* name : kMoodNames) {
    const Pattern* pattern = patternForName(name);
    for (uint8_t i = 0; i < pattern->stepCount; ++i) {
      assert(pattern->steps[i].left >= kCommandMin);
      assert(pattern->steps[i].left <= kCommandMax);
      assert(pattern->steps[i].right >= kCommandMin);
      assert(pattern->steps[i].right <= kCommandMax);
    }
  }
}

void testPatternsAreBounded() {
  for (const char* name : kMoodNames) {
    const Pattern* pattern = patternForName(name);
    const uint32_t total = totalDurationMs(*pattern);
    assert(total > 0);
    assert(total <= kMaxPatternMs);
  }
}

void testEveryPatternEndsNeutral() {
  // A gesture that ends on a moving step would leave the wheels turning until
  // the idle power-down noticed.
  for (const char* name : kMoodNames) {
    const Pattern* pattern = patternForName(name);
    const Step& last = pattern->steps[pattern->stepCount - 1];
    assert(last.left == 0);
    assert(last.right == 0);
  }
}

void testMoodsAndNamesAgree() {
  assert(patternFor(Mood::Frustrated) == patternForName("frustrated"));
  assert(patternFor(Mood::Excited) == patternForName("excited"));
  assert(patternFor(Mood::Scared) == patternForName("scared"));
  assert(patternForName("happy") == nullptr);
  assert(patternForName(nullptr) == nullptr);
  assert(patternForName("") == nullptr);
}

void testSequencerWalksStepsInOrder() {
  const Pattern* pattern = patternForName("frustrated");
  Sequencer sequencer;
  assert(!sequencer.active());
  assert(sequencer.start(pattern, 1000));
  assert(sequencer.active());

  int left = 0;
  int right = 0;
  // First step.
  assert(sequencer.sample(1000, left, right));
  assert(left == pattern->steps[0].left);
  assert(right == pattern->steps[0].right);

  // Just before the first boundary.
  const uint32_t first = pattern->steps[0].durationMs;
  assert(sequencer.sample(1000 + first - 1, left, right));
  assert(left == pattern->steps[0].left);

  // Exactly on the boundary belongs to the second step.
  assert(sequencer.sample(1000 + first, left, right));
  assert(left == pattern->steps[1].left);
  assert(right == pattern->steps[1].right);
}

void testSequencerStopsAtTheEndAndReturnsNeutral() {
  const Pattern* pattern = patternForName("excited");
  Sequencer sequencer;
  sequencer.start(pattern, 0);
  const uint32_t total = totalDurationMs(*pattern);

  int left = -1;
  int right = -1;
  assert(!sequencer.sample(total, left, right));
  assert(left == 0);
  assert(right == 0);
  assert(!sequencer.active());

  // Sampling again after completion stays safe.
  left = -1;
  right = -1;
  assert(!sequencer.sample(total + 5000, left, right));
  assert(left == 0 && right == 0);
}

void testSequencerRejectsBadPatterns() {
  Sequencer sequencer;
  assert(!sequencer.start(nullptr, 0));
  assert(!sequencer.active());

  const Step travelling[] = {{100, 100, 100}};  // never returns to start
  const Pattern bad = {"bad", travelling, 1};
  assert(!validate(bad));
  assert(!sequencer.start(&bad, 0));
  assert(!sequencer.active());

  const Step tooLong[] = {{50, -50, 400}, {-50, 50, 400}};
  const Pattern longStep = {"long", tooLong, 2};
  assert(!validate(longStep));

  const Step outOfRange[] = {{127, -127, 100}, {-127, 127, 100}};
  const Pattern hot = {"hot", outOfRange, 2};
  assert(!validate(hot));

  const Pattern empty = {"empty", nullptr, 0};
  assert(!validate(empty));
}

void testStopIsIdempotentAndSafe() {
  Sequencer sequencer;
  sequencer.stop();
  assert(!sequencer.active());
  sequencer.start(patternForName("scared"), 0);
  sequencer.stop();
  sequencer.stop();
  assert(!sequencer.active());
  int left = -1;
  int right = -1;
  assert(!sequencer.sample(10, left, right));
  assert(left == 0 && right == 0);
}

void testSampledMotionIntegratesToZero() {
  // Walk each pattern at the firmware tick, and confirm the accumulated motion
  // cancels EXACTLY. This is the test that caught real drift: durations of 85
  // and 55 ms left a residue when sampled every 10 ms, so a pattern balanced
  // on paper would still have crept when actually played.
  for (const char* name : kMoodNames) {
    const Pattern* pattern = patternForName(name);
    Sequencer sequencer;
    sequencer.start(pattern, 0);
    long left = 0;
    long right = 0;
    int l = 0;
    int r = 0;
    for (uint32_t t = 0; t < totalDurationMs(*pattern);
         t += kStepQuantumMs) {
      if (!sequencer.sample(t, l, r)) break;
      left += static_cast<long>(l) * kStepQuantumMs;
      right += static_cast<long>(r) * kStepQuantumMs;
    }
    assert(left == 0);
    assert(right == 0);
  }
}

void testDurationsAreOnTheTickGrid() {
  for (const char* name : kMoodNames) {
    const Pattern* pattern = patternForName(name);
    for (uint8_t i = 0; i < pattern->stepCount; ++i) {
      assert(pattern->steps[i].durationMs % kStepQuantumMs == 0);
    }
  }
  // And an off-grid pattern must be rejected rather than silently drifting.
  const Step offGrid[] = {{50, -50, 85}, {-50, 50, 85}};
  const Pattern bad = {"offgrid", offGrid, 2};
  assert(!validate(bad));
}

void testMoodsAreDistinguishable() {
  // The three gestures must not be near-copies of one another. Compare the
  // shape of each: how much of the phrase is spent still, and the peak
  // amplitude used.
  auto stillFraction = [](const Pattern& pattern) {
    uint32_t still = 0;
    for (uint8_t i = 0; i < pattern.stepCount; ++i) {
      if (pattern.steps[i].left == 0 && pattern.steps[i].right == 0) {
        still += pattern.steps[i].durationMs;
      }
    }
    return static_cast<double>(still) / totalDurationMs(pattern);
  };
  auto peak = [](const Pattern& pattern) {
    int high = 0;
    for (uint8_t i = 0; i < pattern.stepCount; ++i) {
      const int magnitude = pattern.steps[i].left < 0
                                ? -pattern.steps[i].left
                                : pattern.steps[i].left;
      if (magnitude > high) high = magnitude;
    }
    return high;
  };

  const Pattern* frustrated = patternForName("frustrated");
  const Pattern* excited = patternForName("excited");
  const Pattern* scared = patternForName("scared");

  // Fear is carried by the freeze: startle then stillness. It must hold still
  // markedly more than the other two, which are continuous motion. This is the
  // real separator — a scared flinch is as fast and hard as an angry swing, so
  // peak amplitude does NOT distinguish them and must not be asserted as if it
  // did.
  assert(stillFraction(*scared) > 0.25);
  assert(stillFraction(*scared) > stillFraction(*frustrated) * 2);
  assert(stillFraction(*scared) > stillFraction(*excited) * 2);

  // Frustration is the one that never lets up: it is essentially all motion.
  assert(stillFraction(*frustrated) < 0.15);
  assert(peak(*frustrated) == kCommandMax);

  // Excited is the only one that translates rather than only spinning. Pure
  // counter-rotation would make it a faster frustrated.
  auto hasForwardMotion = [](const Pattern& pattern) {
    for (uint8_t i = 0; i < pattern.stepCount; ++i) {
      if (pattern.steps[i].left != 0 &&
          pattern.steps[i].left == pattern.steps[i].right) {
        return true;
      }
    }
    return false;
  };
  assert(hasForwardMotion(*excited));
  assert(!hasForwardMotion(*frustrated));
}

void testFrustratedBuilderProducesValidPatterns() {
  Step storage[kFrustratedStepCap];
  Pattern built{};
  for (uint16_t run = kFrustratedMinRunMs; run <= kFrustratedMaxRunMs;
       run += kStepQuantumMs) {
    // Every on-grid run length in range must be accepted; refusing some would
    // put holes in the tuning dial.
    assert(buildFrustrated(run, storage, kFrustratedStepCap, built));
    assert(validate(built));
    long left = 0;
    long right = 0;
    netDisplacement(built, left, right);
    assert(left == 0);
    assert(right == 0);
    // The requested swing is honoured exactly, however it was chunked.
    assert(totalDurationMs(built) ==
           static_cast<uint32_t>(run) * kFrustratedRuns + kFrustratedTailMs);
    assert(strcmp(built.name, "frustrated") == 0);
  }
}

void testFrustratedBuilderRejectsBadInput() {
  Step storage[kFrustratedStepCap];
  Pattern built{};
  assert(!buildFrustrated(kFrustratedDefaultRunMs, nullptr, kFrustratedStepCap,
                          built));
  assert(!buildFrustrated(0, storage, kFrustratedStepCap, built));
  assert(!buildFrustrated(kFrustratedMinRunMs - kStepQuantumMs, storage,
                          kFrustratedStepCap, built));
  assert(!buildFrustrated(kFrustratedMaxRunMs + kStepQuantumMs, storage,
                          kFrustratedStepCap, built));
  // Off-grid durations reintroduce sampling drift.
  assert(!buildFrustrated(245, storage, kFrustratedStepCap, built));
  // Too little storage must fail rather than overrun.
  assert(!buildFrustrated(kFrustratedMaxRunMs, storage, 3, built));
}

void testFrustratedBuilderSplitsLongRunsUnderTheCap() {
  Step storage[kFrustratedStepCap];
  Pattern built{};
  assert(buildFrustrated(kFrustratedMaxRunMs, storage, kFrustratedStepCap,
                         built));
  for (uint8_t i = 0; i < built.stepCount; ++i) {
    assert(built.steps[i].durationMs <= kMaxStepMs);
  }
  // A long swing must still be one continuous run, not a stutter: the
  // sub-steps of a run all carry the same command.
  assert(built.steps[0].left == built.steps[1].left);
  assert(built.steps[0].left == kCommandMax);
  assert(built.steps[0].right == kCommandMin);
}

void testFrustratedBuilderAlternatesDirection() {
  Step storage[kFrustratedStepCap];
  Pattern built{};
  assert(buildFrustrated(240, storage, kFrustratedStepCap, built));
  // Four runs plus the tail, each run one 240 ms step.
  assert(built.stepCount == kFrustratedRuns + 1);
  for (uint8_t i = 0; i + 1 < kFrustratedRuns; ++i) {
    assert(built.steps[i].left == -built.steps[i + 1].left);
  }
  assert(built.steps[kFrustratedRuns].left == 0);
  assert(built.steps[kFrustratedRuns].right == 0);
}

void testFallbackTableMatchesTheBuilderDefault() {
  // The static table and the builder are two descriptions of the same gesture.
  // Nothing stops them drifting apart except this test, and a silent drift
  // would mean the bench-tuned value and the shipped default disagree.
  Step storage[kFrustratedStepCap];
  Pattern built{};
  assert(buildFrustrated(kFrustratedDefaultRunMs, storage, kFrustratedStepCap,
                         built));
  const Pattern* table = patternForName("frustrated");
  assert(table != nullptr);
  assert(table->stepCount == built.stepCount);
  for (uint8_t i = 0; i < built.stepCount; ++i) {
    assert(table->steps[i].left == built.steps[i].left);
    assert(table->steps[i].right == built.steps[i].right);
    assert(table->steps[i].durationMs == built.steps[i].durationMs);
  }
}

void testExcitedOpensAndClosesWithAFullTurn() {
  const Pattern* excited = patternForName("excited");
  assert(excited != nullptr);

  // The opening and closing spins are written as split steps to respect
  // kMaxStepMs, so their total is what must equal a full turn. Without this
  // check, re-tuning kHalfTurnMs would leave the table behind and the "360"
  // would quietly stop being a 360.
  auto runLength = [&](uint8_t start) {
    uint32_t total = 0;
    const int8_t left = excited->steps[start].left;
    for (uint8_t i = start; i < excited->stepCount; ++i) {
      if (excited->steps[i].left != left) break;
      total += excited->steps[i].durationMs;
    }
    return total;
  };

  assert(runLength(0) == kFullTurnMs);
  assert(excited->steps[0].left == kCommandMax);

  // Find the closing spin: the last non-neutral run.
  uint8_t last = excited->stepCount - 1;
  while (last > 0 && excited->steps[last].left == 0) --last;
  const int8_t closingLeft = excited->steps[last].left;
  uint8_t start = last;
  while (start > 0 && excited->steps[start - 1].left == closingLeft) --start;
  assert(runLength(start) == kFullTurnMs);

  // Opposite directions, so the two turns cancel and the heading is preserved.
  assert(closingLeft == -excited->steps[0].left);
}

void testSpinBuilderProducesValidMeasurementSpins() {
  Step storage[kSpinStepCap];
  Pattern built{};
  for (uint16_t ms = kSpinMinMs; ms <= kSpinMaxMs; ms += kStepQuantumMs) {
    for (int direction = 0; direction < 2; ++direction) {
      const bool clockwise = direction == 0;
      assert(buildSpin(ms, clockwise, kSpinSpeed, storage, kSpinStepCap, built));
      // A measurement spin is deliberately unbalanced...
      assert(validate(built, Balance::Unbalanced));
      // ...and the net-zero guarantee must still reject it, or the opt-out
      // would have quietly become the default for everything.
      assert(!validate(built, Balance::NetZero));
      assert(!validate(built));

      assert(totalDurationMs(built) ==
             static_cast<uint32_t>(ms) + kSpinTailMs);
      for (uint8_t i = 0; i < built.stepCount; ++i) {
        assert(built.steps[i].durationMs <= kMaxStepMs);
        assert(built.steps[i].durationMs % kStepQuantumMs == 0);
      }
      // Pivots in place: the wheels always oppose, so a spin changes heading
      // without travelling.
      for (uint8_t i = 0; i + 1 < built.stepCount; ++i) {
        assert(built.steps[i].left == -built.steps[i].right);
      }
      // One continuous run in a single direction, then a commanded stop.
      assert(built.steps[0].left == (clockwise ? kSpinSpeed : -kSpinSpeed));
      for (uint8_t i = 1; i + 1 < built.stepCount; ++i) {
        assert(built.steps[i].left == built.steps[0].left);
      }
      const Step& last = built.steps[built.stepCount - 1];
      assert(last.left == 0 && last.right == 0);
    }
  }
}

void testSpinBuilderRejectsBadInput() {
  Step storage[kSpinStepCap];
  Pattern built{};
  assert(!buildSpin(kSpinDefaultMs, true, kSpinSpeed, nullptr, kSpinStepCap, built));
  assert(!buildSpin(0, true, kSpinSpeed, storage, kSpinStepCap, built));
  assert(!buildSpin(kSpinMinMs - kStepQuantumMs, true, kSpinSpeed, storage,
                    kSpinStepCap, built));
  assert(!buildSpin(kSpinPatternMaxMs + kStepQuantumMs, true, kSpinSpeed,
                    storage, kSpinStepCap, built));
  // A duration between the full-turn max and the pattern max is now VALID as a
  // pattern — that is the whole point of separating the two ranges, so a slow
  // turn can sweep the same angle over more time.
  assert(buildSpin(kSpinMaxMs + kStepQuantumMs, true, kSpinSpeed, storage,
                   kSpinStepCap, built));
  // Off-grid reintroduces sampling drift.
  assert(!buildSpin(605, true, kSpinSpeed, storage, kSpinStepCap, built));
  // Too little storage must fail rather than overrun.
  assert(!buildSpin(kSpinMaxMs, true, kSpinSpeed, storage, 2, built));
  // A speed inside the servo deadband is not a probe, it is a stall.
  assert(!buildSpin(600, true, kSpinMinSpeed - 1, storage, kSpinStepCap,
                    built));
  assert(!buildSpin(600, true, 0, storage, kSpinStepCap, built));
  assert(buildSpin(600, true, kSpinMinSpeed, storage, kSpinStepCap, built));
}

void testSpinBuilderHonoursTheRequestedSpeed() {
  // Characterising the chassis means probing at more than one wheel command,
  // so the speed has to actually reach the steps.
  Step storage[kSpinStepCap];
  Pattern built{};
  for (int8_t speed = kSpinMinSpeed; speed <= kCommandMax; speed += 7) {
    assert(buildSpin(800, true, speed, storage, kSpinStepCap, built));
    assert(built.steps[0].left == speed);
    assert(built.steps[0].right == -speed);
    assert(validate(built, Balance::Unbalanced));
    // Still a pure pivot at every speed: no translation to creep on.
    for (uint8_t i = 0; i + 1 < built.stepCount; ++i) {
      assert(built.steps[i].left == -built.steps[i].right);
    }
  }
}

void testExcitedBuilderProducesValidPatterns() {
  Step storage[kExcitedStepCap];
  Pattern built{};
  for (uint16_t turn = kSpinMinMs; turn <= kSpinMaxMs;
       turn += kStepQuantumMs) {
    assert(buildExcited(turn, storage, kExcitedStepCap, built));
    // Net-zero must survive ANY calibrated turn, because the whole point is
    // that this number changes once it is measured.
    assert(validate(built));
    long left = 0;
    long right = 0;
    netDisplacement(built, left, right);
    assert(left == 0);
    assert(right == 0);
    assert(strcmp(built.name, "excited") == 0);

    // The opening and closing turns must each last exactly the calibrated
    // duration, however they were chunked.
    auto runLength = [&](uint8_t start) {
      uint32_t total = 0;
      const int8_t left8 = built.steps[start].left;
      for (uint8_t i = start; i < built.stepCount; ++i) {
        if (built.steps[i].left != left8) break;
        total += built.steps[i].durationMs;
      }
      return total;
    };
    assert(runLength(0) == turn);
    assert(built.steps[0].left == kCommandMax);

    uint8_t last = built.stepCount - 1;
    while (last > 0 && built.steps[last].left == 0) --last;
    const int8_t closingLeft = built.steps[last].left;
    uint8_t start = last;
    while (start > 0 && built.steps[start - 1].left == closingLeft) --start;
    assert(runLength(start) == turn);
    assert(closingLeft == -built.steps[0].left);

    const Step& tail = built.steps[built.stepCount - 1];
    assert(tail.left == 0 && tail.right == 0);
  }
}

void testExcitedBuilderRejectsBadInput() {
  Step storage[kExcitedStepCap];
  Pattern built{};
  assert(!buildExcited(kSpinDefaultMs, nullptr, kExcitedStepCap, built));
  assert(!buildExcited(0, storage, kExcitedStepCap, built));
  assert(!buildExcited(kSpinMinMs - kStepQuantumMs, storage, kExcitedStepCap,
                       built));
  assert(!buildExcited(kSpinMaxMs + kStepQuantumMs, storage, kExcitedStepCap,
                       built));
  assert(!buildExcited(605, storage, kExcitedStepCap, built));
  assert(!buildExcited(kSpinMaxMs, storage, 10, built));
}

void testExcitedFitsAtTheLongestCalibratedTurn() {
  // The reason kMaxPatternMs was raised. If the measured turn comes back long
  // — and 600 ms under-rotating says it will — excited must still build rather
  // than failing validation at the moment it finally has a correct number.
  Step storage[kExcitedStepCap];
  Pattern built{};
  assert(buildExcited(kSpinMaxMs, storage, kExcitedStepCap, built));
  assert(totalDurationMs(built) <= kMaxPatternMs);
  assert(built.stepCount <= kExcitedStepCap);

  // And the declared storage cap must actually be sufficient at the worst
  // case, not merely generous at the default.
  Pattern exact{};
  assert(buildExcited(kSpinMaxMs, storage, built.stepCount, exact));
}

void testExcitedTableMatchesTheBuilderDefault() {
  // Same drift guard as frustrated: the static table and the builder are two
  // descriptions of one gesture, and only this test stops them disagreeing.
  Step storage[kExcitedStepCap];
  Pattern built{};
  assert(buildExcited(kFullTurnMs, storage, kExcitedStepCap, built));
  const Pattern* table = patternForName("excited");
  assert(table != nullptr);
  assert(table->stepCount == built.stepCount);
  for (uint8_t i = 0; i < built.stepCount; ++i) {
    assert(table->steps[i].left == built.steps[i].left);
    assert(table->steps[i].right == built.steps[i].right);
    assert(table->steps[i].durationMs == built.steps[i].durationMs);
  }
}

void testFullTurnIsNoLongerDerivedFromTheHalfTurn() {
  // The original bug: kFullTurnMs was kHalfTurnMs * 2, so a number nobody had
  // measured was presented as a 360. Both turns pay the same standing-start
  // acceleration, so doubling the half-turn under-rotates. This pins the two
  // apart so the derivation cannot quietly return.
  assert(kSpinDefaultMs == kFullTurnMs);
  assert(isValidFullTurnMs(kFullTurnMs));
  assert(isValidFullTurnMs(kSpinMinMs));
  assert(isValidFullTurnMs(kSpinMaxMs));
  assert(!isValidFullTurnMs(kSpinMinMs - kStepQuantumMs));
  assert(!isValidFullTurnMs(kSpinMaxMs + kStepQuantumMs));
  assert(!isValidFullTurnMs(605));
  // The calibratable range must actually straddle the default, or the dial
  // could not correct it in the direction the evidence points.
  assert(kSpinMinMs < kFullTurnMs);
  assert(kSpinMaxMs > kFullTurnMs);
}

void testExcitedBuildsAtTheMeasuredChassisValue() {
  // 1260 ms is the real measured full turn for this chassis (gyro-calibrated
  // untethered, 2026-08-05, saved to NVS). Pinning it means the value the
  // robot actually runs on is a tested case rather than merely inside a range,
  // and any future change to the step cap or the duration cap that would break
  // the shipped configuration fails here.
  constexpr uint16_t kMeasuredFullTurnMs = 1260;
  assert(isValidFullTurnMs(kMeasuredFullTurnMs));

  Step storage[kExcitedStepCap];
  Pattern built{};
  assert(buildExcited(kMeasuredFullTurnMs, storage, kExcitedStepCap, built));
  assert(validate(built));

  // 2 x 1260 turns + 1500 ms fixed middle + 120 ms tail.
  assert(totalDurationMs(built) == 4140);
  assert(totalDurationMs(built) <= kMaxPatternMs);

  long left = 0;
  long right = 0;
  netDisplacement(built, left, right);
  assert(left == 0 && right == 0);

  // The chunking lands on 5 full 250 ms steps plus a 10 ms remainder, which is
  // exactly one tick quantum — the tightest remainder the grid allows, and the
  // case most likely to expose an off-by-one.
  uint32_t opening = 0;
  const int8_t lead = built.steps[0].left;
  uint8_t i = 0;
  for (; i < built.stepCount && built.steps[i].left == lead; ++i) {
    opening += built.steps[i].durationMs;
  }
  assert(opening == kMeasuredFullTurnMs);
  assert(i == 6);
  assert(built.steps[5].durationMs == kStepQuantumMs);
}

void testSpinPatternRangeIsWiderThanTheFullTurnRange() {
  // Two different quantities: a calibrated full turn is "how long 360 takes at
  // full speed", while a spin pattern may run much longer because a slow turn
  // sweeps the same angle over more time. Conflating them would cap slow turns
  // at the calibration range.
  assert(kSpinPatternMaxMs > kSpinMaxMs);
  assert(isValidSpinDurationMs(kSpinMaxMs));
  assert(isValidSpinDurationMs(kSpinPatternMaxMs));
  assert(!isValidSpinDurationMs(kSpinPatternMaxMs + kStepQuantumMs));
  // A long spin must still fit inside the global pattern cap.
  assert(static_cast<uint32_t>(kSpinPatternMaxMs) + kSpinTailMs <=
         kMaxPatternMs);

  // A duration beyond the full-turn range is NOT a valid calibration value,
  // even though it is a valid pattern.
  assert(!isValidFullTurnMs(kSpinPatternMaxMs));

  Step storage[kSpinStepCap];
  Pattern built{};
  assert(buildSpin(kSpinPatternMaxMs, true, kSpinSpeed, storage, kSpinStepCap,
                   built));
  assert(validate(built, Balance::Unbalanced));
  assert(totalDurationMs(built) ==
         static_cast<uint32_t>(kSpinPatternMaxMs) + kSpinTailMs);
  for (uint8_t i = 0; i < built.stepCount; ++i) {
    assert(built.steps[i].durationMs <= kMaxStepMs);
  }
}

void testExcitedCapStillSizedFromTheFullTurnRange() {
  // excited's bookends come from the full-turn range, so widening the spin
  // pattern range must not inflate its storage.
  assert(kExcitedStepCap == (2 * kFullTurnMaxChunks) + kExcitedMiddleSteps + 1);
  Step storage[kExcitedStepCap];
  Pattern built{};
  assert(buildExcited(kSpinMaxMs, storage, kExcitedStepCap, built));
  assert(validate(built));
  assert(totalDurationMs(built) <= kMaxPatternMs);
}

void testStraightDrivesBothWheelsTheSameWay() {
  // The one pattern here that deliberately does NOT return the robot to where
  // it started - translation is the entire point, because it is the only
  // motion that produces RSSI information.
  Step storage[kStraightStepCap];
  Pattern pattern{};
  assert(buildStraight(2000, /*forward=*/true, 80, storage,
                       kStraightStepCap, pattern));
  assert(pattern.stepCount >= 2);
  for (uint8_t i = 0; i + 1 < pattern.stepCount; ++i) {
    // Equal commands mean translation on a differential drive.
    assert(pattern.steps[i].left == pattern.steps[i].right);
    assert(pattern.steps[i].left > 0);
  }
  // Ends commanded to a stop rather than leaving the last moving value latched.
  assert(pattern.steps[pattern.stepCount - 1].left == 0);
  assert(pattern.steps[pattern.stepCount - 1].right == 0);
  assert(totalDurationMs(pattern) >= 2000);
}

void testStraightReverses() {
  Step storage[kStraightStepCap];
  Pattern pattern{};
  assert(buildStraight(1000, /*forward=*/false, 60, storage,
                       kStraightStepCap, pattern));
  assert(pattern.steps[0].left < 0);
  assert(pattern.steps[0].left == pattern.steps[0].right);
}

void testStraightRejectsOutOfRange() {
  Step storage[kStraightStepCap];
  Pattern pattern{};
  assert(!buildStraight(0, true, 80, storage, kStraightStepCap, pattern));
  assert(!buildStraight(kStraightMaxMs + kStepQuantumMs, true, 80, storage,
                        kStraightStepCap, pattern));
  // Off the step grid.
  assert(!buildStraight(1005, true, 80, storage, kStraightStepCap, pattern));
  // Inside the servo deadband.
  assert(!buildStraight(1000, true, 1, storage, kStraightStepCap, pattern));
  // Not enough storage.
  assert(!buildStraight(8000, true, 80, storage, 2, pattern));
}

void testStraightIsNotNetZero() {
  // Recorded explicitly: every other pattern must return the robot to its
  // starting orientation, and this one must not, so nobody later "fixes" it.
  Step storage[kStraightStepCap];
  Pattern pattern{};
  assert(buildStraight(1000, true, 80, storage, kStraightStepCap, pattern));
  int32_t net = 0;
  for (uint8_t i = 0; i < pattern.stepCount; ++i) {
    net += static_cast<int32_t>(pattern.steps[i].left) *
           pattern.steps[i].durationMs;
  }
  assert(net != 0);
}

// The web app indexes patternNameAt() over the network, so a gap or an
// off-by-one here would let a remote index select the wrong movement — or,
// before this table was decoupled from the LCD button list, reach a slot that
// was not a movement at all.
void testPatternEnumerationMatchesTable() {
  const uint8_t count = patternCount();
  assert(count == sizeof(kMoodNames) / sizeof(kMoodNames[0]));
  for (uint8_t i = 0; i < count; ++i) {
    const char* name = patternNameAt(i);
    assert(name != nullptr);
    assert(strcmp(name, kMoodNames[i]) == 0);
    assert(patternForName(name) != nullptr);
  }
  assert(patternNameAt(count) == nullptr);
  assert(patternNameAt(255) == nullptr);
}

}  // namespace

int main() {
  testEveryPatternValidates();
  testPatternsReturnToStart();
  testStepsRespectTheHardwareLease();
  testCommandsStayInRange();
  testPatternsAreBounded();
  testEveryPatternEndsNeutral();
  testMoodsAndNamesAgree();
  testPatternEnumerationMatchesTable();
  testSequencerWalksStepsInOrder();
  testSequencerStopsAtTheEndAndReturnsNeutral();
  testSequencerRejectsBadPatterns();
  testStopIsIdempotentAndSafe();
  testSampledMotionIntegratesToZero();
  testDurationsAreOnTheTickGrid();
  testMoodsAreDistinguishable();
  testFrustratedBuilderProducesValidPatterns();
  testFrustratedBuilderRejectsBadInput();
  testFrustratedBuilderSplitsLongRunsUnderTheCap();
  testFrustratedBuilderAlternatesDirection();
  testFallbackTableMatchesTheBuilderDefault();
  testExcitedOpensAndClosesWithAFullTurn();
  testSpinBuilderProducesValidMeasurementSpins();
  testSpinBuilderRejectsBadInput();
  testSpinBuilderHonoursTheRequestedSpeed();
  testExcitedBuilderProducesValidPatterns();
  testExcitedBuilderRejectsBadInput();
  testExcitedFitsAtTheLongestCalibratedTurn();
  testExcitedTableMatchesTheBuilderDefault();
  testFullTurnIsNoLongerDerivedFromTheHalfTurn();
  testExcitedBuildsAtTheMeasuredChassisValue();
  testSpinPatternRangeIsWiderThanTheFullTurnRange();
  testExcitedCapStillSizedFromTheFullTurnRange();
  testStraightDrivesBothWheelsTheSameWay();
  testStraightReverses();
  testStraightRejectsOutOfRange();
  testStraightIsNotNetZero();
  printf("movement-expression tests passed\n");
  return 0;
}
