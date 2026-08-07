#include "drive_calibration.h"

#include <nvs.h>
#include <stddef.h>
#include <string.h>

namespace DriveCalibration {
namespace {

constexpr char kNamespace[] = "drivecal";
constexpr char kSlotAKey[] = "us_a";
constexpr char kSlotBKey[] = "us_b";
constexpr char kLegacySlotAKey[] = "slot_a";
constexpr char kLegacySlotBKey[] = "slot_b";
constexpr uint32_t kMagic = 0x4443414CUL;  // "DCAL"
constexpr uint16_t kSchema = 3;
constexpr uint16_t kPulseMinUs = 1000;
constexpr uint16_t kPulseMaxUs = 2000;
constexpr uint16_t kStopMinUs = 1450;
constexpr uint16_t kStopMaxUs = 1550;
constexpr uint8_t kServoTimerWidthBits = 14;
constexpr uint32_t kServoPeriodUs = 20000;
constexpr uint32_t kServoTimerTicks = 1UL << kServoTimerWidthBits;
constexpr uint16_t kLeftFieldsMask = 0x001FU;
constexpr uint16_t kRightFieldsMask = 0x03E0U;
constexpr uint16_t kAllFieldsMask =
    kLeftFieldsMask | kRightFieldsMask;
constexpr uint8_t kConfirmedLockedFlags =
    static_cast<uint8_t>(Flag::Confirmed) |
    static_cast<uint8_t>(Flag::Locked);
constexpr uint8_t kPersistedFlags =
    kConfirmedLockedFlags | static_cast<uint8_t>(Flag::Verified);
constexpr uint8_t kKnownFlags = kPersistedFlags;

constexpr Result ok() {
  return {true, ""};
}

constexpr Result fail(const char* error) {
  return {false, error};
}

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

static_assert(sizeof(MotorCalibration) == 10,
              "MotorCalibration persistence layout changed");
static_assert(offsetof(MotorCalibration, stopUs) == 0,
              "MotorCalibration stop layout changed");
static_assert(offsetof(MotorCalibration, fwdStartUs) == 2,
              "MotorCalibration forward start layout changed");
static_assert(offsetof(MotorCalibration, revStartUs) == 4,
              "MotorCalibration reverse start layout changed");
static_assert(offsetof(MotorCalibration, fwdMaxUs) == 6,
              "MotorCalibration forward max layout changed");
static_assert(offsetof(MotorCalibration, revMaxUs) == 8,
              "MotorCalibration reverse max layout changed");
static_assert(sizeof(PersistedRecord) == 40,
              "Persisted calibration record layout changed");
static_assert(offsetof(PersistedRecord, crc32) == 36,
              "CRC must be the final record field");

enum class ReadState : uint8_t {
  Missing,
  Invalid,
  Valid,
  IoError,
};

enum class CleanupState : uint8_t {
  Missing,
  Erased,
  EraseError,
  CommitError,
};

struct ReadResult {
  ReadState state;
  PersistedRecord record;
};

enum class PresenceState : uint8_t {
  Missing,
  Present,
  IoError,
};

uint32_t crc32(const void* data, size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
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

bool validSource(Source source) {
  return (source == Source::LegacyDefaults) || (source == Source::User);
}

uint32_t quantizedPulseTicks(uint16_t pulseUs) {
  return static_cast<uint32_t>(pulseUs) * kServoTimerTicks /
         kServoPeriodUs;
}

Result validateMotorImpl(const MotorCalibration& motor) {
  if ((motor.stopUs < kPulseMinUs) || (motor.stopUs > kPulseMaxUs) ||
      (motor.fwdStartUs < kPulseMinUs) ||
      (motor.fwdStartUs > kPulseMaxUs) ||
      (motor.revStartUs < kPulseMinUs) ||
      (motor.revStartUs > kPulseMaxUs) ||
      (motor.fwdMaxUs < kPulseMinUs) ||
      (motor.fwdMaxUs > kPulseMaxUs) ||
      (motor.revMaxUs < kPulseMinUs) ||
      (motor.revMaxUs > kPulseMaxUs)) {
    return fail("pulse must be 1000..2000us");
  }
  if ((motor.stopUs < kStopMinUs) || (motor.stopUs > kStopMaxUs)) {
    return fail("stop must be 1450..1550us");
  }
  const uint32_t stopTicks = quantizedPulseTicks(motor.stopUs);
  if ((quantizedPulseTicks(motor.fwdStartUs) == stopTicks) ||
      (quantizedPulseTicks(motor.revStartUs) == stopTicks)) {
    return fail(
        "starts must differ from stop after 14-bit quantization");
  }

  const bool forwardHigh = motor.fwdStartUs > motor.stopUs;
  if ((motor.fwdStartUs == motor.stopUs) ||
      (motor.revStartUs == motor.stopUs) ||
      (forwardHigh == (motor.revStartUs > motor.stopUs))) {
    return fail("starts must be on opposite sides of stop");
  }
  if (forwardHigh) {
    if (motor.fwdMaxUs < motor.fwdStartUs) {
      return fail("forward max must be at or beyond forward start");
    }
    if (motor.revMaxUs > motor.revStartUs) {
      return fail("reverse max must be at or beyond reverse start");
    }
  } else {
    if (motor.fwdMaxUs > motor.fwdStartUs) {
      return fail("forward max must be at or beyond forward start");
    }
    if (motor.revMaxUs < motor.revStartUs) {
      return fail("reverse max must be at or beyond reverse start");
    }
  }
  return ok();
}

Result validateSnapshotImpl(const Snapshot& snapshot) {
  if (!validSource(snapshot.source)) {
    return fail("invalid source");
  }
  if ((snapshot.flags & static_cast<uint8_t>(~kKnownFlags)) != 0U) {
    return fail("invalid flags");
  }
  Result result = validateMotorImpl(snapshot.left);
  if (!result) {
    return result;
  }
  return validateMotorImpl(snapshot.right);
}

PresenceState keyPresence(nvs_handle_t handle, const char* key) {
  size_t storedSize = 0;
  const esp_err_t error = nvs_get_blob(handle, key, nullptr, &storedSize);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    return PresenceState::Missing;
  }
  return error == ESP_OK ? PresenceState::Present : PresenceState::IoError;
}

bool validatePersisted(const PersistedRecord& record, uint8_t expectedFlags) {
  if ((record.magic != kMagic) || (record.schema != kSchema) ||
      (record.size != sizeof(PersistedRecord)) || (record.reserved != 0U)) {
    return false;
  }
  if ((record.source != static_cast<uint8_t>(Source::User)) ||
      (record.flags != expectedFlags)) {
    return false;
  }
  if (record.crc32 != crc32(&record, offsetof(PersistedRecord, crc32))) {
    return false;
  }

  Snapshot snapshot = {};
  snapshot.left = record.left;
  snapshot.right = record.right;
  snapshot.source = static_cast<Source>(record.source);
  snapshot.flags = record.flags;
  snapshot.generation = record.generation;
  snapshot.dirty = false;
  snapshot.loadedFromNvs = true;
  return validateSnapshotImpl(snapshot).success;
}

ReadResult readSlot(nvs_handle_t handle, const char* key,
                    uint8_t expectedFlags) {
  ReadResult result = {};
  size_t storedSize = 0;
  esp_err_t error = nvs_get_blob(handle, key, nullptr, &storedSize);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    result.state = ReadState::Missing;
    return result;
  }
  if (error != ESP_OK) {
    result.state = ReadState::IoError;
    return result;
  }
  if (storedSize != sizeof(PersistedRecord)) {
    result.state = ReadState::Invalid;
    return result;
  }

