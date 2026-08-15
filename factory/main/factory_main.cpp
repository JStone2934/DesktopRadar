#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_app_desc.h>
#include <esp_littlefs.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "update_protocol.h"
#include "update_store.h"

namespace {

constexpr char kTag[] = "radar-recovery";
constexpr gpio_num_t kReset = GPIO_NUM_0;
constexpr gpio_num_t kChipSelect = GPIO_NUM_1;
constexpr gpio_num_t kDataCommand = GPIO_NUM_2;
constexpr gpio_num_t kMosi = GPIO_NUM_3;
constexpr gpio_num_t kClock = GPIO_NUM_4;
constexpr gpio_num_t kButton = GPIO_NUM_9;
constexpr size_t kIoBlock = 4096;
constexpr char kPackagePath[] = "/littlefs/update/firmware.bin";

spi_device_handle_t s_lcd = nullptr;
bool s_littlefsMounted = false;

// 5x7 glyphs: A-Z, 0-9, then space, %, -, /, ., :.
constexpr uint8_t kGlyphs[][5] = {
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x00,0x00,0x00,0x00},{0x63,0x13,0x08,0x64,0x63},
    {0x08,0x08,0x08,0x08,0x08},{0x20,0x10,0x08,0x04,0x02},
    {0x00,0x60,0x60,0x00,0x00},{0x00,0x36,0x36,0x00,0x00},
};

const uint8_t* glyph(char ch) {
  if (ch >= 'a' && ch <= 'z') ch -= ('a' - 'A');
  if (ch >= 'A' && ch <= 'Z') return kGlyphs[ch - 'A'];
  if (ch >= '0' && ch <= '9') return kGlyphs[26 + ch - '0'];
  switch (ch) {
    case ' ': return kGlyphs[36];
    case '%': return kGlyphs[37];
    case '-': return kGlyphs[38];
    case '/': return kGlyphs[39];
    case '.': return kGlyphs[40];
    case ':': return kGlyphs[41];
    default: return kGlyphs[36];
  }
}

void lcdTransfer(bool data, const void* bytes, size_t length) {
  if (!s_lcd || !bytes || length == 0) return;
  gpio_set_level(kDataCommand, data ? 1 : 0);
  spi_transaction_t transaction{};
  transaction.length = length * 8;
  transaction.tx_buffer = bytes;
  ESP_ERROR_CHECK(spi_device_polling_transmit(s_lcd, &transaction));
}

void lcdCommand(uint8_t command, const uint8_t* data = nullptr,
                size_t length = 0) {
  lcdTransfer(false, &command, 1);
  if (data && length) lcdTransfer(true, data, length);
}

void lcdWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  const uint16_t x2 = x + w - 1;
  const uint16_t y2 = y + h - 1;
  const uint8_t columns[] = {static_cast<uint8_t>(x >> 8),
                             static_cast<uint8_t>(x),
                             static_cast<uint8_t>(x2 >> 8),
                             static_cast<uint8_t>(x2)};
  const uint8_t rows[] = {static_cast<uint8_t>(y >> 8),
                          static_cast<uint8_t>(y),
                          static_cast<uint8_t>(y2 >> 8),
                          static_cast<uint8_t>(y2)};
  lcdCommand(0x2A, columns, sizeof(columns));
  lcdCommand(0x2B, rows, sizeof(rows));
  lcdCommand(0x2C);
}

void lcdFill(uint16_t color) {
  uint16_t pixels[240];
  const uint16_t wire = static_cast<uint16_t>((color >> 8) | (color << 8));
  for (auto& pixel : pixels) pixel = wire;
  lcdWindow(0, 0, 240, 240);
  for (int row = 0; row < 240; ++row) {
    lcdTransfer(true, pixels, sizeof(pixels));
  }
}

void lcdText(const char* text, int y, uint16_t color = 0xFFFF,
             int scale = 2) {
  if (!text) return;
  const size_t count = strlen(text);
  const int width = static_cast<int>(count) * 6 * scale;
  int x0 = (240 - width) / 2;
  if (x0 < 0) x0 = 0;
  const uint16_t wire = static_cast<uint16_t>((color >> 8) | (color << 8));
  uint16_t block[3 * 3];
  for (auto& pixel : block) pixel = wire;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* bitmap = glyph(text[i]);
    for (int column = 0; column < 5; ++column) {
      for (int row = 0; row < 7; ++row) {
        if ((bitmap[column] & (1U << row)) == 0) continue;
        const int x = x0 + static_cast<int>(i) * 6 * scale + column * scale;
        lcdWindow(x, y + row * scale, scale, scale);
        lcdTransfer(true, block, scale * scale * sizeof(uint16_t));
      }
    }
  }
}

