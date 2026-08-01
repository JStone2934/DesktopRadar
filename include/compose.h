#pragma once

#include "LGFX_GC9A01.hpp"

/**
 * 造片进度回调：zoom 为当前档；local01 为该档内 0..1（下载+烘焙）。
 * 在 HTTP / 解码循环中可能被频繁调用。
 */
typedef void (*ComposeProgressFn)(int zoom, float local01);

void composeSetProgressFn(ComposeProgressFn fn);

/**
 * 两阶段造片：HTTPS 瓦片落盘 → 烘焙 RGB565 成品。
 * pushToDisplay 时烘焙后行刷上屏。
 */
bool composeRadarFrame(LGFX* lcd, float lat, float lon, int zoom,
                       bool pushToDisplay);
