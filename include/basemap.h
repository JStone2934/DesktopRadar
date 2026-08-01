#pragma once

#include <Arduino.h>

/** 高德矢量路网瓦片 URL（Web 墨卡托 z/x/y）。 */
String basemapTileUrl(int zoom, int tx, int ty);
