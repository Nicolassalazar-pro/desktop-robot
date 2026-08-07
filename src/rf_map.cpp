#include "rf_map.h"

#include <string.h>

#include "nvs.h"

namespace RfMap {
namespace {

constexpr char kNamespace[] = "rfmap";
constexpr char kKey[] = "survey";
constexpr uint32_t kMagic = 0x52464D50;  // "RFMP"
constexpr uint16_t kVersion = 1;

struct Record {
  uint32_t magic;
  uint16_t version;
  uint8_t apCount;
  uint8_t positionCount;
  uint8_t bssids[kMaxAps][kBssidLen];
  char labels[kMaxPositions][kLabelLen];
  Reading readings[kMaxPositions][kMaxAps];
  int16_t minTenth[kMaxPositions][kMaxAps];
  int16_t maxTenth[kMaxPositions][kMaxAps];
  int32_t sumTenth[kMaxPositions][kMaxAps];
  uint32_t checksum;
};

uint32_t fnv1a(const void* data, size_t length) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

uint32_t checksumOf(const Record& record) {
  return fnv1a(&record, sizeof(Record) - sizeof(uint32_t));
}

}  // namespace

Store::Store() : apCount_(0), positionCount_(0) {
  reset();
}

void Store::reset() {
  memset(bssids_, 0, sizeof(bssids_));
  memset(labels_, 0, sizeof(labels_));
  memset(readings_, 0, sizeof(readings_));
  memset(minTenth_, 0, sizeof(minTenth_));
  memset(maxTenth_, 0, sizeof(maxTenth_));
  memset(sumTenth_, 0, sizeof(sumTenth_));
  apCount_ = 0;
  positionCount_ = 0;
}

int8_t Store::findAp(const uint8_t* bssid) const {
  for (uint8_t i = 0; i < apCount_; ++i) {
    if (memcmp(bssids_[i], bssid, kBssidLen) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

int8_t Store::beginPosition(const char* label) {
  if (label == nullptr) return -1;
  // Reuse an existing label so a position can be revisited and its samples
  // accumulated - repeatability across visits is the actual test.
  for (uint8_t i = 0; i < positionCount_; ++i) {
    if (strncmp(labels_[i], label, kLabelLen - 1) == 0) {
      return static_cast<int8_t>(i);
    }
  }
  if (positionCount_ >= kMaxPositions) return -1;
  const uint8_t index = positionCount_++;
  strncpy(labels_[index], label, kLabelLen - 1);
  labels_[index][kLabelLen - 1] = '\0';
  return static_cast<int8_t>(index);
}

bool Store::addSample(int8_t position, const uint8_t* bssid, int16_t rssiDbm) {
  if (position < 0 || position >= static_cast<int8_t>(positionCount_)) {
    return false;
  }
  if (bssid == nullptr) return false;

  int8_t ap = findAp(bssid);
  if (ap < 0) {
    if (apCount_ >= kMaxAps) return false;
    ap = static_cast<int8_t>(apCount_++);
    memcpy(bssids_[ap], bssid, kBssidLen);
  }

  const int16_t tenth = static_cast<int16_t>(rssiDbm * 10);
  Reading& reading = readings_[position][ap];
  if (reading.samples == 0) {
    minTenth_[position][ap] = tenth;
    maxTenth_[position][ap] = tenth;
    sumTenth_[position][ap] = tenth;
  } else {
    if (tenth < minTenth_[position][ap]) minTenth_[position][ap] = tenth;
    if (tenth > maxTenth_[position][ap]) maxTenth_[position][ap] = tenth;
    sumTenth_[position][ap] += tenth;
  }
  ++reading.samples;
  reading.meanTenthDbm = static_cast<int16_t>(
      sumTenth_[position][ap] / static_cast<int32_t>(reading.samples));
  reading.spreadTenthDbm =
      static_cast<int16_t>(maxTenth_[position][ap] - minTenth_[position][ap]);
  return true;
}

uint8_t Store::apCount() const { return apCount_; }
uint8_t Store::positionCount() const { return positionCount_; }

const uint8_t* Store::bssidAt(uint8_t apIndex) const {
  if (apIndex >= apCount_) return bssids_[0];
  return bssids_[apIndex];
}

const char* Store::labelAt(uint8_t position) const {
  if (position >= positionCount_) return "";
  return labels_[position];
}

const Reading& Store::readingAt(uint8_t position, uint8_t apIndex) const {
  if (position >= kMaxPositions) position = 0;
  if (apIndex >= kMaxAps) apIndex = 0;
  return readings_[position][apIndex];
}

bool Store::compare(uint8_t apIndex, uint8_t positionA, uint8_t positionB,
                    float* deltaDbOut, float* worstSpreadDbOut,
                    uint16_t* minSamplesOut) const {
  if (apIndex >= apCount_) return false;
  if (positionA >= positionCount_ || positionB >= positionCount_) return false;
  const Reading& a = readings_[positionA][apIndex];
  const Reading& b = readings_[positionB][apIndex];
  // An AP that one position never saw is not a reading of zero; refusing is
  // the difference between "no signal" and "a huge apparent signal".
  if (a.samples == 0 || b.samples == 0) return false;

  if (deltaDbOut != nullptr) {
    *deltaDbOut =
        static_cast<float>(b.meanTenthDbm - a.meanTenthDbm) / 10.0f;
  }
  if (worstSpreadDbOut != nullptr) {
    const int16_t worst = a.spreadTenthDbm > b.spreadTenthDbm
                              ? a.spreadTenthDbm
                              : b.spreadTenthDbm;
    *worstSpreadDbOut = static_cast<float>(worst) / 10.0f;
  }
  if (minSamplesOut != nullptr) {
    *minSamplesOut = a.samples < b.samples ? a.samples : b.samples;
  }
  return true;
}

bool Store::load() {
  Record record{};
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  size_t size = sizeof(Record);
  const esp_err_t error = nvs_get_blob(handle, kKey, &record, &size);
  nvs_close(handle);
  if (error != ESP_OK || size != sizeof(Record)) return false;
  if (record.magic != kMagic || record.version != kVersion) return false;
  if (record.apCount > kMaxAps || record.positionCount > kMaxPositions) {
    return false;
  }
  if (record.checksum != checksumOf(record)) return false;

  memcpy(bssids_, record.bssids, sizeof(bssids_));
  memcpy(labels_, record.labels, sizeof(labels_));
  memcpy(readings_, record.readings, sizeof(readings_));
  memcpy(minTenth_, record.minTenth, sizeof(minTenth_));
  memcpy(maxTenth_, record.maxTenth, sizeof(maxTenth_));
  memcpy(sumTenth_, record.sumTenth, sizeof(sumTenth_));
  apCount_ = record.apCount;
  positionCount_ = record.positionCount;
  return true;
}

bool Store::save() {
  Record record{};
  record.magic = kMagic;
  record.version = kVersion;
  record.apCount = apCount_;
  record.positionCount = positionCount_;
  memcpy(record.bssids, bssids_, sizeof(bssids_));
  memcpy(record.labels, labels_, sizeof(labels_));
  memcpy(record.readings, readings_, sizeof(readings_));
  memcpy(record.minTenth, minTenth_, sizeof(minTenth_));
  memcpy(record.maxTenth, maxTenth_, sizeof(maxTenth_));
  memcpy(record.sumTenth, sumTenth_, sizeof(sumTenth_));
  record.checksum = checksumOf(record);

  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  if (nvs_set_blob(handle, kKey, &record, sizeof(record)) != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  const esp_err_t committed = nvs_commit(handle);
  nvs_close(handle);
  return committed == ESP_OK;
}

bool Store::erase() {
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  const esp_err_t error = nvs_erase_key(handle, kKey);
  if (error == ESP_OK) nvs_commit(handle);
  nvs_close(handle);
  reset();
  return error == ESP_OK || error == ESP_ERR_NVS_NOT_FOUND;
}

}  // namespace RfMap
