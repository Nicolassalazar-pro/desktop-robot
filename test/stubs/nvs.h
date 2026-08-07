#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t esp_err_t;
typedef uint32_t nvs_handle_t;

typedef enum {
  NVS_READONLY = 0,
  NVS_READWRITE = 1,
} nvs_open_mode_t;

enum {
  ESP_OK = 0,
  ESP_FAIL = -1,
  ESP_ERR_NVS_NOT_FOUND = 0x1102,
  ESP_ERR_NVS_INVALID_LENGTH = 0x110C,
};

esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t* out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key,
                       void* out_value, size_t* length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key,
                       const void* value, size_t length);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key);
esp_err_t nvs_commit(nvs_handle_t handle);

#ifdef __cplusplus
}

#include <string>
#include <vector>

namespace FakeNvs {

void reset();
bool hasKey(const std::string& key);
std::vector<uint8_t> getBlob(const std::string& key);
void putBlob(const std::string& key, const std::vector<uint8_t>& value);
void failNextCommit();

}  // namespace FakeNvs
#endif
