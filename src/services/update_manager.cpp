#include "update_manager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "button.h"
#include "config.h"
#include "update_store.h"
#include "version.h"
#include "wifi_sta.h"

extern "C" bool verifyRollbackLater() { return true; }

namespace {

constexpr uint64_t kDaySeconds = 24ULL * 60ULL * 60ULL;
constexpr time_t kValidEpoch = 1700000000;
constexpr size_t kManifestHttpMax = 4096;
constexpr uint32_t kFactoryOffset = 0x10000;
constexpr uint32_t kFactorySize = 0x80000;
constexpr uint32_t kAppOffset = 0x90000;
constexpr uint32_t kAppSize = 0x1B0000;
constexpr uint32_t kFsOffset = 0x240000;
constexpr uint32_t kFsSize = 0x1B0000;

volatile bool s_dailyActive = false;
volatile bool s_dailyDone = false;
volatile UpdateError s_dailyResult = UpdateError::None;
bool s_timeStarted = false;
bool s_filesystemReset = false;
bool s_exclusive = false;
uint32_t s_nonce = 0;
uint32_t s_dailyIdleSince = 0;

struct ManifestRecordBuffer {
  UpdateManifestRecord* value;

  ManifestRecordBuffer()
      : value(static_cast<UpdateManifestRecord*>(
            calloc(1, sizeof(UpdateManifestRecord)))) {}
  ~ManifestRecordBuffer() { free(value); }
};

void drawUpdateStatus(LGFX* lcd, const char* title, const char* detail) {
  if (!lcd) return;
  lcd->fillScreen(TFT_BLACK);
  lcd->setTextDatum(MC_DATUM);
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setFont(&fonts::Font4);
  lcd->drawString(title ? title : "UPDATE", LCD_WIDTH / 2,
                  LCD_HEIGHT / 2 - 14);
  if (detail) {
    lcd->setFont(&fonts::Font2);
    lcd->drawString(detail, LCD_WIDTH / 2, LCD_HEIGHT / 2 + 18);
  }
}

bool configureHttp(HTTPClient* http, WiFiClientSecure* client,
                   const char* url, uint32_t timeoutMs = 20000) {
  if (!http || !client || !url) return false;
  // The signed manifest authenticates the exact versioned asset and its SHA.
  // Avoid carrying the complete CA bundle in the 4 MB product image.
  client->setInsecure();
  client->setTimeout(timeoutMs);
  http->setTimeout(timeoutMs);
  http->setConnectTimeout(timeoutMs);
  http->setReuse(false);
  http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http->begin(*client, url)) return false;
  http->addHeader("Accept-Encoding", "identity");
  http->addHeader("User-Agent", "ESP32Radar/0.2");
  return true;
}

bool httpGetMemory(const char* url, uint8_t* buffer, size_t capacity,
                   size_t* outLength) {
  if (!url || !buffer || !outLength) return false;
  *outLength = 0;
  WiFiClientSecure client;
  HTTPClient http;
  if (!configureHttp(&http, &client, url)) return false;
  const int status = http.GET();
  const int declared = http.getSize();
  if (status != HTTP_CODE_OK || declared > static_cast<int>(capacity)) {
    http.end();
    client.stop();
    return false;
  }
  NetworkClient* stream = http.getStreamPtr();
  size_t length = 0;
  uint32_t lastData = millis();
  while (length < capacity && (http.connected() || stream->available())) {
    const size_t available = stream->available();
    if (available == 0) {
      if (millis() - lastData > 20000UL) break;
      delay(2);
      continue;
    }
    const size_t take = min(available, capacity - length);
    const int got = stream->readBytes(buffer + length, take);
    if (got <= 0) break;
    length += static_cast<size_t>(got);
    lastData = millis();
  }
  const bool exact = length > 0 &&
                     (declared < 0 || length == static_cast<size_t>(declared)) &&
                     stream->available() == 0;
  http.end();
  client.stop();
  if (exact) *outLength = length;
  return exact;
}

