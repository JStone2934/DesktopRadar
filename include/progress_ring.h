#pragma once

#include "LGFX_GC9A01.hpp"

/**
 * 底栏横向进度条：贴在日期黑条上沿。
 * done01=0 无条；增大时自中心向两侧延伸；done01≈1 消失。
 * underlayZoom 保留兼容调用，不再用于整屏 blit。
 * @return 若实际擦除/绘制了细条则 true（调用方据此决定是否重绘预警环）。
 */
bool progressRingUpdate(LGFX* lcd, float done01, int underlayZoom);

/** 强制隐藏；若擦除了细条返回 true。 */
bool progressRingHide(LGFX* lcd, int underlayZoom);
