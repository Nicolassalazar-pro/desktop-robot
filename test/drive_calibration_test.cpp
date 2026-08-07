#include "../src/drive_calibration.h"
#include "nvs.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <vector>

#ifdef NDEBUG
#error "Host safety tests require assertions enabled"
#endif

namespace {

using DriveCalibration::Field;
using DriveCalibration::MotorCalibration;
using DriveCalibration::Side;
using DriveCalibration::Snapshot;
using DriveCalibration::Store;

constexpr uint32_t kMagic = 0x4443414CUL;
constexpr uint16_t kSchema = 3;
constexpr uint8_t kUserSource = 1;
constexpr uint8_t kPersistedFlags = 0x07;

#pragma pack(push, 1)
struct PersistedRecord {
  uint32_t magic;
  uint16_t schema;
  uint16_t size;
  uint32_t generation;
  uint8_t source;
  uint8_t flags;
  uint16_t reserved;
  MotorCalibration left;
  MotorCalibration right;
  uint32_t crc32;
};
#pragma pack(pop)

static_assert(sizeof(PersistedRecord) == 40,
              "test persistence layout must match production");

uint32_t crc32(const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

MotorCalibration leftA() {
  return {1500, 1550, 1450, 1900, 1100};
}

MotorCalibration rightA() {
  return {1500, 1450, 1550, 1100, 1900};
}

MotorCalibration leftB() {
  return {1502, 1560, 1440, 1950, 1050};
}

MotorCalibration rightB() {
  return {1498, 1440, 1560, 1050, 1950};
}

void expectSuccess(const DriveCalibration::Result& result) {
  assert(result.success);
}

// Mirrors quantizedPulseTicks() in drive_calibration.cpp: LEDC 14-bit at 50Hz.
uint32_t quantizedTicks(uint16_t pulseUs) {
  return static_cast<uint32_t>(pulseUs) * (1UL << 14) / 20000UL;
}

void configure(Store& store, const MotorCalibration& left,
               const MotorCalibration& right) {
  expectSuccess(store.setMotor(Side::Left, left));
  expectSuccess(store.setMotor(Side::Right, right));
}

PersistedRecord readRecord(const char* key) {
  const std::vector<uint8_t> blob = FakeNvs::getBlob(key);
  assert(blob.size() == sizeof(PersistedRecord));
  PersistedRecord record = {};
  memcpy(&record, blob.data(), sizeof(record));
  return record;
}

void writeRecord(const char* key, PersistedRecord record) {
  record.magic = kMagic;
  record.schema = kSchema;
  record.size = sizeof(record);
  record.source = kUserSource;
  record.flags = kPersistedFlags;
  record.reserved = 0;
  record.crc32 = crc32(&record, offsetof(PersistedRecord, crc32));
  const auto* begin = reinterpret_cast<const uint8_t*>(&record);
  FakeNvs::putBlob(
      key, std::vector<uint8_t>(begin, begin + sizeof(record)));
}

void seedTwoGenerations() {
  FakeNvs::reset();
  Store store;
  expectSuccess(store.load());
  configure(store, leftA(), rightA());
  expectSuccess(store.saveConfirmed());
  store.resetDraft();
  configure(store, leftB(), rightB());
  expectSuccess(store.saveConfirmed());
  assert(FakeNvs::hasKey("us_a"));
  assert(FakeNvs::hasKey("us_b"));
}

void testStoreRequiresExplicitCompleteCalibration() {
  FakeNvs::reset();
  Store store;
  expectSuccess(store.load());
  expectSuccess(store.set(Side::Left, Field::Stop, 1500));
  const DriveCalibration::Result oneField = store.saveConfirmed();
  assert(!oneField.success);
  assert(strcmp(oneField.error, "all calibration fields required") == 0);

  store.resetDraft();
  expectSuccess(store.setMotor(Side::Left, leftA()));
  const DriveCalibration::Result incomplete = store.saveConfirmed();
  assert(!incomplete.success);
  assert(strcmp(incomplete.error, "all calibration fields required") == 0);

  expectSuccess(store.setMotor(Side::Right, rightA()));
  expectSuccess(store.saveConfirmed());
  assert(store.eligible());
}

void testAllTenFieldSettersCanSave() {
  FakeNvs::reset();
  Store store;
  expectSuccess(store.load());
  const MotorCalibration left = leftA();
  const MotorCalibration right = rightA();
  const struct {
    Side side;
    Field field;
    int value;
  } values[] = {
      {Side::Left, Field::Stop, left.stopUs},
      {Side::Left, Field::ForwardStart, left.fwdStartUs},
      {Side::Left, Field::ReverseStart, left.revStartUs},
      {Side::Left, Field::ForwardMax, left.fwdMaxUs},
      {Side::Left, Field::ReverseMax, left.revMaxUs},
      {Side::Right, Field::Stop, right.stopUs},
      {Side::Right, Field::ForwardStart, right.fwdStartUs},
      {Side::Right, Field::ReverseStart, right.revStartUs},
      {Side::Right, Field::ForwardMax, right.fwdMaxUs},
      {Side::Right, Field::ReverseMax, right.revMaxUs},
  };
  for (const auto& value : values) {
    expectSuccess(store.set(value.side, value.field, value.value));
  }
  expectSuccess(store.saveConfirmed());
  assert(store.eligible());
}

void testQuantizedStartAliasIsRejected() {
  const MotorCalibration forwardAlias =
      {1454, 1455, 1400, 2000, 1000};
  const DriveCalibration::Result forwardResult =
      DriveCalibration::validateMotor(forwardAlias);
  assert(!forwardResult.success);
  assert(strstr(forwardResult.error, "14-bit quantization") != nullptr);

  const MotorCalibration reverseAlias =
      {1455, 1500, 1454, 2000, 1000};
  const DriveCalibration::Result reverseResult =
      DriveCalibration::validateMotor(reverseAlias);
  assert(!reverseResult.success);
  assert(strstr(reverseResult.error, "14-bit quantization") != nullptr);
}

void testMeasuredBenchDeadBandCalibrates() {
  // Bench measurement 2026-08-03: no shaft motion for any requested pulse in
  // 1423..1533 inclusive. Both MG90D units come from one batch, so the same
  // band is applied to each side with mirrored polarity.
  constexpr uint16_t kDeadLowUs = 1423;
  constexpr uint16_t kDeadHighUs = 1533;
  constexpr uint16_t kStopUs = 1478;
  constexpr uint16_t kAboveUs = kDeadHighUs + 1;
  constexpr uint16_t kBelowUs = kDeadLowUs - 1;

  const MotorCalibration forwardHigh =
      {kStopUs, kAboveUs, kBelowUs, 2000, 1000};
  const MotorCalibration forwardLow =
      {kStopUs, kBelowUs, kAboveUs, 1000, 2000};

  // Wiring and mounting are still unknown, so every polarity pairing the
  // installer might end up with has to be storable.
  const MotorCalibration options[] = {forwardHigh, forwardLow};
  for (const auto& left : options) {
    for (const auto& right : options) {
      FakeNvs::reset();
      Store store;
      expectSuccess(store.load());
      configure(store, left, right);
      expectSuccess(store.saveConfirmed());
      assert(store.eligible());
    }
  }

  // Stop has to quantize inside the measured band while both starts land on
  // ticks outside it; otherwise a start request cannot move the shaft.
  const uint32_t lowTicks = quantizedTicks(kDeadLowUs);
  const uint32_t highTicks = quantizedTicks(kDeadHighUs);
  const uint32_t stopTicks = quantizedTicks(kStopUs);
  assert(stopTicks >= lowTicks && stopTicks <= highTicks);
  assert(quantizedTicks(kAboveUs) > highTicks);
  assert(quantizedTicks(kBelowUs) < lowTicks);

  // Stop must also sit inside the range the drive mapping is allowed to use.
  assert(kStopUs >= 1450 && kStopUs <= 1550);
}

void testResetRetainsAndEraseRemovesBaseline() {
  FakeNvs::reset();
  Store store;
  expectSuccess(store.load());
  configure(store, leftA(), rightA());
  expectSuccess(store.saveConfirmed());
  assert(store.retainedGeneration() == 1);

  store.resetDraft();
  assert(!store.eligible());
  assert(store.hasRetainedBaseline());
  assert(store.retainedGeneration() == 1);

  Store rebooted;
  expectSuccess(rebooted.load());
  assert(rebooted.eligible());
  assert(rebooted.generation() == 1);

  expectSuccess(rebooted.eraseAndReset());
  Store erasedReboot;
  expectSuccess(erasedReboot.load());
  assert(!erasedReboot.eligible());
  assert(!erasedReboot.hasRetainedBaseline());
}

void testEqualDivergentGenerationLocks() {
  seedTwoGenerations();
  PersistedRecord slotA = readRecord("us_a");
  PersistedRecord slotB = readRecord("us_b");
  slotB.generation = slotA.generation;
  writeRecord("us_b", slotB);

  Store store;
  const DriveCalibration::Result result = store.load();
  assert(!result.success);
  assert(strcmp(result.error, "divergent equal-generation calibration") == 0);
  assert(!store.eligible());
}

void testHalfRangeGenerationLocks() {
  seedTwoGenerations();
  const PersistedRecord slotA = readRecord("us_a");
  PersistedRecord slotB = readRecord("us_b");
  slotB.generation = slotA.generation + 0x80000000UL;
  writeRecord("us_b", slotB);

  Store store;
  const DriveCalibration::Result result = store.load();
  assert(!result.success);
  assert(strcmp(result.error, "ambiguous calibration generation") == 0);
}

void testIdenticalEqualGenerationLoads() {
  seedTwoGenerations();
  FakeNvs::putBlob("us_b", FakeNvs::getBlob("us_a"));
  Store store;
  expectSuccess(store.load());
  assert(store.eligible());
  assert(store.generation() == 1);
}

void testGenerationWrapSelectsZeroAfterMax() {
  seedTwoGenerations();
  PersistedRecord slotA = readRecord("us_a");
  PersistedRecord slotB = readRecord("us_b");
  slotA.generation = UINT32_MAX;
  slotB.generation = 0;
  writeRecord("us_a", slotA);
  writeRecord("us_b", slotB);

  Store store;
  expectSuccess(store.load());
  const Snapshot selected = store.snapshot();
  assert(selected.generation == 0);
  assert(memcmp(&selected.left, &slotB.left, sizeof(slotB.left)) == 0);
}

void testCorruptNewerSlotFallsBack() {
  seedTwoGenerations();
  std::vector<uint8_t> slotB = FakeNvs::getBlob("us_b");
  slotB.back() ^= 0x80U;
  FakeNvs::putBlob("us_b", slotB);

  Store store;
  expectSuccess(store.load());
  assert(store.eligible());
  assert(store.generation() == 1);
}

void testLegacyKeysAreErased() {
  FakeNvs::reset();
  FakeNvs::putBlob("slot_a", {1, 2, 3});
  FakeNvs::putBlob("slot_b", {4, 5, 6});
  Store store;
  expectSuccess(store.load());
  assert(!FakeNvs::hasKey("slot_a"));
  assert(!FakeNvs::hasKey("slot_b"));
  assert(!store.eligible());
}

void testInterruptedStageCommitCleansTargetAndRetries() {
  FakeNvs::reset();
  Store store;
  expectSuccess(store.load());
  configure(store, leftA(), rightA());
  FakeNvs::failNextCommit();
  const DriveCalibration::Result failed = store.saveConfirmed();
  assert(!failed.success);
  assert(!FakeNvs::hasKey("us_a"));
  expectSuccess(store.saveConfirmed());
  assert(store.eligible());
}

}  // namespace

int main() {
  testStoreRequiresExplicitCompleteCalibration();
  testAllTenFieldSettersCanSave();
  testQuantizedStartAliasIsRejected();
  testMeasuredBenchDeadBandCalibrates();
  testResetRetainsAndEraseRemovesBaseline();
  testEqualDivergentGenerationLocks();
  testHalfRangeGenerationLocks();
  testIdenticalEqualGenerationLoads();
  testGenerationWrapSelectsZeroAfterMax();
  testCorruptNewerSlotFallsBack();
  testLegacyKeysAreErased();
  testInterruptedStageCommitCleansTargetAndRetries();
  return 0;
}
