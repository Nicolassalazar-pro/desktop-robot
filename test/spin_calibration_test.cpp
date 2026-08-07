// Host tests for the persisted full-turn calibration.
//
// The value itself is not safety-critical — a wrong turn length makes a
// gesture rotate the wrong amount, nothing more. What IS worth testing is that
// a bad record can never present itself as a good one, because a plausible
// wrong number that survives reboots is far worse than an obvious absence.

#include "spin_calibration.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "movement_expression.h"
#include "nvs.h"

namespace {

using SpinCalibration::Store;

constexpr char kKey[] = "turn";

std::vector<uint8_t> storedBytes() {
  return FakeNvs::getBlob(kKey);
}

void testDefaultsWhenNothingIsStored() {
  FakeNvs::reset();
  Store store;
  assert(store.fullTurnMs() == MovementExpression::kSpinDefaultMs);
  assert(store.savedMs() == 0);
  assert(!store.dirty());
  assert(!store.loadedFromNvs());

  // A missing record is not a failure, it is simply "not calibrated yet".
  assert(!store.load());
  assert(store.fullTurnMs() == MovementExpression::kSpinDefaultMs);
  assert(!store.loadedFromNvs());
}

void testStageRejectsUnusableValues() {
  FakeNvs::reset();
  Store store;
  assert(!store.stage(0));
  assert(!store.stage(MovementExpression::kSpinMinMs -
                      MovementExpression::kStepQuantumMs));
  assert(!store.stage(MovementExpression::kSpinMaxMs +
                      MovementExpression::kStepQuantumMs));
  assert(!store.stage(605));  // off the tick grid
  // A rejected stage must not disturb what is already in effect.
  assert(store.fullTurnMs() == MovementExpression::kSpinDefaultMs);
  assert(!store.dirty());
}

void testStageThenSaveRoundTrips() {
  FakeNvs::reset();
  Store store;
  assert(store.stage(1180));
  assert(store.fullTurnMs() == 1180);
  assert(store.dirty());
  assert(store.savedMs() == 0);

  assert(store.save());
  assert(!store.dirty());
  assert(store.savedMs() == 1180);
  assert(store.loadedFromNvs());

  // A fresh store must see it after a notional reboot.
  Store reloaded;
  assert(reloaded.load());
  assert(reloaded.fullTurnMs() == 1180);
  assert(reloaded.savedMs() == 1180);
  assert(!reloaded.dirty());
  assert(reloaded.loadedFromNvs());
}

void testStagingTheSavedValueIsNotDirty() {
  FakeNvs::reset();
  Store store;
  assert(store.stage(900));
  assert(store.save());
  assert(store.stage(1200));
  assert(store.dirty());
  assert(store.stage(900));
  assert(!store.dirty());
}

void testFailedCommitDoesNotClaimSuccess() {
  FakeNvs::reset();
  Store store;
  assert(store.stage(760));
  FakeNvs::failNextCommit();
  assert(!store.save());
  // The staged value survives so it can be retried, but nothing claims to be
  // persisted.
  assert(store.fullTurnMs() == 760);
  assert(store.savedMs() == 0);
  assert(!store.loadedFromNvs());

  Store reloaded;
  assert(!reloaded.load());
  assert(reloaded.fullTurnMs() == MovementExpression::kSpinDefaultMs);
}

void testCorruptRecordIsTreatedAsAbsent() {
  FakeNvs::reset();
  Store seed;
  assert(seed.stage(1400));
  assert(seed.save());
  std::vector<uint8_t> good = storedBytes();

  // Flip a byte in the payload. The checksum must catch it and the record must
  // be discarded, not repaired into some other number.
  std::vector<uint8_t> corrupt = good;
  corrupt[sizeof(uint32_t) + sizeof(uint16_t)] ^= 0xFF;
  FakeNvs::putBlob(kKey, corrupt);

  Store store;
  assert(!store.load());
  assert(store.fullTurnMs() == MovementExpression::kSpinDefaultMs);
  assert(store.savedMs() == 0);
  assert(!store.loadedFromNvs());
}

void testWrongMagicOrVersionIsRejected() {
  FakeNvs::reset();
  Store seed;
  assert(seed.stage(1000));
  assert(seed.save());
  const std::vector<uint8_t> good = storedBytes();

  std::vector<uint8_t> badMagic = good;
  badMagic[0] ^= 0xFF;
  FakeNvs::putBlob(kKey, badMagic);
  Store a;
  assert(!a.load());

  std::vector<uint8_t> badVersion = good;
  badVersion[sizeof(uint32_t)] ^= 0xFF;
  FakeNvs::putBlob(kKey, badVersion);
  Store b;
  assert(!b.load());
}

void testTruncatedRecordIsRejected() {
  FakeNvs::reset();
  Store seed;
  assert(seed.stage(1000));
  assert(seed.save());
  std::vector<uint8_t> truncated = storedBytes();
  truncated.pop_back();
  FakeNvs::putBlob(kKey, truncated);

  Store store;
  assert(!store.load());
  assert(store.fullTurnMs() == MovementExpression::kSpinDefaultMs);
}

void testOutOfRangeStoredValueIsRejected() {
  // A record can be structurally perfect and still hold a value the firmware
  // must not use — for instance one written by an older build with different
  // bounds. Range is re-checked on read, not assumed from the checksum.
  FakeNvs::reset();
  Store seed;
  assert(seed.stage(1000));
  assert(seed.save());
  std::vector<uint8_t> record = storedBytes();

  // Rewrite fullTurnMs to something out of range and fix the checksum, exactly
  // as a consistent-but-wrong record would look.
  const uint16_t hostile = 9000;
  std::memcpy(&record[sizeof(uint32_t) + sizeof(uint16_t)], &hostile,
              sizeof(hostile));
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < record.size() - sizeof(uint32_t); ++i) {
    hash ^= record[i];
    hash *= 16777619UL;
  }
  std::memcpy(&record[record.size() - sizeof(uint32_t)], &hash, sizeof(hash));
  FakeNvs::putBlob(kKey, record);