bool decodeOuterManifest(const uint8_t* json, size_t jsonLength,
                         uint64_t checkedAt, UpdateManifestRecord* out,
                         UpdateError* error) {
  if (!json || !out || !error) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json, jsonLength) != DeserializationError::Ok ||
      doc["schema"].as<int>() != 2 ||
      strcmp(doc["algorithm"] | "", "ecdsa-p256-sha256") != 0 ||
      strcmp(doc["key_id"] | "", "radar-prod-1") != 0) {
    *error = UpdateError::ManifestFormat;
    return false;
  }
  const char* payloadB64 = doc["payload_b64"] | "";
  const char* signatureB64 = doc["signature_b64"] | "";
  memset(out, 0, sizeof(*out));
  size_t payloadLength = 0;
  size_t signatureLength = 0;
  if (mbedtls_base64_decode(out->payload, sizeof(out->payload),
                            &payloadLength,
                            reinterpret_cast<const uint8_t*>(payloadB64),
                            strlen(payloadB64)) != 0 ||
      mbedtls_base64_decode(out->signature, sizeof(out->signature),
                            &signatureLength,
                            reinterpret_cast<const uint8_t*>(signatureB64),
                            strlen(signatureB64)) != 0 ||
      payloadLength > UINT16_MAX || signatureLength > UINT16_MAX) {
    *error = UpdateError::ManifestFormat;
    return false;
  }
  out->payloadLength = static_cast<uint16_t>(payloadLength);
  out->signatureLength = static_cast<uint16_t>(signatureLength);
  if (!updateManifestVerifyAndParse(out->payload, out->payloadLength,
                                    out->signature, out->signatureLength,
                                    &out->manifest)) {
    *error = UpdateError::ManifestSignature;
    return false;
  }
  out->checkedAt = checkedAt;
  *error = UpdateError::None;
  return true;
}

bool fetchManifest(UpdateManifestRecord* out, UpdateError* error) {
  uint8_t* json = static_cast<uint8_t*>(malloc(kManifestHttpMax));
  if (!json) {
    *error = UpdateError::ManifestHttp;
    return false;
  }
  size_t length = 0;
  const bool fetched = httpGetMemory(RADAR_MANIFEST_URL, json,
                                     kManifestHttpMax, &length);
  if (!fetched) {
    free(json);
    *error = UpdateError::ManifestHttp;
    return false;
  }
  const uint64_t now = static_cast<uint64_t>(time(nullptr));
  const bool ok = decodeOuterManifest(json, length, now, out, error);
  free(json);
  return ok;
}

void startTimeSync() {
  if (s_timeStarted) return;
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  s_timeStarted = true;
}

bool waitForValidTime(uint32_t timeoutMs) {
  startTimeSync();
  const uint32_t started = millis();
  while (time(nullptr) < kValidEpoch && millis() - started < timeoutMs) {
    delay(100);
  }
  return time(nullptr) >= kValidEpoch;
}

bool partitionMatches(const esp_partition_t* partition, uint32_t address,
                      uint32_t size) {
  return partition && partition->address == address && partition->size == size;
}

bool layoutCompatible(bool requireFactoryImage) {
  const esp_partition_t* factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory");
  const esp_partition_t* app = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "app0");
  const esp_partition_t* fs = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!partitionMatches(factory, kFactoryOffset, kFactorySize) ||
      !partitionMatches(app, kAppOffset, kAppSize) ||
      !partitionMatches(fs, kFsOffset, kFsSize)) {
    return false;
  }
  if (!requireFactoryImage) return true;
  esp_app_desc_t description{};
  return esp_ota_get_partition_description(factory, &description) == ESP_OK &&
         strcmp(description.version, "recovery-1") == 0;
}

void journalFailure(UpdateJournal* journal, UpdatePhase phase,
                    UpdateError error) {
  if (!journal) return;
  Serial.printf("update failed: phase=%u error=%s attempts=%u\n",
                static_cast<unsigned>(phase), updateErrorName(error),
                static_cast<unsigned>(journal->attempts));
  journal->state = UpdateState::Failed;
  journal->phase = phase;
  journal->error = error;
  journal->updatedAt = static_cast<uint64_t>(time(nullptr));
  updateStoreSaveJournal(journal);
  updateStoreSetLastError(error);
}

