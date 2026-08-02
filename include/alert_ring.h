#pragma once

#include <stdint.h>

#include "LGFX_GC9A01.hpp"

/**
 * 屏缘彩色预警环：中心有云图时显示对应雷达色。
 * 3 秒内从透明淡入到实色，之后常显；可覆盖进度环。
 * underlayZoom>=0 时优先 blit 成品再画环。
 */
void alertRingSet(uint16_t color565, bool hasCloud);
void alertRingClear(LGFX* lcd, int underlayZoom);
void alertRingHide(LGFX* lcd, int underlayZoom);
/** 淡入期间由 loop 调用；已稳定时几乎无开销。 */
void alertRingTick(LGFX* lcd, int underlayZoom);
/** 强制用当前状态重绘一帧（进度环刷新后叠在上层）。 */
void alertRingRedraw(LGFX* lcd, int underlayZoom);
bool alertRingIsVisible();
bool alertRingNeedsTick();
