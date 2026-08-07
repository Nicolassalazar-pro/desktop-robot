#include "nvs.h"

#include <cstring>
#include <unordered_map>

namespace {

using BlobMap =
    std::unordered_map<std::string, std::vector<uint8_t>>;

BlobMap g_durableBlobs;
std::unordered_map<nvs_handle_t, BlobMap> g_workingBlobs;
nvs_handle_t g_nextHandle = 1;
bool g_failNextCommit = false;

}  // namespace

extern "C" {

esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t* out_handle) {
  (void)open_mode;
  if (namespace_name == nullptr || out_handle == nullptr) {
    return ESP_FAIL;
  }
  const nvs_handle_t handle = g_nextHandle++;
  g_workingBlobs[handle] = g_durableBlobs;
  *out_handle = handle;
  return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
  g_workingBlobs.erase(handle);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key,
                       void* out_value, size_t* length) {
  const auto working = g_workingBlobs.find(handle);
  if (working == g_workingBlobs.end() ||
      key == nullptr || length == nullptr) {
    return ESP_FAIL;
  }
  const auto found = working->second.find(key);
  if (found == working->second.end()) {
    return ESP_ERR_NVS_NOT_FOUND;
  }
  if (out_value == nullptr) {
    *length = found->second.size();
    return ESP_OK;
  }
  if (*length < found->second.size()) {
    *length = found->second.size();
    return ESP_ERR_NVS_INVALID_LENGTH;
  }
  std::memcpy(out_value, found->second.data(), found->second.size());
  *length = found->second.size();
  return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key,
                       const void* value, size_t length) {
  const auto working = g_workingBlobs.find(handle);
  if (working == g_workingBlobs.end() ||
      key == nullptr || value == nullptr) {
    return ESP_FAIL;
  }
  const auto* bytes = static_cast<const uint8_t*>(value);
  working->second[key] =
      std::vector<uint8_t>(bytes, bytes + length);
  return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key) {
  const auto working = g_workingBlobs.find(handle);
  if (working == g_workingBlobs.end() || key == nullptr) {
    return ESP_FAIL;
  }
  return working->second.erase(key) == 0
             ? ESP_ERR_NVS_NOT_FOUND
             : ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
  const auto working = g_workingBlobs.find(handle);
  if (working == g_workingBlobs.end()) {
    return ESP_FAIL;
  }
  if (g_failNextCommit) {
    g_failNextCommit = false;
    return ESP_FAIL;
  }
  g_durableBlobs = working->second;
  return ESP_OK;
}

}  // extern "C"

namespace FakeNvs {

void reset() {
  g_durableBlobs.clear();
  g_workingBlobs.clear();
  g_nextHandle = 1;
  g_failNextCommit = false;
}

bool hasKey(const std::string& key) {
  return g_durableBlobs.find(key) != g_durableBlobs.end();
}

std::vector<uint8_t> getBlob(const std::string& key) {
  return g_durableBlobs.at(key);
}

void putBlob(const std::string& key, const std::vector<uint8_t>& value) {
  g_durableBlobs[key] = value;
}

void failNextCommit() {
  g_failNextCommit = true;
}

}  // namespace FakeNvs
