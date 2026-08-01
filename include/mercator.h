#pragma once

#include <stdint.h>

struct GlobalPixel {
  double x;
  double y;
};

struct TileCoord {
  int tx;
  int ty;
};

/** Web 墨卡托：经纬度 → 全局像素（TILE_SIZE * 2^zoom 坐标系）。 */
GlobalPixel latLonToGlobalPixel(double lat, double lon, int zoom);

TileCoord latLonToTile(double lat, double lon, int zoom);

/**
 * 以 (lat,lon) 为中心的 LCD_WIDTH×LCD_HEIGHT 视口，
 * 计算左上角全局像素与覆盖的瓦片范围 [tx0..tx1], [ty0..ty1]（含端点）。
 */
struct Viewport {
  double origin_px;  // 视口左上角全局像素 X
  double origin_py;
  int tx0, ty0, tx1, ty1;
};

Viewport computeViewport(double lat, double lon, int zoom);
