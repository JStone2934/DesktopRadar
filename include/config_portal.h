#pragma once

#include <stdint.h>

#include "LGFX_GC9A01.hpp"
#include "app_config.h"

enum class PortalResult : uint8_t {
  TimedOut = 0,
  SkippedByButton,
  Saved,
};

/**
 * 开 SoftAP + Web 配置页，阻塞直到保存 / S 键短按 / 超时。
 * timeoutMs：门户最长等待；屏上显示倒计时。
 * 若 Saved，outCfg 为刚保存的配置；否则用 appConfigLoad 结果填入。
 */
PortalResult configPortalRun(LGFX* lcd, uint32_t timeoutMs, AppConfig* outCfg);