void show(const char* line1, const char* line2 = nullptr,
          const char* line3 = nullptr, uint16_t color = 0xFFFF) {
  ESP_LOGI(kTag, "%s%s%s%s%s", line1 ? line1 : "",
           line2 ? " / " : "", line2 ? line2 : "",
           line3 ? " / " : "", line3 ? line3 : "");
  lcdFill(0x0000);
  if (line1) lcdText(line1, line2 ? 82 : 108, color, 2);
  if (line2) lcdText(line2, 108, color, 2);
  if (line3) lcdText(line3, 134, color, 2);
}

void lcdBegin() {
  gpio_config_t output{};
  output.pin_bit_mask = (1ULL << kReset) | (1ULL << kDataCommand);
  output.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&output));
  gpio_set_level(kReset, 0);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(kReset, 1);
  vTaskDelay(pdMS_TO_TICKS(120));

  spi_bus_config_t bus{};
  bus.mosi_io_num = kMosi;
  bus.miso_io_num = -1;
  bus.sclk_io_num = kClock;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 480;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
  spi_device_interface_config_t device{};
  device.clock_speed_hz = 40000000;
  device.mode = 0;
  device.spics_io_num = kChipSelect;
  device.queue_size = 1;
  ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device, &s_lcd));

  lcdCommand(0xEF);
  const uint8_t eb[] = {0x14}; lcdCommand(0xEB, eb, sizeof(eb));
  lcdCommand(0xFE); lcdCommand(0xEF);
  const uint8_t b6[] = {0x00,0x20}; lcdCommand(0xB6,b6,sizeof(b6));
  const uint8_t madctl[] = {0x08}; lcdCommand(0x36,madctl,sizeof(madctl));
  const uint8_t pixfmt[] = {0x05}; lcdCommand(0x3A,pixfmt,sizeof(pixfmt));
  const uint8_t c3[] = {0x13}; lcdCommand(0xC3,c3,sizeof(c3));
  const uint8_t c4[] = {0x13}; lcdCommand(0xC4,c4,sizeof(c4));
  const uint8_t c9[] = {0x22}; lcdCommand(0xC9,c9,sizeof(c9));
  const uint8_t f0[] = {0x45,0x09,0x08,0x08,0x26,0x2A};
  const uint8_t f1[] = {0x43,0x70,0x72,0x36,0x37,0x6F};
  const uint8_t f2[] = {0x45,0x09,0x08,0x08,0x26,0x2A};
  const uint8_t f3[] = {0x43,0x70,0x72,0x36,0x37,0x6F};
  lcdCommand(0xF0,f0,sizeof(f0)); lcdCommand(0xF1,f1,sizeof(f1));
  lcdCommand(0xF2,f2,sizeof(f2)); lcdCommand(0xF3,f3,sizeof(f3));
  lcdCommand(0x21);
  lcdCommand(0x11);
  vTaskDelay(pdMS_TO_TICKS(120));
  lcdCommand(0x29);
  vTaskDelay(pdMS_TO_TICKS(20));
  lcdFill(0x0000);
}

void journalFailure(UpdateJournal* journal, UpdatePhase phase,
                    UpdateError error) {
  if (!journal) return;
  journal->state = UpdateState::Failed;
  journal->phase = phase;
  journal->error = error;
  journal->updatedAt = 0;
  updateStoreSaveJournal(journal);
  updateStoreSetLastError(error);
}

bool mountPackageFilesystem() {
  esp_vfs_littlefs_conf_t config{};
  config.base_path = "/littlefs";
  config.partition_label = "spiffs";
  config.format_if_mount_failed = false;
  config.dont_mount = false;
  const esp_err_t result = esp_vfs_littlefs_register(&config);
  s_littlefsMounted = result == ESP_OK;
  return s_littlefsMounted;
}

bool hashFile(const char* path, uint8_t digest[32], size_t* outSize) {
  FILE* file = fopen(path, "rb");
  if (!file) return false;
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  bool ok = mbedtls_sha256_starts(&sha, 0) == 0;
  uint8_t buffer[kIoBlock];
  size_t total = 0;
  while (ok) {
    const size_t count = fread(buffer, 1, sizeof(buffer), file);
    if (count > 0) {
      total += count;
      ok = mbedtls_sha256_update(&sha, buffer, count) == 0;
    }
    if (count < sizeof(buffer)) {
      if (ferror(file)) ok = false;
      break;
    }
    vTaskDelay(1);
  }
  if (ok) ok = mbedtls_sha256_finish(&sha, digest) == 0;
  mbedtls_sha256_free(&sha);
  fclose(file);
  if (ok && outSize) *outSize = total;
  return ok;
}

