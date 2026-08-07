#include "spin_calibration.h"

#include <string.h>

#include "movement_expression.h"
#include "nvs.h"

namespace SpinCalibration {
namespace {

constexpr char kNamespace[] = "spincal";
constexpr char kKey[] = "turn";

constexpr uint32_t kMagic = 0x53504E31UL;  // "SPN1"
constexpr uint16_t kVersion = 1;

struct Record {
  uint32_t magic;
  uint16_t version;
  uint16_t fullTurnMs;
  uint32_t checksum;
};

// FNV-1a over everything before the checksum field. Enough to reject a torn or
// garbage record; this is corruption detection, not tamper resistance.
uint32_t checksumOf(const Record& record) {
  uint32_t hash = 2166136261UL;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  const size_t span = sizeof(Record) - sizeof(record.checksum);
  for (size_t i = 0; i < span; ++i) {
    hash ^= bytes[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool recordIsUsable(const Record& record) {
  return record.magic == kMagic && record.version == kVersion &&
         record.checksum == checksumOf(record) &&
         MovementExpression::isValidFullTurnMs(record.fullTurnMs);
}

// Returns 0 when there is no usable record, for any reason.
uint16_t readStored() {
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return 0;
  }
  Record record{};
  size_t size = sizeof(record);
  const esp_err_t error = nvs_get_blob(handle, kKey, &record, &size);
  nvs_close(handle);
  if (error != ESP_OK || size != sizeof(record)) {
    return 0;
  }
  if (!recordIsUsable(record)) {
    return 0;
  }
  return record.fullTurnMs;
}

}  // namespace

Store::Store()
    : effective_(MovementExpression::kSpinDefaultMs),
      saved_(0),
      dirty_(false),
      loadedFromNvs_(false) {}

bool Store::load() {
  const uint16_t stored = readStored();
  if (stored == 0) {
    effective_ = MovementExpression::kSpinDefaultMs;
    saved_ = 0;
    dirty_ = false;
    loadedFromNvs_ = false;
    return false;
  }
  effective_ = stored;
  saved_ = stored;
  dirty_ = false;
  loadedFromNvs_ = true;
  return true;
}

bool Store::stage(uint16_t fullTurnMs) {
  if (!MovementExpression::isValidFullTurnMs(fullTurnMs)) {
    return false;
  }
  effective_ = fullTurnMs;
  dirty_ = (fullTurnMs != saved_);
  return true;
}

bool Store::save() {
  if (!MovementExpression::isValidFullTurnMs(effective_)) {
    return false;
  }

  Record record{};
  record.magic = kMagic;
  record.version = kVersion;
  record.fullTurnMs = effective_;
  record.checksum = checksumOf(record);

  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  if (nvs_set_blob(handle, kKey, &record, sizeof(record)) != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  if (nvs_commit(handle) != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  nvs_close(handle);

  // Read back rather than trusting the write. A save that reports success
  // while storing something else is the one failure that would silently
  // survive a reboot.
  if (readStored() != effective_) {
    return false;
  }
  saved_ = effective_;
  dirty_ = false;
  loadedFromNvs_ = true;
  return true;
}

bool Store::erase() {
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  const esp_err_t error = nvs_erase_key(handle, kKey);
  if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return false;
  }
  if (nvs_commit(handle) != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  nvs_close(handle);

  if (readStored() != 0) {
    return false;
  }
  effective_ = MovementExpression::kSpinDefaultMs;
  saved_ = 0;
  dirty_ = false;
  loadedFromNvs_ = false;
  return true;
}

uint16_t Store::fullTurnMs() const {
  return effective_;
}

uint16_t Store::savedMs() const {
  return saved_;
}

bool Store::dirty() const {
  return dirty_;
}

bool Store::loadedFromNvs() const {
  return loadedFromNvs_;
}

}  // namespace SpinCalibration
