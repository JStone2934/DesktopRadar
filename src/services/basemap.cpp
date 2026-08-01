#include "basemap.h"

#include <stdio.h>

#include "config.h"

String basemapTileUrl(int zoom, int tx, int ty) {
  const unsigned sub = (unsigned)((tx + ty) % 4) + 1;
  char buf[192];
  snprintf(buf, sizeof(buf), AMAP_TILE_FMT, sub, tx, ty, zoom);
  return String(buf);
}
