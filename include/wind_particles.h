#pragma once

#include "LGFX_GC9A01.hpp"

void windParticlesBegin();
void windParticlesReset();

/**
 * 推进并绘制一帧。每帧先从 RGB565 缓存恢复干净底图，再叠加短轨迹。
 * busy=true 时自动降到 WIND_BUSY_FRAME_MS。
 * @return 本次是否重绘了整屏和粒子。
 */
bool windParticlesTick(LGFX* lcd, int displayedZoom, bool busy);
