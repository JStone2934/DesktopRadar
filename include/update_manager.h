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

/**
 * Marks a check due after 24 hours and starts it when the foreground is safe.
 * On each seventh day of a failure cycle, userIdle is no longer required;
 * a failed forced attempt starts a new seven-day cycle. safeToStart still
 * protects an active compose and physical button handling.
 */
void updateManagerServiceDailyCheck(bool safeToStart, bool userIdle);
/** True while a persisted due check is waiting to run. */
bool updateManagerCheckPending();
bool updateManagerBusy();

/** True if the failed update formatted LittleFS and cache state must reload. */
bool updateManagerConsumeFilesystemReset();
