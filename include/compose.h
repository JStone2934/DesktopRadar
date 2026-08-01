#pragma once

#include "LGFX_GC9A01.hpp"

/**
 * 下载底图+雷达：直绘 LCD（pushToDisplay）和/或写入瓦片 PNG 缓存。
 * 不占用 112KB 帧缓冲，避免挤爆 PNG/TLS 堆。
 */
bool composeRadarFrame(LGFX* lcd, float lat, float lon, int zoom,
                       bool pushToDisplay);
