#pragma once

#include "LGFX_GC9A01.hpp"

/**
 * 屏缘白色圆环：表示全局缓存剩余量。
 * done01=0 满环；done01=1 消失。
 * underlayZoom>=0 时优先 blit 成品（含造片中未 ready 的旧图）再画环。
 */
void progressRingUpdate(LGFX* lcd, float done01, int underlayZoom);

/** 强制隐藏（例如进设置门户前）。 */
void progressRingHide(LGFX* lcd, int underlayZoom);