  size_t readSize = sizeof(result.record);
  error = nvs_get_blob(handle, key, &result.record, &readSize);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    result.state = ReadState::Missing;
    return result;
  }
  if ((error != ESP_OK) || (readSize != sizeof(result.record))) {
    result.state = ReadState::IoError;
    return result;
  }

  result.state =
      validatePersisted(result.record, expectedFlags)
          ? ReadState::Valid
          : ReadState::Invalid;
  return result;
}

Snapshot snapshotFromRecord(const PersistedRecord& record) {
  Snapshot snapshot = {};
  snapshot.left = record.left;
  snapshot.right = record.right;
  snapshot.source = static_cast<Source>(record.source);
  snapshot.flags = record.flags;
  snapshot.generation = record.generation;
  snapshot.dirty = false;
  snapshot.loadedFromNvs = true;
  return snapshot;
}

PersistedRecord recordFromSnapshot(const Snapshot& snapshot,
                                   uint32_t generation, uint8_t flags) {
  PersistedRecord record = {};
  record.magic = kMagic;
  record.schema = kSchema;
  record.size = sizeof(PersistedRecord);
  record.generation = generation;
  record.source = static_cast<uint8_t>(Source::User);
  record.flags = flags;
  record.reserved = 0;
  record.left = snapshot.left;
  record.right = snapshot.right;
  record.crc32 = crc32(&record, offsetof(PersistedRecord, crc32));
  return record;
}

enum class GenerationOrder : uint8_t {
  Equal,
  CandidateNewer,
  ReferenceNewer,
  Ambiguous,
};

