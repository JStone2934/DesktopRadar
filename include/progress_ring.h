#pragma once

#include "LGFX_GC9A01.hpp"

/**
 * 底栏横向进度条：贴在日期黑条上沿。
 * done01=0 无条；增大时自中心向两侧延伸；done01≈1 消失。
 * underlayZoom 保留兼容调用，不再用于整屏 blit。
 */
void progressRingUpdate(LGFX* lcd, float done01, int underlayZoom);

/** 强制隐藏（例如进设置门户前）。 */
void progressRingHide(LGFX* lcd, int underlayZoom);