bool manifestsMatch(const UpdateManifest& a, const UpdateManifest& b) {
  return a.versionCode == b.versionCode && a.size == b.size &&
         memcmp(a.sha256, b.sha256, sizeof(a.sha256)) == 0;
}

bool preflightAsset(const UpdateManifest& manifest) {
  uint8_t prefix[4096];
  WiFiClientSecure client;
  HTTPClient http;
  if (!configureHttp(&http, &client, manifest.url)) return false;
  static const char* headers[] = {"Content-Range"};
  http.collectHeaders(headers, 1);
  http.addHeader("Range", "bytes=0-4095");
  const int status = http.GET();
  const int contentLength = http.getSize();
  uint64_t total = contentLength > 0 ? static_cast<uint64_t>(contentLength) : 0;
  const String contentRange = http.header("Content-Range");
  const int slash = contentRange.lastIndexOf('/');
  if (slash >= 0) {
    total = strtoull(contentRange.c_str() + slash + 1, nullptr, 10);
  }
  NetworkClient* stream = http.getStreamPtr();
  size_t length = 0;
  const uint32_t deadline = millis() + 20000UL;
  while (length < sizeof(prefix) &&
         static_cast<int32_t>(millis() - deadline) < 0 &&
         (http.connected() || stream->available())) {
    size_t available = stream->available();
    if (available == 0) {
      delay(2);
      continue;
    }
    available = min(available, sizeof(prefix) - length);
    const int got = stream->readBytes(prefix + length, available);
    if (got <= 0) break;
    length += static_cast<size_t>(got);
  }
  http.end();
  client.stop();
  return (status == HTTP_CODE_OK || status == HTTP_CODE_PARTIAL_CONTENT) &&
         length >= 64 && prefix[0] == 0xE9 && total == manifest.size;
}