GenerationOrder compareGeneration(uint32_t candidate, uint32_t reference) {
  if (candidate == reference) {
    return GenerationOrder::Equal;
  }
  const uint32_t delta = candidate - reference;
  if (delta == 0x80000000UL) {
    return GenerationOrder::Ambiguous;
  }
  return delta < 0x80000000UL
             ? GenerationOrder::CandidateNewer
             : GenerationOrder::ReferenceNewer;
}

uint16_t fieldMask(Side side, Field field) {
  const uint8_t offset = side == Side::Right ? 5U : 0U;
  return static_cast<uint16_t>(
      1U << (static_cast<uint8_t>(field) + offset));
}

CleanupState invalidateSlot(nvs_handle_t handle, const char* key) {
  const esp_err_t eraseError = nvs_erase_key(handle, key);
  if (eraseError == ESP_ERR_NVS_NOT_FOUND) {
    return CleanupState::Missing;
  }
  if (eraseError != ESP_OK) {
    return CleanupState::EraseError;
  }
  return nvs_commit(handle) == ESP_OK ? CleanupState::Erased
                                      : CleanupState::CommitError;
}

}  // namespace

bool hasFlag(uint8_t flags, Flag flag) {
  return (flags & static_cast<uint8_t>(flag)) != 0U;
}

Result validateMotor(const MotorCalibration& motor) {
  return validateMotorImpl(motor);
}

Result validateSnapshot(const Snapshot& snapshot) {
  return validateSnapshotImpl(snapshot);
}

Snapshot legacyDraft() {
  Snapshot snapshot = {};
  snapshot.left = {1500, 1550, 1450, 2000, 1000};
  snapshot.right = {1500, 1450, 1550, 1000, 2000};
  snapshot.source = Source::LegacyDefaults;
  snapshot.flags = 0;
  snapshot.generation = 0;
  snapshot.dirty = true;
  snapshot.loadedFromNvs = false;
  return snapshot;
}

Store::Store()
    : draft_(legacyDraft()),
      current_(legacyDraft()),
      currentSlot_(Slot::None),
      hasCurrent_(false),
      storageReady_(false),
      touchedMask_(0) {}

Result Store::load() {
  const auto lockDraft = [this](const char* error) {
    draft_ = legacyDraft();
    current_ = legacyDraft();
    currentSlot_ = Slot::None;
    hasCurrent_ = false;
    storageReady_ = false;
    touchedMask_ = 0;
    return fail(error);
  };

  nvs_handle_t handle = 0;
  const esp_err_t openError =
      nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (openError != ESP_OK) {
    return lockDraft("nvs open failed");
  }

  const esp_err_t eraseLegacyA = nvs_erase_key(handle, kLegacySlotAKey);
  const esp_err_t eraseLegacyB = nvs_erase_key(handle, kLegacySlotBKey);
  if (((eraseLegacyA != ESP_OK) &&
       (eraseLegacyA != ESP_ERR_NVS_NOT_FOUND)) ||
      ((eraseLegacyB != ESP_OK) &&
       (eraseLegacyB != ESP_ERR_NVS_NOT_FOUND))) {
    nvs_close(handle);
    return lockDraft("legacy erase failed");
  }
  if (nvs_commit(handle) != ESP_OK) {
    nvs_close(handle);
    return lockDraft("legacy erase commit failed");
  }
  const PresenceState legacyA =
      keyPresence(handle, kLegacySlotAKey);
  const PresenceState legacyB =
      keyPresence(handle, kLegacySlotBKey);
  if ((legacyA == PresenceState::IoError) ||
      (legacyB == PresenceState::IoError)) {
    nvs_close(handle);
    return lockDraft("legacy erase verify read failed");
  }
  if ((legacyA != PresenceState::Missing) ||
      (legacyB != PresenceState::Missing)) {
    nvs_close(handle);
    return lockDraft("legacy erase verify failed");
  }

  const ReadResult slotA = readSlot(handle, kSlotAKey, kPersistedFlags);
  const ReadResult slotB = readSlot(handle, kSlotBKey, kPersistedFlags);
  nvs_close(handle);

  if ((slotA.state == ReadState::IoError) ||
      (slotB.state == ReadState::IoError)) {
    return lockDraft("nvs read failed");
  }

  const bool validA = slotA.state == ReadState::Valid;
  const bool validB = slotB.state == ReadState::Valid;
  if (!validA && !validB) {
    if ((slotA.state == ReadState::Invalid) ||
        (slotB.state == ReadState::Invalid)) {
      return lockDraft("no valid calibration");
    }
    draft_ = legacyDraft();
    current_ = legacyDraft();
    currentSlot_ = Slot::None;
    hasCurrent_ = false;
    storageReady_ = true;
    touchedMask_ = 0;
    return ok();
  }

  bool chooseB = validB && !validA;
  if (validA && validB) {
    const GenerationOrder order =
        compareGeneration(slotB.record.generation,
                          slotA.record.generation);
    if (order == GenerationOrder::Ambiguous) {
      return lockDraft("ambiguous calibration generation");
    }
    if (order == GenerationOrder::Equal) {
      if (memcmp(&slotA.record, &slotB.record,
                 sizeof(slotA.record)) != 0) {
        return lockDraft("divergent equal-generation calibration");
      }
      chooseB = false;
    } else {
      chooseB = order == GenerationOrder::CandidateNewer;
    }
  }
  const PersistedRecord& selected = chooseB ? slotB.record : slotA.record;
  currentSlot_ = chooseB ? Slot::B : Slot::A;
  draft_ = snapshotFromRecord(selected);
  current_ = draft_;
  hasCurrent_ = true;
  storageReady_ = true;
  touchedMask_ = kAllFieldsMask;
  return ok();
}

