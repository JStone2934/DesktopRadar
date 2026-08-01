#pragma once

#include "LGFX_GC9A01.hpp"

/**
 * 两阶段造片：HTTPS 瓦片落盘 → 烘焙 RGB565 成品。
 * pushToDisplay 时烘焙后行刷上屏。
 */
bool composeRadarFrame(LGFX* lcd, float lat, float lon, int zoom,
                       bool pushToDisplay);
