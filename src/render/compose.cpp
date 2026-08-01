#include "compose.h"

#include <LittleFS.h>
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

#include <lgfx/utility/lgfx_pngle.h>

static void pollButtonDuringCompose() { inputServiceDuringBlock(); }

static void logHeap(const char* tag) {
  Serial.printf("  [%s] heap=%u maxAlloc=%u\n", tag, ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
}

static bool pngPathLooksComplete(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("  png check open fail %s\n", path);
    return false;
  }
  const size_t sz = f.size();
  if (sz < 33) {
    Serial.printf("  png check tiny sz=%u\n", (unsigned)sz);
    f.close();
    return false;
  }
  uint8_t head[8];
  const int nr = f.read(head, 8);
  if (nr != 8) {
    Serial.printf("  png check head read %d\n", nr);
    f.close();
    return false;
  }
  static const uint8_t kSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (memcmp(head, kSig, 8) != 0) {
    Serial.printf("  png bad sig %02x%02x%02x%02x sz=%u\n", head[0], head[1],
                  head[2], head[3], (unsigned)sz);
    f.close();
    return false;
  }
  uint8_t tail[12];
  if (!f.seek(sz - 12) || f.read(tail, 12) != 12) {
    Serial.printf("  png check tail read fail sz=%u\n", (unsigned)sz);
    f.close();
    return false;
  }
  f.close();
  static const uint8_t kIend[12] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60,
                                    0x82};
  if (memcmp(tail, kIend, 12) != 0) {
    Serial.printf("  png bad iend %02x%02x%02x%02x%02x%02x%02x%02x sz=%u\n",
                  tail[0], tail[1], tail[2], tail[3], tail[4], tail[5], tail[6],
                  tail[7], (unsigned)sz);
    // 部分 CDN 尾部可能有填充；签名正确则仍接受，交由 pngle 最终判定
    return true;
  }
  return true;
}

static bool fetchTileToFs(int zoom, bool isRadar, int tx, int ty, const String& url,
                          const char* referer, size_t minBytes, const char* tag) {
  pollButtonDuringCompose();
  if (composeAbortRequested()) {
    return false;
  }
  logHeap(tag);

  for (int attempt = 1; attempt <= 2; ++attempt) {
    size_t len = 0;
    uint8_t* data =
        httpFetch(url.c_str(), referer, HTTP_MAX_TILE_BYTES, &len, TILE_TIMEOUT_MS);
    if (!data || len < minBytes) {
      Serial.printf("  %s http fail len=%u try=%d\n", tag, (unsigned)len, attempt);
      free(data);
      delay(200 * attempt);
      continue;
    }
    if (len < 8 || data[0] != 0x89 || data[1] != 0x50 || data[2] != 0x4E ||
        data[3] != 0x47) {
      Serial.printf("  %s not png sig len=%u\n", tag, (unsigned)len);
      free(data);
      return false;
    }
    if (!frameCacheSaveTile(zoom, isRadar, tx, ty, data, len)) {
      Serial.printf("  %s save fail len=%u\n", tag, (unsigned)len);
      free(data);
      return false;
    }
    free(data);
    Serial.printf("  %s saved %u bytes\n", tag, (unsigned)len);
    delay(40);
    return true;
  }
  return false;
}

struct PngDecodeCtx {
  const uint8_t* data;
  size_t len;
  size_t pos;
  File* rawOut;
  File* alphaOut;  // 非空时同步写每像素 alpha
  uint16_t row[TILE_SIZE];
  uint8_t alpha[TILE_SIZE];
  uint32_t lastY;
  bool rowDirty;
  bool started;
  uint32_t pixels;
};

static uint32_t pngReadCb(void* user, uint8_t* buf, uint32_t len) {
  auto* ctx = (PngDecodeCtx*)user;
  if (!ctx || !ctx->data) {
    return 0;
  }
  if (!buf) {
    ctx->pos += len;
    return len;
  }
  if (ctx->pos >= ctx->len) {
    return 0;
  }
  uint32_t n = len;
  if (ctx->pos + n > ctx->len) {
    n = (uint32_t)(ctx->len - ctx->pos);
  }
  memcpy(buf, ctx->data + ctx->pos, n);
  ctx->pos += n;
  return n;
}