bool packageValid(const UpdateJournal& journal,
                  UpdateManifestRecord* outManifest) {
  if (!s_littlefsMounted || !outManifest ||
      !updateStoreLoadManifest(outManifest) ||
      outManifest->sequence != journal.manifestSequence ||
      !updateManifestVerifyAndParse(outManifest->payload,
                                    outManifest->payloadLength,
                                    outManifest->signature,
                                    outManifest->signatureLength,
                                    &outManifest->manifest) ||
      outManifest->manifest.versionCode != journal.targetVersionCode ||
      outManifest->manifest.versionCode == 0 ||
      outManifest->manifest.minRecovery > RADAR_RECOVERY_API ||
      outManifest->manifest.size > RADAR_MAIN_MAX_BYTES ||
      outManifest->manifest.size != journal.expectedSize ||
      memcmp(outManifest->manifest.sha256, journal.expectedSha256, 32) != 0) {
    return false;
  }
  uint8_t digest[32];
  size_t size = 0;
  return hashFile(kPackagePath, digest, &size) && size == journal.expectedSize &&
         memcmp(digest, journal.expectedSha256, sizeof(digest)) == 0;
}

bool appPartitionValid(const esp_partition_t* app) {
  esp_app_desc_t description{};
  return app && esp_ota_get_partition_description(app, &description) == ESP_OK;
}

bool selectApp(const esp_partition_t* app, UpdateJournal* journal) {
  if (esp_ota_set_boot_partition(app) != ESP_OK) {
    if (journal) journalFailure(journal, UpdatePhase::Boot,
                                UpdateError::BootSelect);
    return false;
  }
  show("STARTING APP");
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
  return true;
}

