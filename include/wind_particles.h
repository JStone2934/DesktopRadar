#pragma once

#include "LGFX_GC9A01.hpp"
#include "app_config.h"

void windParticlesBegin();
void windParticlesReset();

/** 设置粒子头样式；调用方在切换样式后应重置或重绘底图。 */
void windParticlesSetStyle(WindParticleStyle style);

/** LCD 已被完整新底图覆盖；丢弃旧底色轨迹，防止随后恢复旧雷达像素。 */
void windParticlesNotifyBaseRedrawn();

/**
 * 推进并绘制一帧。亮点使用局部渐隐轨迹；两种箭头均无拖尾且合并
 * 恢复/绘制，所有样式都不会逐帧清空整屏。
 * busy=true 时自动降到 WIND_BUSY_FRAME_MS。
 * @return 本次是否更新了粒子覆盖层。
 */
bool windParticlesTick(LGFX* lcd, int displayedZoom, bool busy);