static void pngWritePadRows(PngDecodeCtx* ctx, uint32_t count) {
  uint16_t zrow[TILE_SIZE];
  uint8_t za[TILE_SIZE];
  memset(zrow, 0, sizeof(zrow));
  memset(za, 0, sizeof(za));
  for (uint32_t i = 0; i < count; ++i) {
    ctx->rawOut->write(reinterpret_cast<uint8_t*>(zrow), TILE_SIZE * 2);
    if (ctx->alphaOut) {
      ctx->alphaOut->write(za, TILE_SIZE);
    }
  }
}

static void pngFlushRow(PngDecodeCtx* ctx) {
  if (!ctx->rowDirty || !ctx->rawOut) {
    return;
  }
  ctx->rawOut->write(reinterpret_cast<uint8_t*>(ctx->row), TILE_SIZE * 2);
  if (ctx->alphaOut) {
    ctx->alphaOut->write(ctx->alpha, TILE_SIZE);
  }
  ctx->rowDirty = false;
}

static void pngDrawCb(void* user, uint32_t x, uint32_t y, uint_fast8_t div_x,
                      size_t len, const uint8_t* argb) {
  (void)div_x;
  auto* ctx = (PngDecodeCtx*)user;
  if (!ctx) {
    return;
  }
  if (!ctx->started) {
    ctx->started = true;
    ctx->lastY = 0;
    memset(ctx->row, 0, sizeof(ctx->row));
    memset(ctx->alpha, 0, sizeof(ctx->alpha));
    if (y > 0) {
      const uint32_t pad = y < (uint32_t)TILE_SIZE ? y : (uint32_t)TILE_SIZE;
      pngWritePadRows(ctx, pad);
      ctx->lastY = pad;
    }
  }
  if (y != ctx->lastY) {
    pngFlushRow(ctx);
    memset(ctx->row, 0, sizeof(ctx->row));
    memset(ctx->alpha, 0, sizeof(ctx->alpha));
    if (y > ctx->lastY + 1) {
      uint32_t gap = y - ctx->lastY - 1;
      if (ctx->lastY + 1 + gap > (uint32_t)TILE_SIZE) {
        gap = (uint32_t)TILE_SIZE - (ctx->lastY + 1);
      }
      pngWritePadRows(ctx, gap);
    }
    ctx->lastY = y;
  }
  for (size_t i = 0; i < len; ++i) {
    const uint32_t px = x + (uint32_t)i;
    if (px >= (uint32_t)TILE_SIZE) {
      break;
    }
    const uint8_t* p = argb + i * 4;
    const uint8_t a = p[0];
    ctx->alpha[px] = a;
    if (a == 0) {
      ctx->row[px] = 0;
    } else {
      const uint8_t r = p[1];
      const uint8_t g = p[2];
      const uint8_t b = p[3];
      ctx->row[px] =
          (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    ctx->rowDirty = true;
    ctx->pixels++;
  }
  if ((ctx->pixels & 0x7FF) == 0) {
    inputServiceDuringBlock();
  }
}

/** alphaPath 非空时额外写出 256×256 alpha（雷达半透明边缘）。 */
static bool decodePngToRawFile(const char* pngPath, const char* rawPath,
                               const char* alphaPath) {
  File in = LittleFS.open(pngPath, "r");
  if (!in || in.size() < 33 || !pngPathLooksComplete(pngPath)) {
    Serial.printf("  png incomplete/missing %s sz=%u\n", pngPath,
                  in ? (unsigned)in.size() : 0);
    if (in) {
      in.close();
    }
    return false;
  }
  if (in) {
    in.close();
  }
  in = LittleFS.open(pngPath, "r");
  if (!in) {
    return false;
  }
  const size_t pngLen = in.size();
  in.seek(0);
  uint8_t* pngData = (uint8_t*)malloc(pngLen);
  if (!pngData) {
    Serial.printf("  png malloc fail %u max=%u\n", (unsigned)pngLen,
                  ESP.getMaxAllocHeap());
    in.close();
    return false;
  }
  if (in.read(pngData, pngLen) != (int)pngLen) {
    Serial.println("  png read fail");
    free(pngData);
    in.close();
    return false;
  }
  in.close();

  LittleFS.remove(rawPath);
  if (alphaPath) {
    LittleFS.remove(alphaPath);
  }
  File raw = LittleFS.open(rawPath, "w");
  if (!raw) {
    free(pngData);
    return false;
  }
  File alphaFile;
  File* alphaPtr = nullptr;
  if (alphaPath) {
    alphaFile = LittleFS.open(alphaPath, "w");
    if (!alphaFile) {
      free(pngData);
      raw.close();
      LittleFS.remove(rawPath);
      return false;
    }
    alphaPtr = &alphaFile;
  }

  logHeap("pngle-before");
  pngle_t* pngle = lgfx_pngle_new();
  if (!pngle) {
    Serial.printf("  pngle_new fail max=%u\n", ESP.getMaxAllocHeap());
    free(pngData);
    raw.close();
    if (alphaPtr) {
      alphaPtr->close();
      LittleFS.remove(alphaPath);
    }
    return false;
  }

  PngDecodeCtx ctx{};
  ctx.data = pngData;
  ctx.len = pngLen;
  ctx.pos = 0;
  ctx.rawOut = &raw;
  ctx.alphaOut = alphaPtr;
  ctx.lastY = 0;
  ctx.rowDirty = false;
  ctx.started = false;
  ctx.pixels = 0;
  memset(ctx.row, 0, sizeof(ctx.row));
  memset(ctx.alpha, 0, sizeof(ctx.alpha));

  if (lgfx_pngle_prepare(pngle, pngReadCb, &ctx) < 0) {
    Serial.println("  pngle_prepare fail");
    lgfx_pngle_destroy(pngle);
    free(pngData);
    raw.close();
    LittleFS.remove(rawPath);
    if (alphaPtr) {
      alphaPtr->close();
      LittleFS.remove(alphaPath);
    }
    return false;
  }

  const int w = (int)lgfx_pngle_get_width(pngle);
  const int h = (int)lgfx_pngle_get_height(pngle);
  if (w <= 0 || h <= 0 || w > TILE_SIZE || h > TILE_SIZE) {
    Serial.printf("  bad png size %dx%d\n", w, h);
    lgfx_pngle_destroy(pngle);
    free(pngData);
    raw.close();
    LittleFS.remove(rawPath);
    if (alphaPtr) {
      alphaPtr->close();
      LittleFS.remove(alphaPath);
    }
    return false;
  }

  const int rc = lgfx_pngle_decomp(pngle, pngDrawCb);
  pngFlushRow(&ctx);
  uint32_t rowsWritten = ctx.started ? (ctx.lastY + 1) : 0;
  if (rowsWritten < (uint32_t)TILE_SIZE) {
    pngWritePadRows(&ctx, (uint32_t)TILE_SIZE - rowsWritten);
  }

  lgfx_pngle_destroy(pngle);
  free(pngData);
  raw.flush();
  const size_t sz = raw.size();
  raw.close();
  size_t asz = 0;
  if (alphaPtr) {
    alphaPtr->flush();
    asz = alphaPtr->size();
    alphaPtr->close();
  }

  const bool alphaOk =
      !alphaPath || asz == (size_t)TILE_SIZE * TILE_SIZE;
  if (rc < 0 || sz != (size_t)TILE_SIZE * TILE_SIZE * 2 || ctx.pixels == 0 ||
      !alphaOk) {
    Serial.printf("  png decomp fail rc=%d sz=%u asz=%u w=%d h=%d pix=%u\n", rc,
                  (unsigned)sz, (unsigned)asz, w, h, (unsigned)ctx.pixels);
    LittleFS.remove(rawPath);
    if (alphaPath) {
      LittleFS.remove(alphaPath);
    }
    return false;
  }
  Serial.printf("  png ok %dx%d pix=%u alpha=%d\n", w, h, (unsigned)ctx.pixels,
                alphaPath ? 1 : 0);
  return true;
}

static uint16_t backdropColor(LGFX* lcd) {
  return lcd->color565(BASEMAP_BACKDROP_R, BASEMAP_BACKDROP_G, BASEMAP_BACKDROP_B);
}

bool composeRadarFrame(LGFX* lcd, float lat, float lon, int zoom,
                       bool pushToDisplay) {
  if (!lcd) {
    return false;
  }
  if (!zoomCanCompose(zoom)) {
    Serial.printf("compose reject zoom=%d\n", zoom);
    return false;
  }

  logHeap("compose-start");
  composeClearAbort();

  if (!frameCachePrepare(zoom)) {
    Serial.println("frameCachePrepare fail");
    return false;
  }

  const Viewport vp = computeViewport((double)lat, (double)lon, zoom);
  const int overlayZoom =
      (zoom > RAINVIEWER_MAX_ZOOM) ? RAINVIEWER_MAX_ZOOM : zoom;
  const int scale = 1 << (zoom - overlayZoom);

  int rtx0 = vp.tx0;
  int rty0 = vp.ty0;
  int rtx1 = vp.tx1;
  int rty1 = vp.ty1;
  if (scale > 1) {
    rtx0 = (int)floor(vp.origin_px / ((double)TILE_SIZE * scale));
    rty0 = (int)floor(vp.origin_py / ((double)TILE_SIZE * scale));
    rtx1 = (int)floor((vp.origin_px + LCD_WIDTH - 1) / ((double)TILE_SIZE * scale));
    rty1 = (int)floor((vp.origin_py + LCD_HEIGHT - 1) / ((double)TILE_SIZE * scale));
  }

  Serial.printf(
      "Viewport z=%d base=[%d..%d],[%d..%d] radar_z=%d scale=%d "
      "rtx=[%d..%d] rty=[%d..%d] push=%d\n",
      zoom, vp.tx0, vp.tx1, vp.ty0, vp.ty1, overlayZoom, scale, rtx0, rtx1, rty0,
      rty1, (int)pushToDisplay);

  if (pushToDisplay) {
    lcd->fillScreen(TFT_BLACK);
    lcd->setTextDatum(MC_DATUM);
    lcd->setTextColor(TFT_WHITE, TFT_BLACK);
    lcd->setFont(&fonts::Font2);
    lcd->drawString("Downloading...", LCD_WIDTH / 2, LCD_HEIGHT / 2);
  }

  RainviewerFrame meta;
  const bool haveRadarMeta = rainviewerFetchLatest(&meta);
  if (composeAbortRequested()) {
    return false;
  }
  logHeap("after-meta");

  int baseOk = 0;
  int radarOk = 0;

  for (int ty = vp.ty0; ty <= vp.ty1; ++ty) {
    for (int tx = vp.tx0; tx <= vp.tx1; ++tx) {
      if (composeAbortRequested()) {
        return false;
      }
      Serial.printf("  dl basemap z=%d x=%d y=%d\n", zoom, tx, ty);
      if (fetchTileToFs(zoom, false, tx, ty, basemapTileUrl(zoom, tx, ty),
                        AMAP_REFERER, 800, "basemap")) {
        ++baseOk;
      }
    }
  }
  Serial.printf("Basemap dl ok=%d\n", baseOk);

  if (haveRadarMeta && !composeAbortRequested()) {
    for (int ty = rty0; ty <= rty1; ++ty) {
      for (int tx = rtx0; tx <= rtx1; ++tx) {
        if (composeAbortRequested()) {
          return false;
        }
        Serial.printf("  dl radar oz=%d x=%d y=%d\n", overlayZoom, tx, ty);
        if (fetchTileToFs(zoom, true, tx, ty,
                          rainviewerTileUrl(meta, overlayZoom, tx, ty), nullptr,
                          200, "radar")) {
          ++radarOk;
        }
      }
    }
  }
  Serial.printf("Radar dl ok=%d\n", radarOk);

  // 元数据成功但瓦片全失败：勿写入「无雷达」成品，否则秒切会一直缺雷达
  if (haveRadarMeta && radarOk == 0) {
    Serial.println("radar tiles missing, skip commit");
    return false;
  }
  // 元数据失败：允许仅底图上屏但不 commit（由调用方/预取重试）
  const bool allowCommit = haveRadarMeta && radarOk > 0;
  if (!haveRadarMeta) {
    Serial.println("RainViewer meta unavailable; bake display-only if needed");
  }

  if (composeAbortRequested() || (baseOk == 0 && radarOk == 0)) {
    return false;
  }

  frameCacheWriteMeta(zoom, vp, radarOk > 0, overlayZoom, scale, rtx0, rty0, rtx1,
                      rty1);

  if (pushToDisplay) {
    lcd->fillScreen(TFT_BLACK);
    lcd->setTextDatum(MC_DATUM);
    lcd->setTextColor(TFT_WHITE, TFT_BLACK);
    lcd->setFont(&fonts::Font2);
    lcd->drawString("Baking...", LCD_WIDTH / 2, LCD_HEIGHT / 2);
  }
  logHeap("before-bake");
  // 造片前再清一遍其它档残留，降低 LittleFS 峰值
  frameCacheScrubOrphans();

  // 先占帧缓冲，再逐张 PNG→raw→贴图→立刻删临时文件（避免 raw/alpha 堆积撑爆 FS）
  logHeap("malloc-frame");
  uint16_t* frame = (uint16_t*)malloc(FRAME_RGB565_BYTES);
  if (!frame) {
    Serial.printf("frame malloc fail max=%u\n", ESP.getMaxAllocHeap());
    return false;
  }
  const uint16_t bg = backdropColor(lcd);
  for (size_t i = 0; i < (size_t)LCD_WIDTH * LCD_HEIGHT; ++i) {
    frame[i] = bg;
  }

  char pngPath[48];
  char rawPath[48];
  char alphaPathBuf[48];

  auto stampOne = [&](bool isRadar, int tx, int ty, int sc,
                      bool withAlpha) -> bool {
    frameCacheTilePath(zoom, isRadar, tx, ty, pngPath, sizeof(pngPath));
    if (!LittleFS.exists(pngPath)) {
      return false;
    }
    snprintf(rawPath, sizeof(rawPath), "/frames/z%02d/%c_%d_%d.raw", zoom,
             isRadar ? 'r' : 'b', tx, ty);
    const char* ap = nullptr;
    if (withAlpha) {
      snprintf(alphaPathBuf, sizeof(alphaPathBuf), "/frames/z%02d/%c_%d_%d.a",
               zoom, isRadar ? 'r' : 'b', tx, ty);
      ap = alphaPathBuf;
    }
    logHeap(isRadar ? "decode-radar" : "decode-base");
    if (!decodePngToRawFile(pngPath, rawPath, ap)) {
      return false;
    }
    LittleFS.remove(pngPath);  // 解码后立刻释放 PNG

    const int ox =
        (int)lround((double)tx * TILE_SIZE * sc - vp.origin_px);
    const int oy =
        (int)lround((double)ty * TILE_SIZE * sc - vp.origin_py);
    const bool ok = frameCacheStampRawToBuffer(
        frame, rawPath, ap, ox, oy, sc, withAlpha);
    LittleFS.remove(rawPath);
    if (ap) {
      LittleFS.remove(ap);
    }
    return ok;
  };

  int stamped = 0;
  for (int ty = vp.ty0; ty <= vp.ty1; ++ty) {
    for (int tx = vp.tx0; tx <= vp.tx1; ++tx) {
      pollButtonDuringCompose();
      if (composeAbortRequested()) {
        free(frame);
        return false;
      }
      if (stampOne(false, tx, ty, 1, false)) {
        ++stamped;
      }
    }
  }
  if (radarOk > 0) {
    for (int ty = rty0; ty <= rty1; ++ty) {
      for (int tx = rtx0; tx <= rtx1; ++tx) {
        pollButtonDuringCompose();
        if (composeAbortRequested()) {
          free(frame);
          return false;
        }
        if (stampOne(true, tx, ty, scale, true)) {
          ++stamped;
        }
      }
    }
  }

  Serial.printf("Stamped tiles=%d fs used=%u/%u\n", stamped,
                (unsigned)LittleFS.usedBytes(),
                (unsigned)LittleFS.totalBytes());
  if (stamped == 0) {
    free(frame);
    return false;
  }

  frameCacheDrawCrosshairBuf(frame, lcd->color565(255, 255, 255));
  frameCacheDrawOverlayBuf(frame, haveRadarMeta ? meta.time : 0);

  if (pushToDisplay) {
    // 即使不 commit 也先刷一帧，避免接口抖动时黑屏
    lcd->setSwapBytes(true);
    lcd->pushImage(0, 0, LCD_WIDTH, LCD_HEIGHT, frame);
    lcd->setSwapBytes(false);
  }

  if (!allowCommit) {
    free(frame);
    Serial.println("compose display-only (no radar cache commit)");
    return pushToDisplay;
  }

  if (!frameCacheWriteRgb565(zoom, frame)) {
    Serial.println("writeRgb565 fail");
    free(frame);
    return false;
  }
  free(frame);

  if (!frameCacheCommit(zoom)) {
    Serial.println("commit fail");
    return false;
  }

  logHeap("compose-done");
  Serial.printf("compose done z=%d ok=1\n", zoom);
  return true;
}
