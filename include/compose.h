#pragma once

#include "LGFX_GC9A01.hpp"

/** 下载底图+雷达并合成到 sprite（下载阶段会 deleteSprite，完成后重新 create）。 */
bool composeRadarFrame(LGFX* lcd, LGFX_Sprite* sprite, float lat, float lon,
                       int zoom);
