#pragma once

#include <stdint.h>

#include "LGFX_GC9A01.hpp"
#include "app_config.h"
#include "update_protocol.h"

struct UpdatePortalInfo {
  bool haveManifest;
  bool available;
  bool factoryCompatible;
  uint64_t lastAttempt;
  uint64_t lastSuccess;
  uint64_t lastErrorAt;
  UpdateError lastError;
  char currentVersion[24];
  char currentBuild[41];
  UpdateManifest manifest;
  uint32_t nonce;
};

void updateManagerBegin();
void updateManagerGetPortalInfo(UpdatePortalInfo* out);
bool updateManagerRequestFromPortal(uint32_t versionCode, uint32_t nonce);

/** Resume REQUESTED/DOWNLOADING/STAGED after a power loss. */
bool updateManagerHandleBootResume(LGFX* lcd, AppConfig* cfg);

/** Execute a request accepted by the Web portal. Success reboots to factory. */
bool updateManagerExecuteConfirmed(LGFX* lcd, AppConfig* cfg);

/** Confirm or reject an ESP-IDF PENDING_VERIFY first boot. */
bool updateManagerConfirmFirstBoot(LGFX* lcd, bool filesystemReady,
                                   bool workerReady);

/** Starts at most one low-priority manifest check when canStart is true. */
void updateManagerServiceDailyCheck(bool canStart);
bool updateManagerBusy();

/** True if the failed update formatted LittleFS and cache state must reload. */
bool updateManagerConsumeFilesystemReset();