Result Store::set(Side side, Field field, int value) {
  if (hasFlag(draft_.flags, Flag::Locked)) {
    return fail("calibration locked");
  }
  if ((side != Side::Left) && (side != Side::Right)) {
    return fail("invalid side");
  }
  if ((value < static_cast<int>(kPulseMinUs)) ||
      (value > static_cast<int>(kPulseMaxUs))) {
    return fail("value out of range");
  }

  Snapshot proposed = draft_;
  MotorCalibration& motor =
      (side == Side::Left) ? proposed.left : proposed.right;
  switch (field) {
    case Field::Stop:
      motor.stopUs = static_cast<uint16_t>(value);
      break;
    case Field::ForwardStart:
      motor.fwdStartUs = static_cast<uint16_t>(value);
      break;
    case Field::ReverseStart:
      motor.revStartUs = static_cast<uint16_t>(value);
      break;
    case Field::ForwardMax:
      motor.fwdMaxUs = static_cast<uint16_t>(value);
      break;
    case Field::ReverseMax:
      motor.revMaxUs = static_cast<uint16_t>(value);
      break;
    default:
      return fail("invalid field");
  }

  proposed.source = Source::User;
  proposed.flags = 0;
  proposed.generation = 0;
  proposed.dirty = true;
  proposed.loadedFromNvs = false;
  Result validation = validateSnapshotImpl(proposed);
  if (!validation) {
    return validation;
  }
  draft_ = proposed;
  touchedMask_ |= fieldMask(side, field);
  return ok();
}

Result Store::setMotor(Side side, const MotorCalibration& motor) {
  if (hasFlag(draft_.flags, Flag::Locked)) {
    return fail("calibration locked");
  }
  if ((side != Side::Left) && (side != Side::Right)) {
    return fail("invalid side");
  }

  Snapshot proposed = draft_;
  ((side == Side::Left) ? proposed.left : proposed.right) = motor;
  proposed.source = Source::User;
  proposed.flags = 0;
  proposed.generation = 0;
  proposed.dirty = true;
  proposed.loadedFromNvs = false;
  Result validation = validateSnapshotImpl(proposed);
  if (!validation) {
    return validation;
  }
  draft_ = proposed;
  touchedMask_ |=
      side == Side::Left ? kLeftFieldsMask : kRightFieldsMask;
  return ok();
}