bool downloadFirmware(const UpdateManifest& manifest, LGFX* lcd,
                      UpdateError* error) {
  LittleFS.remove("/update/firmware.part");
  File file = LittleFS.open("/update/firmware.part", "w");
  if (!file) {
    *error = UpdateError::Download;
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  if (mbedtls_sha256_starts(&sha, 0) != 0) {
    file.close();
    mbedtls_sha256_free(&sha);
    *error = UpdateError::Download;
    return false;
  }
  WiFiClientSecure client;
  HTTPClient http;
  if (!configureHttp(&http, &client, manifest.url)) {
    file.close();
    mbedtls_sha256_free(&sha);
    *error = UpdateError::Download;
    return false;
  }
  const int status = http.GET();
  const int contentLength = http.getSize();
  if (status != HTTP_CODE_OK || contentLength < 0 ||
      static_cast<uint64_t>(contentLength) != manifest.size) {
    http.end();
    client.stop();
    file.close();
    mbedtls_sha256_free(&sha);
    *error = status == HTTP_CODE_OK ? UpdateError::SizeMismatch
                                   : UpdateError::Download;
    return false;
  }

  NetworkClient* stream = http.getStreamPtr();
  uint8_t buffer[4096];
  size_t length = 0;
  int lastPercent = -2;
  uint32_t lastData = millis();
  bool streamOk = true;
  while (length < manifest.size) {
    size_t available = stream->available();
    if (available == 0) {
      if (!http.connected() || millis() - lastData > 20000UL) {
        streamOk = false;
        break;
      }
      delay(2);
      continue;
    }
    available = min(available, sizeof(buffer));
    available = min(available, static_cast<size_t>(manifest.size - length));
    const int got = stream->readBytes(buffer, available);
    if (got <= 0 || file.write(buffer, static_cast<size_t>(got)) !=
                        static_cast<size_t>(got) ||
        mbedtls_sha256_update(&sha, buffer, static_cast<size_t>(got)) != 0) {
      streamOk = false;
      break;
    }
    length += static_cast<size_t>(got);
    lastData = millis();
    const int percent = static_cast<int>((length * 100U) / manifest.size);
    if (percent == 100 || percent - lastPercent >= 2) {
      lastPercent = percent;
      char detail[32];
      snprintf(detail, sizeof(detail), "%d%%  %u/%u KB", percent,
               static_cast<unsigned>(length / 1024U),
               static_cast<unsigned>(manifest.size / 1024U));
      drawUpdateStatus(lcd, "Downloading", detail);
    }
    delay(1);
  }
  http.end();
  client.stop();
  uint8_t digest[32];
  const bool shaOk = mbedtls_sha256_finish(&sha, digest) == 0;
  mbedtls_sha256_free(&sha);
  file.flush();
  file.close();
  if (!streamOk) {
    *error = UpdateError::Download;
    return false;
  }
  if (length != manifest.size) {
    *error = UpdateError::SizeMismatch;
    return false;
  }
  if (!shaOk || memcmp(digest, manifest.sha256, sizeof(digest)) != 0) {
    *error = UpdateError::ShaMismatch;
    return false;
  }
  File verify = LittleFS.open("/update/firmware.part", "r");
  const bool sizeOk = verify && verify.size() == manifest.size;
  uint8_t magic = 0;
  if (verify) {
    verify.read(&magic, 1);
    verify.close();
  }
  if (!sizeOk || magic != 0xE9) {
    *error = UpdateError::SizeMismatch;
    return false;
  }
  LittleFS.remove("/update/firmware.bin");
  if (!LittleFS.rename("/update/firmware.part", "/update/firmware.bin")) {
    *error = UpdateError::Download;
    return false;
  }
  *error = UpdateError::None;
  return true;
}

bool selectFactoryAndRestart(UpdateJournal* journal) {
  const esp_partition_t* factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory");
  if (!factory || esp_ota_set_boot_partition(factory) != ESP_OK) {
    journalFailure(journal, UpdatePhase::Boot, UpdateError::BootSelect);
    return false;
  }
  delay(100);
  ESP.restart();
  return true;
}

bool executeUpdate(LGFX* lcd, AppConfig* cfg, UpdateJournal* journal) {
  if (!lcd || !cfg || !journal) return false;
  ManifestRecordBuffer record;
  if (!record.value || !updateStoreLoadManifest(record.value) ||
      record.value->sequence != journal->manifestSequence ||
      record.value->manifest.versionCode != journal->targetVersionCode) {
    journalFailure(journal, UpdatePhase::Manifest,
                   UpdateError::ManifestChanged);
    return false;
  }
  const UpdateManifest cachedManifest = record.value->manifest;

  s_exclusive = true;
  drawUpdateStatus(lcd, "Update", "Connecting WiFi");
  if (!wifiConnect(cfg)) {
    journalFailure(journal, UpdatePhase::Preflight,
                   UpdateError::AssetPreflight);
    s_exclusive = false;
    return false;
  }
  drawUpdateStatus(lcd, "Update", "Checking package");
  if (!waitForValidTime(15000)) {
    journalFailure(journal, UpdatePhase::TimeSync, UpdateError::TimeSync);
    s_exclusive = false;
    return false;
  }
  UpdateError error = UpdateError::None;
  if (!fetchManifest(record.value, &error)) {
    journalFailure(journal, UpdatePhase::Manifest, error);
    s_exclusive = false;
    return false;
  }
  if (!manifestsMatch(cachedManifest, record.value->manifest)) {
    updateStoreSaveManifest(record.value);
    journalFailure(journal, UpdatePhase::Manifest,
                   UpdateError::ManifestChanged);
    s_exclusive = false;
    return false;
  }
  if (!layoutCompatible(true) ||
      !updateManifestIsInstallable(record.value->manifest, RADAR_VERSION_CODE,
                                   LittleFS.totalBytes())) {
    journalFailure(journal, UpdatePhase::Preflight,
                   UpdateError::FactoryIncompatible);
    s_exclusive = false;
    return false;
  }
  if (!preflightAsset(record.value->manifest)) {
    journalFailure(journal, UpdatePhase::Preflight,
                   UpdateError::AssetPreflight);
    s_exclusive = false;
    return false;
  }

  drawUpdateStatus(lcd, "Update", "Clearing cache");
  LittleFS.end();
  if (!LittleFS.format() || !LittleFS.begin(false)) {
    journalFailure(journal, UpdatePhase::Filesystem,
                   UpdateError::FilesystemFormat);
    s_exclusive = false;
    s_filesystemReset = true;
    return false;
  }
  s_filesystemReset = true;
  if (!LittleFS.mkdir("/update") && !LittleFS.exists("/update")) {
    journalFailure(journal, UpdatePhase::Filesystem,
                   UpdateError::FilesystemFormat);
    s_exclusive = false;
    return false;
  }
  journal->state = UpdateState::Downloading;
  journal->phase = UpdatePhase::Download;
  journal->error = UpdateError::None;
  journal->updatedAt = static_cast<uint64_t>(time(nullptr));
  updateStoreSaveJournal(journal);

  bool downloaded = false;
  for (uint8_t attempt = 1; attempt <= 3 && !downloaded; ++attempt) {
    journal->attempts = attempt;
    updateStoreSaveJournal(journal);
    downloaded = downloadFirmware(record.value->manifest, lcd, &error);
    if (!downloaded) {
      LittleFS.remove("/update/firmware.part");
      if (attempt < 3) delay(attempt == 1 ? 5000 : 30000);
    }
  }
  if (!downloaded) {
    journalFailure(journal, UpdatePhase::Download, error);
    s_exclusive = false;
    return false;
  }

  journal->state = UpdateState::Staged;
  journal->phase = UpdatePhase::Factory;
  journal->error = UpdateError::None;
  journal->attempts = 0;
  journal->updatedAt = static_cast<uint64_t>(time(nullptr));
  if (!updateStoreSaveJournal(journal)) {
    journalFailure(journal, UpdatePhase::Factory, UpdateError::StoreCorrupt);
    s_exclusive = false;
    return false;
  }
  drawUpdateStatus(lcd, "Package ready", "Starting installer");
  return selectFactoryAndRestart(journal);
}

void dailyCheckTask(void*) {
  ManifestRecordBuffer record;
  UpdateError result = UpdateError::None;
  if (!record.value || !fetchManifest(record.value, &result)) {
    updateStoreSetLastError(result);
  } else if (!updateStoreSaveManifest(record.value)) {
    result = UpdateError::StoreCorrupt;
    updateStoreSetLastError(result);
  } else {
    updateStoreSetTime("last_success", record.value->checkedAt);
    updateStoreSetLastError(UpdateError::None);
  }
  s_dailyResult = result;
  s_dailyDone = true;
  free(record.value);
  record.value = nullptr;
  vTaskDelete(nullptr);
}

bool filesystemProbe() {
  File probe = LittleFS.open("/update-selftest.tmp", "w");
  if (!probe) return false;
  const uint32_t marker = 0x52555031U;
  const bool wrote = probe.write(reinterpret_cast<const uint8_t*>(&marker),
                                 sizeof(marker)) == sizeof(marker);
  probe.flush();
  probe.close();
  uint32_t readback = 0;
  probe = LittleFS.open("/update-selftest.tmp", "r");
  const bool read = probe &&
                    probe.read(reinterpret_cast<uint8_t*>(&readback),
                               sizeof(readback)) == sizeof(readback);
  if (probe) probe.close();
  LittleFS.remove("/update-selftest.tmp");
  return wrote && read && readback == marker;
}

}  // namespace

