#include "compose.h"

#include <WiFi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basemap.h"
#include "config.h"
#include "http_fetch.h"
#include "mercator.h"
#include "rainviewer.h"

static void logHeap(const char* tag) {
  Serial.printf("  [%s] heap=%u maxAlloc=%u\n", tag, ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
}

static bool drawPngClipped(LovyanGFX* gfx, const uint8_t* data, size_t len, int ox,
                           int oy) {
  if (!data || len < 8 || data[0] != 0x89 || data[1] != 'P') {
    return false;
  }
  int dest_x = ox, dest_y = oy, off_x = 0, off_y = 0;
  if (dest_x < 0) {
    off_x = -dest_x;
    dest_x = 0;
  }
  if (dest_y < 0) {
    off_y = -dest_y;
    dest_y = 0;
  }
  int max_w = TILE_SIZE - off_x;
  int max_h = TILE_SIZE - off_y;
  if (dest_x >= LCD_WIDTH || dest_y >= LCD_HEIGHT || max_w <= 0 || max_h <= 0) {
    return true;
  }
  if (dest_x + max_w > LCD_WIDTH) {
    max_w = LCD_WIDTH - dest_x;
  }
  if (dest_y + max_h > LCD_HEIGHT) {
    max_h = LCD_HEIGHT - dest_y;
  }

  gfx->releasePngMemory();
  Serial.printf("  draw len=%u @(%d,%d) off(%d,%d) %dx%d max=%u\n",
                (unsigned)len, dest_x, dest_y, off_x, off_y, max_w, max_h,
                ESP.getMaxAllocHeap());
  const bool ok = gfx->drawPng(data, (uint32_t)len, dest_x, dest_y, max_w, max_h,
                               off_x, off_y, 1.0f, 1.0f);
  if (!ok) {
    Serial.println("  drawPng FAIL");
  }
  return ok;
}

static bool fetchAndDraw(LovyanGFX* gfx, const String& url, const char* referer,
                         int ox, int oy, size_t minBytes, const char* tag) {
  logHeap(tag);
  size_t len = 0;
  uint8_t* data =
      httpFetch(url.c_str(), referer, HTTP_MAX_TILE_BYTES, &len, TILE_TIMEOUT_MS);
  if (!data) {
    Serial.printf("  %s download fail\n", tag);
    return false;
  }
  if (len < minBytes) {
    Serial.printf("  %s too small %u\n", tag, (unsigned)len);
    free(data);
    return false;
  }
  const bool ok = drawPngClipped(gfx, data, len, ox, oy);
  free(data);
  delay(30);
  return ok;
}

bool composeRadarFrame(LGFX* lcd, LGFX_Sprite* /*sprite*/, float lat, float lon,
                       int zoom) {
  if (!lcd) {
    return false;
  }
  logHeap("compose-start");

  const Viewport vp = computeViewport((double)lat, (double)lon, zoom);
  Serial.printf("Viewport tiles x=[%d..%d] y=[%d..%d]\n", vp.tx0, vp.tx1, vp.ty0,
                vp.ty1);

  // 先拉元数据
  RainviewerFrame meta;
  const bool haveRadar = rainviewerFetchLatest(&meta);
  logHeap("after-meta");

  lcd->fillScreen(lcd->color565(BASEMAP_BACKDROP_R, BASEMAP_BACKDROP_G,
                                BASEMAP_BACKDROP_B));

  int baseOk = 0, baseFail = 0;
  int radarOk = 0, radarFail = 0;

  for (int ty = vp.ty0; ty <= vp.ty1; ++ty) {
    for (int tx = vp.tx0; tx <= vp.tx1; ++tx) {
      const int ox = (int)lround((double)tx * TILE_SIZE - vp.origin_px);
      const int oy = (int)lround((double)ty * TILE_SIZE - vp.origin_py);
      Serial.printf("  basemap z=%d x=%d y=%d -> (%d,%d)\n", zoom, tx, ty, ox, oy);
      if (fetchAndDraw(lcd, basemapTileUrl(zoom, tx, ty), AMAP_REFERER, ox, oy,
                       800, "basemap")) {
        ++baseOk;
      } else {
        ++baseFail;
      }
    }
  }
  Serial.printf("Basemap ok=%d fail=%d\n", baseOk, baseFail);

  if (haveRadar) {
    for (int ty = vp.ty0; ty <= vp.ty1; ++ty) {
      for (int tx = vp.tx0; tx <= vp.tx1; ++tx) {
        const int ox = (int)lround((double)tx * TILE_SIZE - vp.origin_px);
        const int oy = (int)lround((double)ty * TILE_SIZE - vp.origin_py);
        Serial.printf("  radar z=%d x=%d y=%d\n", zoom, tx, ty);
        if (fetchAndDraw(lcd, rainviewerTileUrl(meta, zoom, tx, ty), nullptr, ox,
                         oy, 200, "radar")) {
          ++radarOk;
        } else {
          ++radarFail;
        }
      }
    }
  }
  Serial.printf("Radar ok=%d fail=%d\n", radarOk, radarFail);

  lcd->releasePngMemory();
  lcd->drawFastHLine(LCD_WIDTH / 2 - 8, LCD_HEIGHT / 2, 17,
                     lcd->color565(255, 255, 255));
  lcd->drawFastVLine(LCD_WIDTH / 2, LCD_HEIGHT / 2 - 8, 17,
                     lcd->color565(255, 255, 255));

  return baseOk > 0 || radarOk > 0;
}
