#pragma once

#include <Arduino.h>

struct RainviewerFrame {
  String host;  // e.g. https://tilecache.rainviewer.com
  String path;  // e.g. /v2/radar/....
  uint32_t time = 0;
};

/** 拉取 weather-maps.json，取 radar.past 最后一帧。 */
bool rainviewerFetchLatest(RainviewerFrame* out);

/**
 * 拉取 radar.past，按 windowHours 过滤（相对最新帧时间），旧→新写入 out。
 * 最多 maxOut 帧；不足 2 帧时仍返回 true 但 *outCount 可能 <2。
 */
bool rainviewerFetchPast(RainviewerFrame* out, int maxOut, int* outCount,
                         float windowHours);

/** 构造雷达瓦片 URL。 */
String rainviewerTileUrl(const RainviewerFrame& frame, int zoom, int tx, int ty);
