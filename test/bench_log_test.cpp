// Host tests for BenchLog.
//
// The log exists because an untethered bench run is the ONLY way to get honest
// movement numbers on this robot, and reconnecting the serial port resets the
// board — so results that are not persisted at the moment they are taken are
// destroyed by the act of going to read them. That makes persistence and
// validation load-bearing, not incidental, which is why they are tested here.

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "bench_log.h"
#include "nvs.h"

namespace {

using BenchLog::Metric;
using BenchLog::Store;

constexpr char kRunKey[] = "run";

// Flip a byte in the persisted blob so its checksum no longer matches.
void corruptStoredRun() {
  std::vector<uint8_t> bytes = FakeNvs::getBlob(kRunKey);
  bytes[8] = static_cast<uint8_t>(bytes[8] ^ 0xFF);
  FakeNvs::putBlob(kRunKey, bytes);
}

bool near(float a, float b, float tolerance) {
  return fabsf(a - b) <= tolerance;
}

void testEmptyStoreReportsNothing() {
  FakeNvs::reset();
  Store store;
  assert(store.count() == 0);
  assert(!store.overflowed());
  assert(!store.full());
  // Loading with nothing stored is not an error; it just leaves it empty.
  assert(!store.load());
  assert(store.count() == 0);
}

void testAddAndSummarise() {
  FakeNvs::reset();
  Store store;
  assert(store.add(Metric::CoastDegrees, 100, 8.0f));
  assert(store.add(Metric::CoastDegrees, 100, 9.0f));
  assert(store.add(Metric::CoastDegrees, 100, 7.0f));
  assert(store.count() == 3);

  float mean = 0.0f;
  float spread = 0.0f;
  uint16_t count = 0;
  assert(store.summarise(Metric::CoastDegrees, BenchLog::kCondNone, &mean,
                         &spread, &count));
  assert(near(mean, 8.0f, 1e-5f));
  assert(near(spread, 2.0f, 1e-5f));
  assert(count == 3);
}

void testSummariseReportsAbsenceRatherThanZero() {
  // A mean of zero and "no data" are completely different conclusions, and
  // conflating them would make an unrun phase look like a perfect result.
  FakeNvs::reset();
  Store store;
  store.add(Metric::CoastDegrees, 100, 8.0f);
  float mean = -1.0f;
  assert(!store.summarise(Metric::DriftDegrees, BenchLog::kCondNone, &mean,
                          nullptr, nullptr));
  assert(mean == -1.0f);   // untouched
}

void testConditionSplitsTheAbTest() {
  // The whole point of the lead A/B: the two conditions must not be averaged
  // together, or the comparison that justifies the feature disappears.
  FakeNvs::reset();
  Store store;
  store.add(Metric::TurnErrorDegrees, 180, 8.0f, BenchLog::kCondLeadOff);
  store.add(Metric::TurnErrorDegrees, 180, 8.4f, BenchLog::kCondLeadOff);
  store.add(Metric::TurnErrorDegrees, 180, 1.0f, BenchLog::kCondLeadOn);
  store.add(Metric::TurnErrorDegrees, 180, 1.4f, BenchLog::kCondLeadOn);

  float meanOff = 0.0f;
  float meanOn = 0.0f;
  uint16_t n = 0;
  assert(store.summarise(Metric::TurnErrorDegrees, BenchLog::kCondLeadOff,
                         &meanOff, nullptr, &n));
  assert(n == 2);
  assert(near(meanOff, 8.2f, 1e-4f));
  assert(store.summarise(Metric::TurnErrorDegrees, BenchLog::kCondLeadOn,
                         &meanOn, nullptr, &n));
  assert(n == 2);
  assert(near(meanOn, 1.2f, 1e-4f));

  // Asking for "any condition" still sees all four.
  assert(store.summarise(Metric::TurnErrorDegrees, BenchLog::kCondNone,
                         nullptr, nullptr, &n));
  assert(n == 4);
}

void testFullLogRefusesRatherThanOverwrites() {
  // Overwriting would silently discard the earliest entries - which are the
  // pose and drift readings that every later number has to be interpreted
  // against. Refusing loudly is strictly better.
  FakeNvs::reset();
  Store store;
  for (uint16_t i = 0; i < BenchLog::kMaxEntries; ++i) {
    assert(store.add(Metric::BiasDps, 0, static_cast<float>(i)));
  }
  assert(store.full());
  assert(!store.overflowed());

  assert(!store.add(Metric::BiasDps, 0, 999.0f));
  assert(store.overflowed());
  assert(store.count() == BenchLog::kMaxEntries);
  // The first entry must still be the first one written.
  assert(near(store.at(0).value, 0.0f, 1e-6f));
}

void testSaveAndLoadRoundTrip() {
  FakeNvs::reset();
  {
    Store store;
    store.add(Metric::GravityX, 0, 1.01f);
    store.add(Metric::GravityZ, 0, -0.10f);
    store.add(Metric::CoastDegrees, 100, 8.3f, BenchLog::kCondLeadOff);
    assert(store.save());
  }
  Store reloaded;
  assert(reloaded.load());
  assert(reloaded.count() == 3);
  assert(near(reloaded.at(0).value, 1.01f, 1e-5f));
  assert(reloaded.at(0).metric == static_cast<uint8_t>(Metric::GravityX));
  assert(near(reloaded.at(1).value, -0.10f, 1e-5f));
  assert(reloaded.at(2).param == 100);
  assert(reloaded.at(2).condition == BenchLog::kCondLeadOff);
}

void testOverflowFlagSurvivesPersistence() {
  // A truncated run must still announce that it was truncated after a reset,
  // or the missing phases look like phases that were never scheduled.
  FakeNvs::reset();
  {
    Store store;
    for (uint16_t i = 0; i < BenchLog::kMaxEntries + 4; ++i) {
      store.add(Metric::BiasDps, 0, 1.0f);
    }
    assert(store.overflowed());
    assert(store.save());
  }
  Store reloaded;
  assert(reloaded.load());
  assert(reloaded.overflowed());
}

void testCorruptRecordIsTreatedAsAbsent() {
  // A corrupt log must never turn into plausible-looking wrong measurements.
  FakeNvs::reset();
  {
    Store store;
    store.add(Metric::CoastDegrees, 100, 8.0f);
    assert(store.save());
  }
  corruptStoredRun();
  Store reloaded;
  assert(!reloaded.load());
  assert(reloaded.count() == 0);
}

void testEraseClearsBothRamAndNvs() {
  FakeNvs::reset();
  Store store;
  store.add(Metric::CoastDegrees, 100, 8.0f);
  assert(store.save());
  assert(store.erase());
  assert(store.count() == 0);

  Store reloaded;
  assert(!reloaded.load());
}

void testResetDoesNotTouchNvs() {
  // reset() is for starting a fresh run in RAM; the previous persisted run
  // must survive until it is explicitly overwritten or erased, so an aborted
  // start cannot destroy the last good results.
  FakeNvs::reset();
  Store store;
  store.add(Metric::CoastDegrees, 100, 8.0f);
  assert(store.save());
  store.reset();
  assert(store.count() == 0);

  Store reloaded;
  assert(reloaded.load());
  assert(reloaded.count() == 1);
}

void testMetricNamesAreDistinct() {
  // The dump is read by a human after the fact; two metrics sharing a name
  // would make the log ambiguous exactly when it cannot be re-taken.
  const Metric metrics[] = {
      Metric::GravityX, Metric::GravityY, Metric::GravityZ,
      Metric::ProjectionGain, Metric::DriftDegrees, Metric::BiasDps,
      Metric::NoiseSpreadDps, Metric::SteadyRateDps, Metric::CoastDegrees,
      Metric::CoastSeconds, Metric::TurnErrorDegrees,
      Metric::NetSequenceDegrees, Metric::VibrationSpreadDps};
  const int count = static_cast<int>(sizeof(metrics) / sizeof(metrics[0]));
  for (int i = 0; i < count; ++i) {
    const char* a = BenchLog::metricName(metrics[i]);
    assert(a != nullptr);
    assert(a[0] != '\0');
    for (int j = i + 1; j < count; ++j) {
      assert(strcmp(a, BenchLog::metricName(metrics[j])) != 0);
    }
  }
}

void testArmingSurvivesAndIsSingleShot() {
  // The run is armed while tethered, the robot is then unplugged, and the user
  // triggers it with the button. The flag therefore has to live in NVS, not
  // RAM - a power-cycle in between must not silently disarm it.
  FakeNvs::reset();
  assert(!BenchLog::isArmed());
  assert(BenchLog::setArmed(true));
  assert(BenchLog::isArmed());
  // Still armed on a fresh read, i.e. after a reset.
  assert(BenchLog::isArmed());
  assert(BenchLog::setArmed(false));
  assert(!BenchLog::isArmed());
}

void testGarbageByteDoesNotArmTheMotors() {
  // Arming drives the motors unattended, so anything other than the exact
  // magic must read as disarmed.
  FakeNvs::reset();
  FakeNvs::putBlob("armed", std::vector<uint8_t>{0x01});
  assert(!BenchLog::isArmed());
  FakeNvs::putBlob("armed", std::vector<uint8_t>{0xFF});
  assert(!BenchLog::isArmed());
}

void testPerSpeedSplitDoesNotPoolDifferentSpeeds() {
  // The first real run reported a pooled A/B that mixed a 16 deg coast with a
  // 4 deg one and understated the effect. Speeds must be separable.
  FakeNvs::reset();
  Store store;
  store.add(Metric::CoastDegrees, 100, 16.0f, BenchLog::kCondLeadOff);
  store.add(Metric::CoastDegrees, 100, 17.0f, BenchLog::kCondLeadOff);
  store.add(Metric::CoastDegrees,  55,  4.0f, BenchLog::kCondLeadOff);
  store.add(Metric::CoastDegrees,  55,  5.0f, BenchLog::kCondLeadOff);

  float mean = 0.0f;
  uint16_t n = 0;
  assert(store.summariseParam(Metric::CoastDegrees, BenchLog::kCondLeadOff, 100,
                              &mean, nullptr, &n));
  assert(n == 2);
  assert(near(mean, 16.5f, 1e-4f));

  assert(store.summariseParam(Metric::CoastDegrees, BenchLog::kCondLeadOff, 55,
                              &mean, nullptr, &n));
  assert(n == 2);
  assert(near(mean, 4.5f, 1e-4f));

  // Pooled would be 10.5 - a number describing neither speed.
  assert(store.summarise(Metric::CoastDegrees, BenchLog::kCondLeadOff, &mean,
                         nullptr, &n));
  assert(n == 4);
  assert(near(mean, 10.5f, 1e-4f));

  // A speed that was never run reports absence, not zero.
  assert(!store.summariseParam(Metric::CoastDegrees, BenchLog::kCondLeadOff, 75,
                               &mean, nullptr, &n));
}

void testLogHoldsAWholeRunWithoutTruncating() {
  // The first run truncated at 96 entries and lost the last two turns of the
  // A/B block - the most expensive data to lose, because retaking it costs
  // another supervised untethered session.
  FakeNvs::reset();
  Store store;
  const uint16_t baseline = 7;
  const uint16_t perTurn = 3;
  const uint16_t turns = 24;
  for (uint16_t i = 0; i < baseline; ++i) store.add(Metric::BiasDps, 0, 1.0f);
  for (uint16_t t = 0; t < turns; ++t) {
    for (uint16_t m = 0; m < perTurn; ++m) {
      assert(store.add(Metric::CoastDegrees, 100, 1.0f));
    }
  }
  assert(!store.overflowed());
  assert(store.count() == baseline + turns * perTurn);
}

}  // namespace

int main() {
  testEmptyStoreReportsNothing();
  testAddAndSummarise();
  testSummariseReportsAbsenceRatherThanZero();
  testConditionSplitsTheAbTest();
  testFullLogRefusesRatherThanOverwrites();
  testSaveAndLoadRoundTrip();
  testOverflowFlagSurvivesPersistence();
  testCorruptRecordIsTreatedAsAbsent();
  testEraseClearsBothRamAndNvs();
  testResetDoesNotTouchNvs();
  testMetricNamesAreDistinct();
  testArmingSurvivesAndIsSingleShot();
  testGarbageByteDoesNotArmTheMotors();
  testPerSpeedSplitDoesNotPoolDifferentSpeeds();
  testLogHoldsAWholeRunWithoutTruncating();
  printf("bench-log tests passed\n");
  return 0;
}
