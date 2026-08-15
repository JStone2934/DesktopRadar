#pragma once

// Release builds override these from PLATFORMIO_BUILD_FLAGS. Keeping safe
// defaults makes normal local builds deterministic and clearly identifiable.
#ifndef RADAR_VERSION
#define RADAR_VERSION "0.2.0"
#endif

#ifndef RADAR_VERSION_CODE
#define RADAR_VERSION_CODE 2000U
#endif

#ifndef RADAR_BUILD_SHA
#define RADAR_BUILD_SHA "local"
#endif

#define RADAR_CHANNEL "stable"
#define RADAR_LAYOUT_ID "radar-4m-recovery-v1"
#define RADAR_RECOVERY_API 1U
#define RADAR_MANIFEST_URL                                                   \
  "https://github.com/JStone2934/DesktopRadar/releases/latest/download/"   \
  "latest.json"

#define RADAR_MAIN_MAX_BYTES 1650000U
#define RADAR_FACTORY_MAX_BYTES (500U * 1024U)
#define RADAR_STAGE_RESERVE_BYTES (96U * 1024U)
