#include "compose.h"

#include <WiFi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basemap.h"
#include "button.h"
#include "config.h"
#include "frame_cache.h"
#include "http_fetch.h"
#include "mercator.h"
#include "rainviewer.h"
#include "zoom_ctrl.h"

static void pollButtonDuringCompose() {
  const ButtonEvent ev = buttonPoll();
  if (ev == ButtonEvent::ShortPress) {
    const int next = zoomCycleNext();
    zoomSetPending(next);
    composeRequestAbort();
    Serial.printf("compose: short press -> pending z%d\n", next);
  }
}

static void logHeap(const char* tag) {
  Serial.printf("  [%s] heap=%u maxAlloc=%u\n", tag, ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
}

static bool drawPngClipped(LGFX* lcd, const uint8_t* data, size_t len, int ox,
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

  lcd->releasePngMemory();
  Serial.printf("  draw len=%u @(%d,%d) off(%d,%d) %dx%d max=%u\n",
                (unsigned)len, dest_x, dest_y, off_x, off_y, max_w, max_h,
                ESP.getMaxAllocHeap());
  const bool ok = lcd->drawPng(data, (uint32_t)len, dest_x, dest_y, max_w, max_h,
                               off_x, off_y, 1.0f, 1.0f);
  if (!ok) {
    Serial.println("  drawPng FAIL");
  }
  return ok;
}

static bool fetchSaveMaybeDraw(LGFX* lcd, int zoom, bool isRadar, int tx, int ty,
                               const String& url, const char* referer, int ox,
                               int oy, size_t minBytes, const char* tag,
                               bool pushToDisplay) {
  pollButtonDuringCompose();
  if (composeAbortRequested()) {
    return false;
  }
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
  if (composeAbortRequested()) {
    free(data);
    return false;
  }

  frameCacheSaveTile(zoom, isRadar, tx, ty, data, len);

  bool ok = true;
  if (pushToDisplay) {
    ok = drawPngClipped(lcd, data, len, ox, oy);
  }
  free(data);
  delay(30);
  return ok;
}

bool composeRadarFrame(LGFX* lcd, float lat, float lon, int zoom,
                       bool pushToDisplay) {
  if (!lcd) {
    return false;
  }
  if (!zoomCanCompose(zoom)) {
    Serial.printf("compose reject zoom=%d (upsample deferred)\n", zoom);
    return false;
  }

  logHeap("compose-start");
  composeClearAbort();

  if (!frameCachePrepare(zoom)) {
    Serial.println("frameCachePrepare fail");
    return false;
  }

  const Viewport vp = computeViewport((double)lat, (double)lon, zoom);
  Serial.printf("Viewport z=%d tiles x=[%d..%d] y=[%d..%d] push=%d\n", zoom,
                vp.tx0, vp.tx1, vp.ty0, vp.ty1, (int)pushToDisplay);

  RainviewerFrame meta;
  const bool haveRadar = rainviewerFetchLatest(&meta);
  if (composeAbortRequested()) {
    return false;
  }
  logHeap("after-meta");

  if (pushToDisplay) {
    lcd->fillScreen(lcd->color565(BASEMAP_BACKDROP_R, BASEMAP_BACKDROP_G,
                                  BASEMAP_BACKDROP_B));
  }

  int baseOk = 0, baseFail = 0;
  int radarOk = 0, radarFail = 0;

  for (int ty = vp.ty0; ty <= vp.ty1; ++ty) {
    for (int tx = vp.tx0; tx <= vp.tx1; ++tx) {
      if (composeAbortRequested()) {
        return false;
      }
      const int ox = (int)lround((double)tx * TILE_SIZE - vp.origin_px);
      const int oy = (int)lround((double)ty * TILE_SIZE - vp.origin_py);
      Serial.printf("  basemap z=%d x=%d y=%d -> (%d,%d)\n", zoom, tx, ty, ox, oy);
      if (fetchSaveMaybeDraw(lcd, zoom, false, tx, ty,
                             basemapTileUrl(zoom, tx, ty), AMAP_REFERER, ox, oy,
                             800, "basemap", pushToDisplay)) {
        ++baseOk;
      } else {
        ++baseFail;
      }
    }
  }
  Serial.printf("Basemap ok=%d fail=%d\n", baseOk, baseFail);

  if (haveRadar && !composeAbortRequested()) {
    for (int ty = vp.ty0; ty <= vp.ty1; ++ty) {
      for (int tx = vp.tx0; tx <= vp.tx1; ++tx) {
        if (composeAbortRequested()) {
          return false;
        }
        const int ox = (int)lround((double)tx * TILE_SIZE - vp.origin_px);
        const int oy = (int)lround((double)ty * TILE_SIZE - vp.origin_py);
        Serial.printf("  radar z=%d x=%d y=%d\n", zoom, tx, ty);
        if (fetchSaveMaybeDraw(lcd, zoom, true, tx, ty,
                               rainviewerTileUrl(meta, zoom, tx, ty), nullptr, ox,
                               oy, 200, "radar", pushToDisplay)) {
          ++radarOk;
        } else {
          ++radarFail;
        }
      }
    }
  }
  Serial.printf("Radar ok=%d fail=%d\n", radarOk, radarFail);

  if (composeAbortRequested()) {
    return false;
  }

  frameCacheWriteMeta(zoom, vp, haveRadar && radarOk > 0);
  const bool ok = baseOk > 0 || radarOk > 0;
  if (ok) {
    frameCacheCommit(zoom);
  }

  if (pushToDisplay && ok) {
    lcd->releasePngMemory();
    lcd->drawFastHLine(LCD_WIDTH / 2 - 8, LCD_HEIGHT / 2, 17,
                       lcd->color565(255, 255, 255));
    lcd->drawFastVLine(LCD_WIDTH / 2, LCD_HEIGHT / 2 - 8, 17,
                       lcd->color565(255, 255, 255));
  }

  Serial.printf("compose done z=%d ok=%d\n", zoom, (int)ok);
  return ok;
}
