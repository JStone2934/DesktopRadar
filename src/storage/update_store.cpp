#include "update_store.h"

#include <nvs.h>
#include <nvs_flash.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr const char* kNamespace = "radar_ota";

bool newer(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) > 0;
}

template <typename T>
bool readBlob(nvs_handle_t handle, const char* key, T* out) {
  size_t size = sizeof(T);
  return out && nvs_get_blob(handle, key, out, &size) == ESP_OK &&
         size == sizeof(T);
}

template <typename T, bool (*Valid)(const T&)>
bool loadDual(const char* key0, const char* key1, T* out) {
  if (!out) return false;
  T* candidate = static_cast<T*>(calloc(1, sizeof(T)));
  if (!candidate) return false;
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
    free(candidate);
    return false;
  }
  memset(out, 0, sizeof(T));
  const bool va = readBlob(handle, key0, out) && Valid(*out);
  const bool vb = readBlob(handle, key1, candidate) && Valid(*candidate);
  nvs_close(handle);
  if (vb && (!va || newer(candidate->sequence, out->sequence))) {
    *out = *candidate;
  }
  free(candidate);
  return va || vb;
}

template <typename T, bool (*Valid)(const T&)>
bool saveDual(const char* key0, const char* key1, T* record) {
  if (!record) return false;
  T* scratch = static_cast<T*>(calloc(1, sizeof(T)));
  if (!scratch) return false;
  const bool have = loadDual<T, Valid>(key0, key1, scratch);
  record->sequence = have ? scratch->sequence + 1U : 1U;
  record->crc32 = updateCrc32(record, offsetof(T, crc32));
  const char* key = (record->sequence & 1U) ? key1 : key0;
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    free(scratch);
    return false;
  }
  esp_err_t rc = nvs_set_blob(handle, key, record, sizeof(T));
  if (rc == ESP_OK) rc = nvs_commit(handle);
  if (rc == ESP_OK &&
      (!readBlob(handle, key, scratch) || !Valid(*scratch) ||
       scratch->sequence != record->sequence)) {
    rc = ESP_FAIL;
  }
  nvs_close(handle);
  free(scratch);
  return rc == ESP_OK;
}

}  // namespace

bool updateStoreBegin() {
  esp_err_t rc = nvs_flash_init();
  if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // Never erase the shared NVS partition automatically: it also contains
    // WiFi/PEAP credentials. Report the error and leave recovery decisions to
    // the user.
    return false;
  }
  return rc == ESP_OK;
}

bool updateStoreLoadManifest(UpdateManifestRecord* out) {
  return loadDual<UpdateManifestRecord, updateManifestRecordValid>(
      "manifest0", "manifest1", out);
}

bool updateStoreSaveManifest(UpdateManifestRecord* record) {
  record->magic = UPDATE_MANIFEST_MAGIC;
  record->schema = UPDATE_RECORD_SCHEMA;
  return saveDual<UpdateManifestRecord, updateManifestRecordValid>(
      "manifest0", "manifest1", record);
}

bool updateStoreLoadJournal(UpdateJournal* out) {
  return loadDual<UpdateJournal, updateJournalValid>("journal0", "journal1",
                                                     out);
}

bool updateStoreSaveJournal(UpdateJournal* journal) {
  journal->magic = UPDATE_JOURNAL_MAGIC;
  journal->schema = UPDATE_RECORD_SCHEMA;
  return saveDual<UpdateJournal, updateJournalValid>("journal0", "journal1",
                                                      journal);
}

bool updateStoreGetTime(const char* key, uint64_t* value) {
  if (!key || !value) return false;
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  const esp_err_t rc = nvs_get_u64(handle, key, value);
  nvs_close(handle);
  return rc == ESP_OK;
}

bool updateStoreSetTime(const char* key, uint64_t value) {
  if (!key) return false;
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t rc = nvs_set_u64(handle, key, value);
  if (rc == ESP_OK) rc = nvs_commit(handle);
  nvs_close(handle);
  return rc == ESP_OK;
}

bool updateStoreGetLastError(UpdateError* error) {
  if (!error) return false;
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  uint16_t raw = 0;
  const esp_err_t rc = nvs_get_u16(handle, "last_error", &raw);
  nvs_close(handle);
  if (rc != ESP_OK) return false;
  *error = static_cast<UpdateError>(raw);
  return true;
}

bool updateStoreSetLastError(UpdateError error) {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t rc = nvs_set_u16(handle, "last_error",
                             static_cast<uint16_t>(error));
  if (rc == ESP_OK) rc = nvs_commit(handle);
  nvs_close(handle);
  return rc == ESP_OK;
}

bool updateStoreProbe() {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  uint32_t before = 0;
  (void)nvs_get_u32(handle, "probe", &before);
  const uint32_t value = before + 1U;
  esp_err_t rc = nvs_set_u32(handle, "probe", value);
  if (rc == ESP_OK) rc = nvs_commit(handle);
  uint32_t after = 0;
  if (rc == ESP_OK) rc = nvs_get_u32(handle, "probe", &after);
  nvs_close(handle);
  return rc == ESP_OK && after == value;
}