void updateManagerBegin() {
  updateStoreBegin();
  s_nonce = esp_random();
  if (s_nonce == 0) s_nonce = 1;
}

void updateManagerGetPortalInfo(UpdatePortalInfo* out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  strncpy(out->currentVersion, RADAR_VERSION, sizeof(out->currentVersion) - 1);
  strncpy(out->currentBuild, RADAR_BUILD_SHA, sizeof(out->currentBuild) - 1);
  out->nonce = s_nonce;
  out->factoryCompatible = layoutCompatible(true);
  updateStoreGetTime("last_attempt", &out->lastAttempt);
  updateStoreGetTime("last_success", &out->lastSuccess);
  updateStoreGetLastError(&out->lastError);
  if (out->lastError != UpdateError::None) out->lastErrorAt = out->lastAttempt;
  UpdateJournal journal{};
  if (updateStoreLoadJournal(&journal) && journal.state == UpdateState::Failed &&
      journal.error != UpdateError::None) {
    out->lastError = journal.error;
    out->lastErrorAt = journal.updatedAt;
  }
  ManifestRecordBuffer record;
  if (record.value && updateStoreLoadManifest(record.value)) {
    out->haveManifest = true;
    out->manifest = record.value->manifest;
    out->available = out->factoryCompatible &&
                     updateManifestIsInstallable(record.value->manifest,
                                                 RADAR_VERSION_CODE,
                                                 kFsSize);
  }
}

