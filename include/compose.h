#pragma once

#include "LGFX_GC9A01.hpp"

/**
 * 造片进度回调：zoom 为当前档；local01 为该档内 0..1（下载+烘焙）。
 * 在 HTTP / 解码循环中可能被频繁调用。
 */
typedef void (*ComposeProgressFn)(int zoom, float local01);

void composeSetProgressFn(ComposeProgressFn fn);

/**
 * 造片上屏回调：composeRadarFrame 在 pushImage 将新帧写入 LCD 后立即调用。
 * 主循环据此同步 s_displayedZoom / 预警环 underlay，避免后续 reportComposeProgress
 * 触发 pumpAlertRingDuringCompose 时仍使用旧档底图导致闪烁。
 */
typedef void (*ComposeDisplayFn)(int zoom);

void composeSetDisplayFn(ComposeDisplayFn fn);

/**
 * 两阶段造片：HTTPS 瓦片落盘 → 烘焙 RGB565 成品。
 * pushToDisplay 时烘焙后行刷上屏。
 */
bool composeRadarFrame(LGFX* lcd, float lat, float lon, int zoom,
                       bool pushToDisplay);

/**
 * 用 RainViewer past 为当前缩放补满动画队列（最多 ANIM_MAX_FRAMES）。
 * 不覆盖已就绪静帧；底图只拉一次。可 abort。
 */
bool composeAnimFillPast(LGFX* lcd, float lat, float lon, int zoom);
