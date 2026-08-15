#pragma once

#include <stdint.h>

#include "LGFX_GC9A01.hpp"
#include "app_config.h"

enum class OrnamentPortalAction : uint8_t {
  None = 0,
  RestartRadar,
  UpdateRequested,
};

bool ornamentPortalBegin(LGFX* lcd, AppConfig* cfg);
void ornamentPortalService();
void ornamentPortalEnd();
bool ornamentPortalBusy();
OrnamentPortalAction ornamentPortalTakeAction();