bool updateManagerRequestFromPortal(uint32_t versionCode, uint32_t nonce) {
  if (nonce != s_nonce || nonce == 0 || !layoutCompatible(true)) return false;
  ManifestRecordBuffer record;
  if (!record.value || !updateStoreLoadManifest(record.value) ||
      record.value->manifest.versionCode != versionCode ||
      !updateManifestIsInstallable(record.value->manifest, RADAR_VERSION_CODE,
                                   kFsSize)) {
    return false;
  }
  UpdateJournal journal{};
  journal.state = UpdateState::Requested;
  journal.phase = UpdatePhase::Manifest;
  journal.targetVersionCode = record.value->manifest.versionCode;
  journal.expectedSize = record.value->manifest.size;
  memcpy(journal.expectedSha256, record.value->manifest.sha256,
         sizeof(journal.expectedSha256));
  journal.manifestSequence = record.value->sequence;
  journal.requestedAt = static_cast<uint64_t>(time(nullptr));
  journal.updatedAt = journal.requestedAt;
  return updateStoreSaveJournal(&journal);
}

bool updateManagerHandleBootResume(LGFX* lcd, AppConfig* cfg) {
  UpdateJournal journal{};
  if (!updateStoreLoadJournal(&journal)) return false;
  if (journal.state == UpdateState::Staged) {
    drawUpdateStatus(lcd, "Package ready", "Starting installer");
    selectFactoryAndRestart(&journal);
    return true;
  }
  if (journal.state != UpdateState::Requested &&
      journal.state != UpdateState::Downloading) {
    return false;
  }
  for (int remain = 5; remain > 0; --remain) {
    char detail[48];
    snprintf(detail, sizeof(detail), "Resume in %ds - press S to cancel", remain);
    drawUpdateStatus(lcd, "Pending update", detail);
    const uint32_t until = millis() + 1000U;
    while (static_cast<int32_t>(millis() - until) < 0) {
      const ButtonEvent event = buttonPoll();
      if (event != ButtonEvent::None || buttonIsDown()) {
        journalFailure(&journal, UpdatePhase::Download, UpdateError::Cancelled);
        return false;
      }
      delay(10);
    }
  }
  executeUpdate(lcd, cfg, &journal);
  return true;
}

bool updateManagerExecuteConfirmed(LGFX* lcd, AppConfig* cfg) {
  UpdateJournal journal{};
  if (!updateStoreLoadJournal(&journal) ||
      journal.state != UpdateState::Requested) {
    return false;
  }
  return executeUpdate(lcd, cfg, &journal);
}

