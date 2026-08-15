#pragma once

#include <stddef.h>
#include <stdint.h>

#include "version.h"

static constexpr uint32_t UPDATE_MANIFEST_MAGIC = 0x524D4632U;  // RMF2
static constexpr uint32_t UPDATE_JOURNAL_MAGIC = 0x524A4E31U;   // RJN1
static constexpr uint16_t UPDATE_RECORD_SCHEMA = 1;
static constexpr size_t UPDATE_PAYLOAD_MAX = 1024;
static constexpr size_t UPDATE_SIGNATURE_MAX = 80;

enum class UpdateState : uint8_t {
  Idle = 0,
  Requested,
  Downloading,
  Staged,
  Installing,
  BootPending,
  FirstBoot,
  Confirmed,
  Failed,
};

enum class UpdatePhase : uint8_t {
  None = 0,
  TimeSync,
  Manifest,
  Preflight,
  Filesystem,
  Download,
  Factory,
  Install,
  Boot,
  SelfTest,
};

enum class UpdateError : uint16_t {
  None = 0,
  TimeSync,
  ManifestHttp,
  ManifestFormat,
  ManifestSignature,
  ManifestChanged,
  AssetPreflight,
  FactoryIncompatible,
  FilesystemFormat,
  Download,
  SizeMismatch,
  ShaMismatch,
  OtaBegin,
  OtaWrite,
  OtaEnd,
  BootSelect,
  SelfTest,
  Cancelled,
  StoreCorrupt,
};

struct UpdateManifest {
  char keyId[24];
  char channel[12];
  char version[24];
  uint32_t versionCode;
  char tag[32];
  char buildSha[41];
  char chip[12];
  char layout[32];
  uint32_t size;
  uint8_t sha256[32];
  char url[256];
  uint16_t minRecovery;
  uint64_t publishedAt;
  char notes[257];
};

struct UpdateManifestRecord {
  uint32_t magic;
  uint16_t schema;
  uint16_t reserved;
  uint32_t sequence;
  uint64_t checkedAt;
  uint16_t payloadLength;
  uint16_t signatureLength;
  UpdateManifest manifest;
  uint8_t payload[UPDATE_PAYLOAD_MAX];
  uint8_t signature[UPDATE_SIGNATURE_MAX];
  uint32_t crc32;
};

struct UpdateJournal {
  uint32_t magic;
  uint16_t schema;
  uint16_t reserved;
  uint32_t sequence;
  UpdateState state;
  UpdatePhase phase;
  UpdateError error;
  uint8_t attempts;
  uint8_t reserved2[3];
  uint32_t targetVersionCode;
  uint32_t expectedSize;
  uint8_t expectedSha256[32];
  uint32_t manifestSequence;
  uint64_t requestedAt;
  uint64_t updatedAt;
  uint32_t crc32;
};

uint32_t updateCrc32(const void* data, size_t length);
bool updateManifestRecordValid(const UpdateManifestRecord& record);
bool updateJournalValid(const UpdateJournal& journal);
bool updateManifestVerifyAndParse(const uint8_t* payload, size_t payloadLength,
                                  const uint8_t* signature,
                                  size_t signatureLength,
                                  UpdateManifest* out);
bool updateManifestIsInstallable(const UpdateManifest& manifest,
                                 uint32_t currentVersionCode,
                                 size_t filesystemBytes);
const char* updateErrorName(UpdateError error);
