#pragma once

#include <Arduino.h>

struct RainviewerFrame {
  String host;  // e.g. https://tilecache.rainviewer.com
  String path;  // e.g. /v2/radar/....
  uint32_t time = 0;
};

/** 拉取 weather-maps.json，取 radar.past 最后一帧。 */
bool rainviewerFetchLatest(RainviewerFrame* out);

/** 构造雷达瓦片 URL。 */
String rainviewerTileUrl(const RainviewerFrame& frame, int zoom, int tx, int ty);
