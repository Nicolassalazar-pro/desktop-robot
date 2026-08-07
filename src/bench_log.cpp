#include "bench_log.h"

#include <string.h>

#include "nvs.h"

namespace BenchLog {
namespace {

constexpr char kNamespace[] = "benchlog";
constexpr char kKey[] = "run";
constexpr char kArmedKey[] = "armed";
// Not simply "non-zero": a half-written or uninitialised byte must never be
// able to arm a run that drives the motors unattended.
constexpr uint8_t kArmedMagic = 0x5A;
constexpr uint32_t kMagic = 0x42454E43;  // "BENC"
// Bumped whenever the Entry layout or kMaxEntries changes. The size check below
// would catch it anyway, but relying on that is accidental: an edit that
// happened to preserve the total size would silently reinterpret old bytes.
constexpr uint16_t kVersion = 2;

// The persisted form. Fixed layout, checksummed, and validated on load: a
// record that fails any check is treated as absent rather than repaired, so a
// corrupt log can never turn into plausible-looking wrong measurements.
struct Record {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint8_t overflowed;
  uint8_t reserved[3];
  Entry entries[kMaxEntries];
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
  // Everything except the trailing checksum field itself.
  return fnv1a(&record, sizeof(Record) - sizeof(uint32_t));
}

bool readRecord(Record* out) {
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  size_t size = sizeof(Record);
  const esp_err_t error = nvs_get_blob(handle, kKey, out, &size);
  nvs_close(handle);
  if (error != ESP_OK || size != sizeof(Record)) return false;
  if (out->magic != kMagic) return false;
  if (out->version != kVersion) return false;
  if (out->count > kMaxEntries) return false;
  if (out->checksum != checksumOf(*out)) return false;
  return true;
}

}  // namespace

const char* metricName(Metric metric) {
  switch (metric) {
    case Metric::GravityX: return "gravity-x";
    case Metric::GravityY: return "gravity-y";
    case Metric::GravityZ: return "gravity-z";
    case Metric::ProjectionGain: return "projection-gain";
    case Metric::DriftDegrees: return "drift-deg";
    case Metric::BiasDps: return "bias-dps";
    case Metric::NoiseSpreadDps: return "noise-spread-dps";
    case Metric::SteadyRateDps: return "steady-rate-dps";
    case Metric::CoastDegrees: return "coast-deg";
    case Metric::CoastSeconds: return "coast-sec";
    case Metric::TurnErrorDegrees: return "turn-error-deg";
    case Metric::NetSequenceDegrees: return "net-sequence-deg";
    case Metric::VibrationSpreadDps: return "vibration-spread-dps";
    case Metric::None:
    default: return "none";
  }
}

const char* conditionName(uint8_t condition) {
  switch (condition) {
    case kCondLeadOn: return "lead-on";
    case kCondLeadOff: return "lead-off";
    case kCondNone:
    default: return "-";
  }
}

bool setArmed(bool armed) {
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  const uint8_t value = armed ? kArmedMagic : 0;
  esp_err_t error = nvs_set_blob(handle, kArmedKey, &value, sizeof(value));
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  return error == ESP_OK;
}

bool isArmed() {
  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  uint8_t value = 0;
  size_t size = sizeof(value);
  const esp_err_t error = nvs_get_blob(handle, kArmedKey, &value, &size);
  nvs_close(handle);
  // A specific magic rather than "non-zero": an uninitialised or partially
  // written byte must not be able to arm a run that drives the motors.
  return error == ESP_OK && size == sizeof(value) && value == kArmedMagic;
}

Store::Store() : count_(0), overflowed_(false) {
  memset(entries_, 0, sizeof(entries_));
}

void Store::reset() {
  count_ = 0;
  overflowed_ = false;
  memset(entries_, 0, sizeof(entries_));
}

bool Store::load() {
  Record record{};
  if (!readRecord(&record)) {
    reset();
    return false;
  }
  count_ = record.count;
  overflowed_ = record.overflowed != 0;
  memcpy(entries_, record.entries, sizeof(entries_));
  return true;
}

bool Store::add(Metric metric, int16_t param, float value, uint8_t condition) {
  if (count_ >= kMaxEntries) {
    overflowed_ = true;
    return false;
  }
  Entry& entry = entries_[count_];
  entry.metric = static_cast<uint8_t>(metric);
  entry.condition = condition;
  entry.param = param;
  entry.value = value;
  ++count_;
  return true;
}

bool Store::save() {
  Record record{};
  record.magic = kMagic;
  record.version = kVersion;
  record.count = count_;
  record.overflowed = overflowed_ ? 1 : 0;
  memcpy(record.entries, entries_, sizeof(entries_));
  record.checksum = checksumOf(record);

  nvs_handle_t handle = 0;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  if (nvs_set_blob(handle, kKey, &record, sizeof(record)) != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  if (nvs_commit(handle) != ESP_OK) {
    nvs_close(handle);
    return false;
  }
  nvs_close(handle);

  // Read back rather than trust the write. This log is the only copy of
  // measurements that cost the user a supervised untethered session; silently
  // persisting a corrupt one would waste the whole run.
  Record verify{};
  if (!readRecord(&verify)) return false;
  return verify.count == count_ && verify.checksum == record.checksum;
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

uint16_t Store::count() const { return count_; }
bool Store::full() const { return count_ >= kMaxEntries; }
bool Store::overflowed() const { return overflowed_; }

const Entry& Store::at(uint16_t index) const {
  if (index >= count_) return entries_[0];
  return entries_[index];
}

bool Store::summarise(Metric metric, uint8_t condition, float* meanOut,
                      float* spreadOut, uint16_t* countOut) const {
  return summariseParamInternal(metric, condition, 0, false, meanOut, spreadOut,
                                countOut);
}

bool Store::summariseParam(Metric metric, uint8_t condition, int16_t param,
                           float* meanOut, float* spreadOut,
                           uint16_t* countOut) const {
  return summariseParamInternal(metric, condition, param, true, meanOut,
                                spreadOut, countOut);
}

bool Store::summariseParamInternal(Metric metric, uint8_t condition,
                                   int16_t param, bool matchParam,
                                   float* meanOut, float* spreadOut,
                                   uint16_t* countOut) const {
  const uint8_t wanted = static_cast<uint8_t>(metric);
  double sum = 0.0;
  float minimum = 0.0f;
  float maximum = 0.0f;
  uint16_t matches = 0;
  for (uint16_t i = 0; i < count_; ++i) {
    const Entry& entry = entries_[i];
    if (entry.metric != wanted) continue;
    // kCondNone as the filter means "any condition", so a caller that does not
    // care about the A/B split still gets every repeat.
    if (condition != kCondNone && entry.condition != condition) continue;
    if (matchParam && entry.param != param) continue;
    if (matches == 0) {
      minimum = entry.value;
      maximum = entry.value;
    } else {
      if (entry.value < minimum) minimum = entry.value;
      if (entry.value > maximum) maximum = entry.value;
    }
    sum += static_cast<double>(entry.value);
    ++matches;
  }
  if (matches == 0) return false;
  if (meanOut != nullptr) {
    *meanOut = static_cast<float>(sum / static_cast<double>(matches));
  }
  if (spreadOut != nullptr) *spreadOut = maximum - minimum;
  if (countOut != nullptr) *countOut = matches;
  return true;
}

}  // namespace BenchLog