Result Store::saveConfirmed() {
  if (hasFlag(draft_.flags, Flag::Locked)) {
    return fail("calibration locked");
  }

  const auto saveFailure = [this](const char* error) {
    draft_.flags = 0;
    draft_.generation = 0;
    draft_.dirty = true;
    draft_.loadedFromNvs = false;
    return fail(error);
  };
  if (!storageReady_) {
    return saveFailure("load required");
  }
  if (draft_.source != Source::User) {
    return saveFailure("explicit calibration required");
  }
  if (touchedMask_ != kAllFieldsMask) {
    return saveFailure("all calibration fields required");
  }

  Result validation = validateSnapshotImpl(draft_);
  if (!validation) {
    return saveFailure(validation.error);
  }

  const uint32_t nextGeneration =
      hasCurrent_ ? current_.generation + 1U : 1U;
  const Slot targetSlot =
      (currentSlot_ == Slot::A) ? Slot::B : Slot::A;
  const PersistedRecord staged =
      recordFromSnapshot(draft_, nextGeneration, kConfirmedLockedFlags);

  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    storageReady_ = false;
    return saveFailure("nvs open failed");
  }

  const char* key = (targetSlot == Slot::A) ? kSlotAKey : kSlotBKey;
  const auto cleanupFailure = [this, handle, key, &saveFailure](
                                  const char* originalError) {
    const CleanupState cleanup = invalidateSlot(handle, key);
    nvs_close(handle);
    if (cleanup == CleanupState::EraseError) {
      storageReady_ = false;
      return saveFailure("nvs cleanup erase failed");
    }
    if (cleanup == CleanupState::CommitError) {
      storageReady_ = false;
      return saveFailure("nvs cleanup commit failed");
    }
    return saveFailure(originalError);
  };

  if (nvs_set_blob(handle, key, &staged, sizeof(staged)) != ESP_OK) {
    return cleanupFailure("nvs stage write failed");
  }
  if (nvs_commit(handle) != ESP_OK) {
    return cleanupFailure("nvs stage commit failed");
  }

  const ReadResult stagedReadback =
      readSlot(handle, key, kConfirmedLockedFlags);
  if ((stagedReadback.state != ReadState::Valid) ||
      (memcmp(&stagedReadback.record, &staged, sizeof(staged)) != 0)) {
    return cleanupFailure(stagedReadback.state == ReadState::IoError
                              ? "nvs stage read failed"
                              : "nvs stage verify failed");
  }

  PersistedRecord committed = staged;
  committed.flags = kPersistedFlags;
  committed.crc32 =
      crc32(&committed, offsetof(PersistedRecord, crc32));
  if (nvs_set_blob(handle, key, &committed, sizeof(committed)) != ESP_OK) {
    return cleanupFailure("nvs verify write failed");
  }
  if (nvs_commit(handle) != ESP_OK) {
    return cleanupFailure("nvs verify commit failed");
  }

  const ReadResult readback =
      readSlot(handle, key, kPersistedFlags);
  if ((readback.state != ReadState::Valid) ||
      (memcmp(&readback.record, &committed, sizeof(committed)) != 0)) {
    return cleanupFailure(readback.state == ReadState::IoError
                              ? "nvs verify read failed"
                              : "nvs verify failed");
  }
  nvs_close(handle);

  draft_ = snapshotFromRecord(readback.record);
  current_ = draft_;
  currentSlot_ = targetSlot;
  hasCurrent_ = true;
  touchedMask_ = kAllFieldsMask;
  return ok();
}

Result Store::eraseAndReset() {
  const auto resetState = [this](bool storageReady) {
    draft_ = legacyDraft();
    current_ = legacyDraft();
    currentSlot_ = Slot::None;
    hasCurrent_ = false;
    storageReady_ = storageReady;
    touchedMask_ = 0;
  };

  resetState(false);

  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return fail("nvs open failed");
  }

  const char* const keys[] = {
      kSlotAKey, kSlotBKey, kLegacySlotAKey, kLegacySlotBKey};
  for (const char* key : keys) {
    const esp_err_t eraseError = nvs_erase_key(handle, key);
    if ((eraseError != ESP_OK) &&
        (eraseError != ESP_ERR_NVS_NOT_FOUND)) {
      nvs_close(handle);
      return fail("nvs erase failed");
    }
  }

  if (nvs_commit(handle) != ESP_OK) {
    nvs_close(handle);
    return fail("nvs commit failed");
  }

  for (const char* key : keys) {
    const PresenceState presence = keyPresence(handle, key);
    if (presence == PresenceState::IoError) {
      nvs_close(handle);
      return fail("nvs erase verify read failed");
    }
    if (presence != PresenceState::Missing) {
      nvs_close(handle);
      return fail("nvs erase verify failed");
    }
  }
  nvs_close(handle);

  resetState(true);
  return ok();
}

void Store::resetDraft() {
  draft_ = legacyDraft();
  touchedMask_ = 0;
}

Snapshot Store::snapshot() const {
  return draft_;
}

bool Store::eligible() const {
  return storageReady_ && validateSnapshotImpl(draft_).success &&
         hasFlag(draft_.flags, Flag::Confirmed) &&
         hasFlag(draft_.flags, Flag::Locked) &&
         hasFlag(draft_.flags, Flag::Verified) && !draft_.dirty;
}

bool Store::dirty() const {
  return draft_.dirty;
}

bool Store::loadedFromNvs() const {
  return draft_.loadedFromNvs;
}

uint32_t Store::generation() const {
  return draft_.generation;
}

bool Store::hasRetainedBaseline() const {
  return hasCurrent_;
}

uint32_t Store::retainedGeneration() const {
  return hasCurrent_ ? current_.generation : 0U;
}

}  // namespace DriveCalibration