bool updateManagerConfirmFirstBoot(LGFX* lcd, bool filesystemReady,
                                   bool workerReady) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t otaState = ESP_OTA_IMG_UNDEFINED;
  const bool haveState = running &&
                         esp_ota_get_state_partition(running, &otaState) ==
                             ESP_OK;
  UpdateJournal journal{};
  const bool haveJournal = updateStoreLoadJournal(&journal);

  if ((!haveState || otaState != ESP_OTA_IMG_PENDING_VERIFY) &&
      !(haveJournal && journal.state == UpdateState::FirstBoot)) {
    return true;
  }
  if (haveState && otaState != ESP_OTA_IMG_PENDING_VERIFY && haveJournal &&
      journal.state == UpdateState::FirstBoot) {
    journal.state = UpdateState::Confirmed;
    journal.phase = UpdatePhase::None;
    journal.error = UpdateError::None;
    updateStoreSaveJournal(&journal);
    LittleFS.remove("/update/firmware.bin");
    LittleFS.rmdir("/update");
    return true;
  }

  if (!haveJournal) memset(&journal, 0, sizeof(journal));
  journal.state = UpdateState::FirstBoot;
  journal.phase = UpdatePhase::SelfTest;
  journal.error = UpdateError::None;
  journal.updatedAt = static_cast<uint64_t>(time(nullptr));
  if (journal.targetVersionCode == 0) journal.targetVersionCode = RADAR_VERSION_CODE;
  updateStoreSaveJournal(&journal);
  drawUpdateStatus(lcd, "Verifying update", "Hardware self-test");

  const bool partitionOk =
      running && running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
      partitionMatches(running, kAppOffset, kAppSize) && layoutCompatible(true);
  const bool versionOk = journal.targetVersionCode == RADAR_VERSION_CODE;
  const bool heapOk = ESP.getFreeHeap() >= 64U * 1024U &&
                      ESP.getMaxAllocHeap() >= 32U * 1024U;
  const bool storageOk = filesystemReady && filesystemProbe() &&
                         updateStoreProbe();
  if (!partitionOk || !versionOk || !heapOk || !storageOk || !workerReady ||
      esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
    journalFailure(&journal, UpdatePhase::SelfTest, UpdateError::SelfTest);
    drawUpdateStatus(lcd, "Update failed", "Returning to installer");
    delay(500);
    selectFactoryAndRestart(&journal);
    return false;
  }

  journal.state = UpdateState::Confirmed;
  journal.phase = UpdatePhase::None;
  journal.error = UpdateError::None;
  journal.updatedAt = static_cast<uint64_t>(time(nullptr));
  updateStoreSaveJournal(&journal);
  updateStoreSetLastError(UpdateError::None);
  LittleFS.remove("/update/firmware.bin");
  LittleFS.rmdir("/update");
  drawUpdateStatus(lcd, "Update verified", RADAR_VERSION);
  delay(700);
  return true;
}

void updateManagerServiceDailyCheck(bool canStart) {
  if (s_dailyDone) {
    Serial.printf("daily update check: %s\n",
                  updateErrorName(static_cast<UpdateError>(s_dailyResult)));
    s_dailyDone = false;
    s_dailyActive = false;
  }
  if (!canStart || WiFi.status() != WL_CONNECTED) {
    s_dailyIdleSince = 0;
    return;
  }
  if (s_dailyActive) return;
  if (s_dailyIdleSince == 0) {
    s_dailyIdleSince = millis() == 0 ? 1 : millis();
    return;
  }
  if (millis() - s_dailyIdleSince < 60000UL) return;
  startTimeSync();
  const time_t now = time(nullptr);
  if (now < kValidEpoch) return;
  uint64_t lastAttempt = 0;
  if (updateStoreGetTime("last_attempt", &lastAttempt) &&
      static_cast<uint64_t>(now) < lastAttempt + kDaySeconds) {
    return;
  }
  if (!updateStoreSetTime("last_attempt", static_cast<uint64_t>(now))) return;
  s_dailyIdleSince = 0;
  s_dailyActive = true;
  s_dailyDone = false;
  TaskHandle_t task = nullptr;
  if (xTaskCreate(dailyCheckTask, "update-check", 12288, nullptr,
                  tskIDLE_PRIORITY, &task) != pdPASS) {
    s_dailyActive = false;
    updateStoreSetLastError(UpdateError::ManifestHttp);
  }
}

bool updateManagerBusy() { return s_exclusive || s_dailyActive; }

bool updateManagerConsumeFilesystemReset() {
  const bool result = s_filesystemReset;
  s_filesystemReset = false;
  return result;
}
