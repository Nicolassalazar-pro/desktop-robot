// Host tests for the RSSI survey store.
//
// The experiment this supports asks one question: does moving the robot change
// RSSI by more than the scatter at a fixed point? Predicted signal is 0.63 dB
// per 0.3 m and 1.98 dB per 1 m (n=2.5, AP at 5 m); measured noise at a fixed
// point is 1-2 dB. So the answer hinges on sub-dB differences, and the store
// must not round them away, must not average unlike things together, and must
// never present an unseen AP as a reading.

#include "rf_map.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "nvs.h"

namespace {

using RfMap::Store;

bool near(float a, float b, float tolerance) {
  return std::fabs(a - b) <= tolerance;
}

const uint8_t kApA[6] = {0x84, 0xD8, 0x1B, 0x32, 0x35, 0x6C};
const uint8_t kApB[6] = {0x1C, 0x28, 0xAF, 0x31, 0xE0, 0xC1};
const uint8_t kApC[6] = {0x04, 0xD9, 0xF5, 0x13, 0xA5, 0xBB};

void testEmptyStore() {
  FakeNvs::reset();
  Store store;
  assert(store.apCount() == 0);
  assert(store.positionCount() == 0);
  assert(!store.load());
}

void testMeanAndSpreadAcrossRepeats() {
  FakeNvs::reset();
  Store store;
  const int8_t p = store.beginPosition("corner1");
  assert(p == 0);
  // Real numbers from four back-to-back scans at a fixed point.
  const int16_t observed[] = {-54, -53, -53, -54};
  for (int16_t r : observed) assert(store.addSample(p, kApA, r));

  const RfMap::Reading& reading = store.readingAt(0, 0);
  assert(reading.samples == 4);
  assert(near(reading.meanTenthDbm / 10.0f, -53.5f, 0.06f));
  assert(near(reading.spreadTenthDbm / 10.0f, 1.0f, 0.01f));
}

void testSubDbResolutionIsPreserved() {
  // The whole experiment turns on sub-dB differences. Storing integer dBm
  // would discard exactly the quantity being measured.
  FakeNvs::reset();
  Store store;
  const int8_t p = store.beginPosition("p");
  store.addSample(p, kApA, -54);
  store.addSample(p, kApA, -53);
  // Mean is -53.5, which integer storage could not represent.
  assert(store.readingAt(0, 0).meanTenthDbm == -535);
}

void testRevisitingAPositionAccumulates() {
  // Repeatability across visits is the actual test, so a label must reopen the
  // same position rather than creating a second one.
  FakeNvs::reset();
  Store store;
  const int8_t first = store.beginPosition("corner1");
  store.addSample(first, kApA, -50);
  const int8_t again = store.beginPosition("corner1");
  assert(again == first);
  store.addSample(again, kApA, -60);
  assert(store.positionCount() == 1);
  assert(store.readingAt(0, 0).samples == 2);
  assert(near(store.readingAt(0, 0).meanTenthDbm / 10.0f, -55.0f, 0.06f));
  assert(near(store.readingAt(0, 0).spreadTenthDbm / 10.0f, 10.0f, 0.01f));
}

void testCompareRefusesWhenAnApWasNotSeenAtBothPositions() {
  // An AP one position never saw is NOT a reading of zero. Treating it as one
  // would manufacture an enormous apparent signal out of an absence - and the
  // AP set genuinely is unstable between scans, so this case is common.
  FakeNvs::reset();
  Store store;
  const int8_t a = store.beginPosition("A");
  const int8_t b = store.beginPosition("B");
  store.addSample(a, kApA, -50);
  store.addSample(b, kApA, -52);
  store.addSample(a, kApC, -90);   // only seen at A

  float delta = 999.0f;
  assert(store.compare(0, a, b, &delta, nullptr, nullptr));
  assert(near(delta, -2.0f, 0.06f));

  delta = 999.0f;
  assert(!store.compare(1, a, b, &delta, nullptr, nullptr));
  assert(delta == 999.0f);   // untouched
}

void testCompareReportsSpreadSoSignalCanBeJudgedAgainstNoise() {
  // A mean difference alone cannot answer the question. 2 dB between positions
  // means nothing if each position scatters by 5 dB.
  FakeNvs::reset();
  Store store;
  const int8_t a = store.beginPosition("A");
  const int8_t b = store.beginPosition("B");
  // Position A is noisy, B is clean.
  for (int16_t r : {-50, -55, -45}) store.addSample(a, kApA, r);
  for (int16_t r : {-52, -52, -52}) store.addSample(b, kApA, r);

  float delta = 0.0f, spread = 0.0f;
  uint16_t samples = 0;
  assert(store.compare(0, a, b, &delta, &spread, &samples));
  assert(near(delta, -2.0f, 0.2f));
  assert(near(spread, 10.0f, 0.01f));   // worst of the two
  assert(samples == 3);
  // The verdict the caller must reach: signal is far below noise.
  assert(std::fabs(delta) < spread);
}

void testApTableFullIgnoresNewRatherThanEvicting() {
  // Evicting an AP other positions already depend on would silently change
  // what a column means. A partial row is analysable; a shifting one is not.
  FakeNvs::reset();
  Store store;
  const int8_t p = store.beginPosition("p");
  uint8_t bssid[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < RfMap::kMaxAps; ++i) {
    bssid[5] = static_cast<uint8_t>(i);
    assert(store.addSample(p, bssid, -60));
  }
  assert(store.apCount() == RfMap::kMaxAps);
  bssid[5] = 0xFF;
  assert(!store.addSample(p, bssid, -60));
  assert(store.apCount() == RfMap::kMaxAps);
  // The first AP is still the first AP.
  assert(store.bssidAt(0)[5] == 0);
}

void testPositionTableFull() {
  FakeNvs::reset();
  Store store;
  char label[8];
  for (int i = 0; i < RfMap::kMaxPositions; ++i) {
    snprintf(label, sizeof(label), "p%d", i);
    assert(store.beginPosition(label) == static_cast<int8_t>(i));
  }
  assert(store.beginPosition("overflow") == -1);
}

void testSaveLoadRoundTrip() {
  FakeNvs::reset();
  {
    Store store;
    const int8_t a = store.beginPosition("NW");
    const int8_t b = store.beginPosition("NE");
    store.addSample(a, kApA, -54);
    store.addSample(a, kApA, -53);
    store.addSample(b, kApA, -51);
    store.addSample(b, kApB, -64);
    assert(store.save());
  }
  Store reloaded;
  assert(reloaded.load());
  assert(reloaded.positionCount() == 2);
  assert(reloaded.apCount() == 2);
  assert(strcmp(reloaded.labelAt(0), "NW") == 0);
  assert(strcmp(reloaded.labelAt(1), "NE") == 0);
  assert(near(reloaded.readingAt(0, 0).meanTenthDbm / 10.0f, -53.5f, 0.06f));
  assert(reloaded.readingAt(1, 1).samples == 1);

  // And accumulation must continue correctly after a reload, since a survey
  // spans multiple sessions and the board resets when serial reconnects.
  const int8_t a = reloaded.beginPosition("NW");
  assert(a == 0);
  assert(reloaded.addSample(a, kApA, -55));
  assert(reloaded.readingAt(0, 0).samples == 3);
  assert(near(reloaded.readingAt(0, 0).meanTenthDbm / 10.0f, -54.0f, 0.06f));
}

void testCorruptRecordIsTreatedAsAbsent() {
  FakeNvs::reset();
  {
    Store store;
    const int8_t p = store.beginPosition("p");
    store.addSample(p, kApA, -50);
    assert(store.save());
  }
  std::vector<uint8_t> bytes = FakeNvs::getBlob("survey");
  bytes[16] = static_cast<uint8_t>(bytes[16] ^ 0xFF);
  FakeNvs::putBlob("survey", bytes);

  Store reloaded;
  assert(!reloaded.load());
  assert(reloaded.positionCount() == 0);
}

void testPredictedSignalVersusMeasuredNoise() {
  // The decision this store exists to inform, encoded as a test so the
  // conclusion is checkable rather than asserted in prose.
  //
  // Predicted translation signal (n=2.5, AP at 5 m): 0.63 dB per 0.3 m.
  // Measured fixed-point noise: 1.0 dB on the strongest AP.
  FakeNvs::reset();
  Store store;
  const int8_t a = store.beginPosition("A");
  const int8_t b = store.beginPosition("B");
  for (int16_t r : {-54, -53, -53, -54}) store.addSample(a, kApA, r);
  // Same AP, 0.3 m away: mean shifted by the predicted 0.63 dB, same scatter.
  for (int16_t r : {-53, -53, -53, -52}) store.addSample(b, kApA, r);

  float delta = 0.0f, spread = 0.0f;
  assert(store.compare(0, a, b, &delta, &spread, nullptr));
  // Signal does NOT clear the noise floor at this baseline. That is the
  // expected outcome, and the store reports it honestly rather than hiding it
  // behind a mean.
  assert(std::fabs(delta) < spread);
}

}  // namespace

int main() {
  testEmptyStore();
  testMeanAndSpreadAcrossRepeats();
  testSubDbResolutionIsPreserved();
  testRevisitingAPositionAccumulates();
  testCompareRefusesWhenAnApWasNotSeenAtBothPositions();
  testCompareReportsSpreadSoSignalCanBeJudgedAgainstNoise();
  testApTableFullIgnoresNewRatherThanEvicting();
  testPositionTableFull();
  testSaveLoadRoundTrip();
  testCorruptRecordIsTreatedAsAbsent();
  testPredictedSignalVersusMeasuredNoise();
  printf("rf-map tests passed\n");
  return 0;
}
