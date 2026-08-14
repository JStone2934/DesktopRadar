#pragma once

#include "LGFX_GC9A01.hpp"

void windParticlesBegin();
void windParticlesReset();

/** LCD 已被完整新底图覆盖；丢弃旧底色轨迹，防止随后恢复旧雷达像素。 */
void windParticlesNotifyBaseRedrawn();

/**
 * 推进并绘制一帧。轨迹像素保存底色并局部渐隐，不再逐帧恢复整屏。
 * busy=true 时自动降到 WIND_BUSY_FRAME_MS。
 * @return 本次是否更新了粒子覆盖层。
 */
bool windParticlesTick(LGFX* lcd, int displayedZoom, bool busy);
