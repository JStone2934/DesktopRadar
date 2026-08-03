#pragma once

#include <stdint.h>

#include "LGFX_GC9A01.hpp"

/**
 * 屏缘彩色预警环：中心有云图时显示对应雷达色。
 * 先 0.5s 淡入→0.5s 淡出（一轮 1s），重复 2 次，再淡入后常显；与底图 alpha 混合。
 * underlayZoom>=0 时按该档成品混合环带。
 */
void alertRingSet(uint16_t color565, bool hasCloud);
void alertRingClear(LGFX* lcd, int underlayZoom);
void alertRingHide(LGFX* lcd, int underlayZoom);
/** 脉冲/淡入期间由 loop 调用；已常显时几乎无开销。 */
void alertRingTick(LGFX* lcd, int underlayZoom);
/** 强制用当前状态重绘一帧（进度环刷新后叠在上层）。 */
void alertRingRedraw(LGFX* lcd, int underlayZoom);
bool alertRingIsVisible();
bool alertRingNeedsTick();