UpdateError installOnce(UpdateJournal* journal,
                        const UpdateManifestRecord& manifest,
                        const esp_partition_t* app) {
  FILE* file = fopen(kPackagePath, "rb");
  if (!file) return UpdateError::ShaMismatch;
  journal->state = UpdateState::Installing;
  journal->phase = UpdatePhase::Install;
  journal->error = UpdateError::None;
  ++journal->attempts;
  updateStoreSaveJournal(journal);

  esp_ota_handle_t handle = 0;
  esp_err_t result = esp_ota_begin(app, manifest.manifest.size, &handle);
  if (result != ESP_OK) {
    fclose(file);
    return UpdateError::OtaBegin;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  bool ok = mbedtls_sha256_starts(&sha, 0) == 0;
  uint8_t buffer[kIoBlock];
  size_t total = 0;
  int shown = -10;
  while (ok && total < manifest.manifest.size) {
    const size_t want = manifest.manifest.size - total < sizeof(buffer)
                            ? manifest.manifest.size - total : sizeof(buffer);
    const size_t count = fread(buffer, 1, want, file);
    if (count != want || mbedtls_sha256_update(&sha, buffer, count) != 0) {
      ok = false;
      break;
    }
    result = esp_ota_write(handle, buffer, count);
    if (result != ESP_OK) {
      ok = false;
      break;
    }
    total += count;
    const int percent = static_cast<int>(total * 100U / manifest.manifest.size);
    if (percent == 100 || percent - shown >= 5) {
      shown = percent;
      char progress[20];
      snprintf(progress, sizeof(progress), "%d%%", percent);
      show("INSTALLING", progress);
    }
    vTaskDelay(1);
  }
  fclose(file);
  uint8_t digest[32];
  const bool digestOk = ok && mbedtls_sha256_finish(&sha, digest) == 0 &&
                        total == manifest.manifest.size &&
                        memcmp(digest, manifest.manifest.sha256, 32) == 0;
  mbedtls_sha256_free(&sha);
  if (!digestOk) {
    esp_ota_abort(handle);
    return result == ESP_OK ? UpdateError::ShaMismatch : UpdateError::OtaWrite;
  }
  if (esp_ota_end(handle) != ESP_OK) return UpdateError::OtaEnd;

  show("VERIFY FLASH");
  mbedtls_sha256_init(&sha);
  ok = mbedtls_sha256_starts(&sha, 0) == 0;
  total = 0;
  while (ok && total < manifest.manifest.size) {
    const size_t count = manifest.manifest.size - total < sizeof(buffer)
                             ? manifest.manifest.size - total : sizeof(buffer);
    if (esp_partition_read(app, total, buffer, count) != ESP_OK ||
        mbedtls_sha256_update(&sha, buffer, count) != 0) {
      ok = false;
      break;
    }
    total += count;
    vTaskDelay(1);
  }
  ok = ok && mbedtls_sha256_finish(&sha, digest) == 0 &&
       memcmp(digest, manifest.manifest.sha256, 32) == 0;
  mbedtls_sha256_free(&sha);
  if (!ok) return UpdateError::ShaMismatch;
  return UpdateError::None;
}

void waitForManualRetry(UpdateJournal* journal) {
  char code[24];
  snprintf(code, sizeof(code), "ERROR %s", updateErrorName(journal->error));
  show("UPDATE FAILED", code, "S RETRY / USB", 0xF800);
  bool wasDown = false;
  while (true) {
    const bool down = gpio_get_level(kButton) == 0;
    if (wasDown && !down) {
      journal->state = UpdateState::Staged;
      journal->attempts = 0;
      journal->error = UpdateError::None;
      updateStoreSaveJournal(journal);
      esp_restart();
    }
    wasDown = down;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

extern "C" void app_main(void) {
  gpio_config_t button{};
  button.pin_bit_mask = 1ULL << kButton;
  button.mode = GPIO_MODE_INPUT;
  button.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&button);
  lcdBegin();
  show("RECOVERY 1");

  if (!updateStoreBegin()) {
    show("NVS ERROR", "USB RECOVERY", nullptr, 0xF800);
    vTaskDelete(nullptr);
    return;
  }
  const esp_partition_t* app = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, "app0");
  const esp_partition_t* factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory");
  const esp_partition_t* fs = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!app || app->address != 0x90000 || app->size != 0x1B0000 ||
      !factory || factory->address != 0x10000 || factory->size != 0x80000 ||
      !fs || fs->address != 0x240000 || fs->size != 0x1B0000) {
    show("LAYOUT ERROR", "USB RECOVERY", nullptr, 0xF800);
    vTaskDelete(nullptr);
    return;
  }

  esp_ota_img_states_t appState = ESP_OTA_IMG_UNDEFINED;
  const bool haveAppState = esp_ota_get_state_partition(app, &appState) == ESP_OK;
  UpdateJournal journal{};
  if (!updateStoreLoadJournal(&journal)) {
    // First full USB installation: never erase app0; only boot it if its image
    // is valid and otadata has never assigned it a rollback state. A missing
    // journal beside INVALID/VALID state is treated as corruption, not as a
    // reason to launch or rewrite an untrusted app.
    if ((!haveAppState || appState == ESP_OTA_IMG_UNDEFINED) &&
        appPartitionValid(app)) {
      selectApp(app, nullptr);
    }
    show("JOURNAL ERROR", "USB RECOVERY", nullptr, 0xF800);
    vTaskDelete(nullptr);
    return;
  }

  if (journal.state == UpdateState::BootPending) {
    if (haveAppState && (appState == ESP_OTA_IMG_INVALID ||
                         appState == ESP_OTA_IMG_ABORTED)) {
      journalFailure(&journal, UpdatePhase::SelfTest, UpdateError::SelfTest);
      waitForManualRetry(&journal);
    }
    selectApp(app, &journal);
  }
  if (journal.state == UpdateState::FirstBoot) {
    journalFailure(&journal, UpdatePhase::SelfTest, UpdateError::SelfTest);
  }
  if (journal.state == UpdateState::Confirmed && appPartitionValid(app)) {
    selectApp(app, &journal);
  }
  if (journal.state == UpdateState::Failed) {
    mountPackageFilesystem();
    waitForManualRetry(&journal);
  }
  if (journal.state != UpdateState::Staged &&
      journal.state != UpdateState::Installing) {
    show("NO STAGED PKG", "USB RECOVERY", nullptr, 0xF800);
    vTaskDelete(nullptr);
    return;
  }
  if (journal.state == UpdateState::Installing &&
      journal.error == UpdateError::None) {
    journal.error = UpdateError::OtaWrite;
    updateStoreSaveJournal(&journal);
  }
  if (!mountPackageFilesystem()) {
    journalFailure(&journal, UpdatePhase::Factory,
                   UpdateError::FilesystemFormat);
    waitForManualRetry(&journal);
  }
  show("VERIFY PACKAGE");
  UpdateManifestRecord manifest{};
  if (!packageValid(journal, &manifest)) {
    journalFailure(&journal, UpdatePhase::Factory, UpdateError::ShaMismatch);
    waitForManualRetry(&journal);
  }

  while (journal.attempts < 3) {
    const UpdateError result = installOnce(&journal, manifest, app);
    if (result == UpdateError::None) {
      journal.state = UpdateState::BootPending;
      journal.phase = UpdatePhase::Boot;
      journal.error = UpdateError::None;
      updateStoreSaveJournal(&journal);
      selectApp(app, &journal);
    }
    ESP_LOGE(kTag, "install attempt %u failed: %s", journal.attempts,
             updateErrorName(result));
    journal.error = result;
    updateStoreSaveJournal(&journal);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  journalFailure(&journal, UpdatePhase::Install, journal.error);
  waitForManualRetry(&journal);
}