  Store store;
  assert(!store.load());
  assert(store.fullTurnMs() == MovementExpression::kSpinDefaultMs);
}

void testEraseReturnsToTheDefault() {
  FakeNvs::reset();
  Store store;
  assert(store.stage(1500));
  assert(store.save());
  assert(store.erase());
  assert(store.fullTurnMs() == MovementExpression::kSpinDefaultMs);
  assert(store.savedMs() == 0);
  assert(!store.loadedFromNvs());
  assert(!store.dirty());
  assert(!FakeNvs::hasKey(kKey));

  // Erasing when nothing is stored is not an error.
  assert(store.erase());
}

void testEveryStoredValueBuildsAWorkingExcited() {
  // The contract that actually matters: whatever survives a round trip must be
  // usable by the gesture, at both ends of the range.
  const uint16_t values[] = {MovementExpression::kSpinMinMs, 600, 1180,
                             MovementExpression::kSpinMaxMs};
  for (uint16_t value : values) {
    FakeNvs::reset();
    Store store;
    assert(store.stage(value));
    assert(store.save());
    Store reloaded;
    assert(reloaded.load());

    MovementExpression::Step storage[MovementExpression::kExcitedStepCap];
    MovementExpression::Pattern built{};
    assert(MovementExpression::buildExcited(
        reloaded.fullTurnMs(), storage,
        MovementExpression::kExcitedStepCap, built));
    assert(MovementExpression::validate(built));
  }
}

}  // namespace

int main() {
  testDefaultsWhenNothingIsStored();
  testStageRejectsUnusableValues();
  testStageThenSaveRoundTrips();
  testStagingTheSavedValueIsNotDirty();
  testFailedCommitDoesNotClaimSuccess();
  testCorruptRecordIsTreatedAsAbsent();
  testWrongMagicOrVersionIsRejected();
  testTruncatedRecordIsRejected();
  testOutOfRangeStoredValueIsRejected();
  testEraseReturnsToTheDefault();
  testEveryStoredValueBuildsAWorkingExcited();
  printf("spin-calibration tests passed\n");
  return 0;
}
