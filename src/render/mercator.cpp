#include "mercator.h"

#include <math.h>

#include "config.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

GlobalPixel latLonToGlobalPixel(double lat, double lon, int zoom) {
  const double scale = (double)TILE_SIZE * (double)(1u << zoom);
  GlobalPixel p;
  p.x = (lon + 180.0) / 360.0 * scale;
  const double lat_rad = lat * M_PI / 180.0;
  p.y = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * scale;
  return p;
}

TileCoord latLonToTile(double lat, double lon, int zoom) {
  const GlobalPixel p = latLonToGlobalPixel(lat, lon, zoom);
  TileCoord t;
  t.tx = (int)floor(p.x / (double)TILE_SIZE);
  t.ty = (int)floor(p.y / (double)TILE_SIZE);
  return t;
}

Viewport computeViewport(double lat, double lon, int zoom) {
  const GlobalPixel c = latLonToGlobalPixel(lat, lon, zoom);
  Viewport v;
  v.origin_px = c.x - (double)VIEW_HALF;
  v.origin_py = c.y - (double)VIEW_HALF;

  const double right = v.origin_px + (double)LCD_WIDTH;
  const double bottom = v.origin_py + (double)LCD_HEIGHT;

  v.tx0 = (int)floor(v.origin_px / (double)TILE_SIZE);
  v.ty0 = (int)floor(v.origin_py / (double)TILE_SIZE);
  v.tx1 = (int)floor((right - 1.0) / (double)TILE_SIZE);
  v.ty1 = (int)floor((bottom - 1.0) / (double)TILE_SIZE);
  return v;
}
