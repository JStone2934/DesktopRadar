#include "update_protocol.h"

#include <ctype.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "update_public_key.h"

namespace {

template <typename T>
uint32_t recordCrc(const T& record) {
  return updateCrc32(&record, offsetof(T, crc32));
}

bool copyValue(char* out, size_t capacity, const char* value) {
  if (!out || !value || capacity == 0) return false;
  const size_t n = strlen(value);
  if (n == 0 || n >= capacity) return false;
  memcpy(out, value, n + 1);
  return true;
}

bool parseU32(const char* value, uint32_t* out) {
  if (!value || !value[0] || !out) return false;
  for (const char* p = value; *p; ++p) {
    if (!isdigit(static_cast<unsigned char>(*p))) return false;
  }
  char* end = nullptr;
  const unsigned long parsed = strtoul(value, &end, 10);
  if (!end || *end != '\0' || parsed > UINT32_MAX) return false;
  *out = static_cast<uint32_t>(parsed);
  return true;
}

bool parseU64(const char* value, uint64_t* out) {
  if (!value || !value[0] || !out) return false;
  for (const char* p = value; *p; ++p) {
    if (!isdigit(static_cast<unsigned char>(*p))) return false;
  }
  char* end = nullptr;
  const unsigned long long parsed = strtoull(value, &end, 10);
  if (!end || *end != '\0') return false;
  *out = static_cast<uint64_t>(parsed);
  return true;
}

bool parseHex(const char* value, uint8_t* out, size_t outLength) {
  if (!value || !out || strlen(value) != outLength * 2U) return false;
  for (size_t i = 0; i < outLength; ++i) {
    const char pair[3] = {value[i * 2], value[i * 2 + 1], '\0'};
    if (!isxdigit(static_cast<unsigned char>(pair[0])) ||
        !isxdigit(static_cast<unsigned char>(pair[1]))) {
      return false;
    }
    out[i] = static_cast<uint8_t>(strtoul(pair, nullptr, 16));
  }
  return true;
}

bool isHexString(const char* value, size_t length) {
  if (!value || strlen(value) != length) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

bool takeExpectedLine(char** cursor, const char* expectedKey,
                      const char** value) {
  if (!cursor || !*cursor || !expectedKey || !value) return false;
  char* line = *cursor;
  char* newline = strchr(line, '\n');
  if (!newline) return false;
  *newline = '\0';
  if (strchr(line, '\r')) return false;
  const size_t keyLength = strlen(expectedKey);
  if (strncmp(line, expectedKey, keyLength) != 0 || line[keyLength] != '=') {
    return false;
  }
  *value = line + keyLength + 1;
  *cursor = newline + 1;
  return true;
}

bool verifySignature(const uint8_t* payload, size_t payloadLength,
                     const uint8_t* signature, size_t signatureLength) {
  uint8_t digest[32];
  if (mbedtls_sha256(payload, payloadLength, digest, 0) != 0) return false;

  mbedtls_ecdsa_context context;
  mbedtls_ecdsa_init(&context);
  int rc = mbedtls_ecp_group_load(&context.MBEDTLS_PRIVATE(grp),
                                  MBEDTLS_ECP_DP_SECP256R1);
  if (rc == 0) {
    rc = mbedtls_ecp_point_read_binary(&context.MBEDTLS_PRIVATE(grp),
                                       &context.MBEDTLS_PRIVATE(Q),
                                       RADAR_UPDATE_PUBLIC_KEY,
                                       sizeof(RADAR_UPDATE_PUBLIC_KEY));
  }
  if (rc == 0) {
    rc = mbedtls_ecdsa_read_signature(&context, digest, sizeof(digest),
                                      signature, signatureLength);
  }
  mbedtls_ecdsa_free(&context);
  return rc == 0;
}

}  // namespace

uint32_t updateCrc32(const void* data, size_t length) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

bool updateManifestRecordValid(const UpdateManifestRecord& record) {
  return record.magic == UPDATE_MANIFEST_MAGIC &&
         record.schema == UPDATE_RECORD_SCHEMA &&
         record.payloadLength > 0 &&
         record.payloadLength <= UPDATE_PAYLOAD_MAX &&
         record.signatureLength > 0 &&
         record.signatureLength <= UPDATE_SIGNATURE_MAX &&
         record.crc32 == recordCrc(record);
}

bool updateJournalValid(const UpdateJournal& journal) {
  return journal.magic == UPDATE_JOURNAL_MAGIC &&
         journal.schema == UPDATE_RECORD_SCHEMA &&
         journal.crc32 == recordCrc(journal);
}

bool updateManifestVerifyAndParse(const uint8_t* payload, size_t payloadLength,
                                  const uint8_t* signature,
                                  size_t signatureLength,
                                  UpdateManifest* out) {
  if (!payload || !signature || !out || payloadLength == 0 ||
      payloadLength > UPDATE_PAYLOAD_MAX || signatureLength == 0 ||
      signatureLength > UPDATE_SIGNATURE_MAX || payload[payloadLength - 1] != '\n' ||
      memchr(payload, '\0', payloadLength) != nullptr ||
      !verifySignature(payload, payloadLength, signature, signatureLength)) {
    return false;
  }

  char text[UPDATE_PAYLOAD_MAX + 1];
  memcpy(text, payload, payloadLength);
  text[payloadLength] = '\0';
  char* cursor = text;
  const char* value = nullptr;
  UpdateManifest parsed{};

#define TAKE(key) takeExpectedLine(&cursor, key, &value)
  if (!TAKE("manifest") || strcmp(value, "radar-update-v1") != 0 ||
      !TAKE("key_id") || !copyValue(parsed.keyId, sizeof(parsed.keyId), value) ||
      !TAKE("channel") || !copyValue(parsed.channel, sizeof(parsed.channel), value) ||
      !TAKE("version") || !copyValue(parsed.version, sizeof(parsed.version), value) ||
      !TAKE("version_code") || !parseU32(value, &parsed.versionCode) ||
      !TAKE("tag") || !copyValue(parsed.tag, sizeof(parsed.tag), value) ||
      !TAKE("build_sha") || !isHexString(value, 40) ||
      !copyValue(parsed.buildSha, sizeof(parsed.buildSha), value) ||
      !TAKE("chip") || !copyValue(parsed.chip, sizeof(parsed.chip), value) ||
      !TAKE("layout") || !copyValue(parsed.layout, sizeof(parsed.layout), value) ||
      !TAKE("size") || !parseU32(value, &parsed.size) ||
      !TAKE("sha256") || !parseHex(value, parsed.sha256, sizeof(parsed.sha256)) ||
      !TAKE("url") || !copyValue(parsed.url, sizeof(parsed.url), value) ||
      !TAKE("min_recovery")) {
    return false;
  }
  uint32_t recovery = 0;
  if (!parseU32(value, &recovery) || recovery > UINT16_MAX) return false;
  parsed.minRecovery = static_cast<uint16_t>(recovery);
  if (!TAKE("published_at") || !parseU64(value, &parsed.publishedAt) ||
      !TAKE("notes_b64")) {
    return false;
  }
  size_t notesLength = 0;
  if (mbedtls_base64_decode(reinterpret_cast<uint8_t*>(parsed.notes),
                            sizeof(parsed.notes) - 1, &notesLength,
                            reinterpret_cast<const uint8_t*>(value),
                            strlen(value)) != 0 ||
      notesLength >= sizeof(parsed.notes)) {
    return false;
  }
  parsed.notes[notesLength] = '\0';
  if (*cursor != '\0') return false;
#undef TAKE

  const char* releasePrefix =
      "https://github.com/JStone2934/DesktopRadar/releases/download/";
  char expectedUrl[sizeof(parsed.url)];
  const int expectedLength = snprintf(
      expectedUrl, sizeof(expectedUrl), "%s%s/DesktopRadar-%s-esp32c3.bin",
      releasePrefix, parsed.tag, parsed.tag);
  if (strcmp(parsed.keyId, "radar-prod-1") != 0 ||
      strcmp(parsed.channel, RADAR_CHANNEL) != 0 ||
      strcmp(parsed.chip, "esp32c3") != 0 ||
      strcmp(parsed.layout, RADAR_LAYOUT_ID) != 0 ||
      parsed.tag[0] != 'v' || strcmp(parsed.tag + 1, parsed.version) != 0 ||
      expectedLength <= 0 ||
      static_cast<size_t>(expectedLength) >= sizeof(expectedUrl) ||
      strcmp(parsed.url, expectedUrl) != 0) {
    return false;
  }
  *out = parsed;
  return true;
}

bool updateManifestIsInstallable(const UpdateManifest& manifest,
                                 uint32_t currentVersionCode,
                                 size_t filesystemBytes) {
  return manifest.versionCode > currentVersionCode &&
         manifest.minRecovery <= RADAR_RECOVERY_API && manifest.size > 0 &&
         manifest.size <= RADAR_MAIN_MAX_BYTES &&
         filesystemBytes >= manifest.size + RADAR_STAGE_RESERVE_BYTES;
}

const char* updateErrorName(UpdateError error) {
  switch (error) {
    case UpdateError::None: return "NONE";
    case UpdateError::TimeSync: return "TIME_SYNC";
    case UpdateError::ManifestHttp: return "MANIFEST_HTTP";
    case UpdateError::ManifestFormat: return "MANIFEST_FORMAT";
    case UpdateError::ManifestSignature: return "MANIFEST_SIGNATURE";
    case UpdateError::ManifestChanged: return "MANIFEST_CHANGED";
    case UpdateError::AssetPreflight: return "ASSET_PREFLIGHT";
    case UpdateError::FactoryIncompatible: return "FACTORY_INCOMPATIBLE";
    case UpdateError::FilesystemFormat: return "FS_FORMAT";
    case UpdateError::Download: return "DOWNLOAD";
    case UpdateError::SizeMismatch: return "SIZE_MISMATCH";
    case UpdateError::ShaMismatch: return "SHA_MISMATCH";
    case UpdateError::OtaBegin: return "OTA_BEGIN";
    case UpdateError::OtaWrite: return "OTA_WRITE";
    case UpdateError::OtaEnd: return "OTA_END";
    case UpdateError::BootSelect: return "BOOT_SELECT";
    case UpdateError::SelfTest: return "SELF_TEST";
    case UpdateError::Cancelled: return "CANCELLED";
    case UpdateError::StoreCorrupt: return "STORE_CORRUPT";
  }
  return "UNKNOWN";
}
