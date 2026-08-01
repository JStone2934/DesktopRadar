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
  uint16_t row[TILE_SIZE];
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
  // 与 LovyanGFX image_decoder_t::read_data 一致：skip 时仍返回请求长度
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

static void pngFlushRow(PngDecodeCtx* ctx) {
  if (!ctx->rowDirty || !ctx->rawOut) {
    return;
  }
  ctx->rawOut->write(reinterpret_cast<uint8_t*>(ctx->row), TILE_SIZE * 2);
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
    // 若首行 y>0，先写空行
    while (ctx->lastY < y && ctx->lastY < (uint32_t)TILE_SIZE) {
      ctx->rawOut->write(reinterpret_cast<uint8_t*>(ctx->row), TILE_SIZE * 2);
      ctx->lastY++;
    }
  }
  if (y != ctx->lastY) {
    pngFlushRow(ctx);
    memset(ctx->row, 0, sizeof(ctx->row));
    while (ctx->lastY + 1 < y && ctx->lastY + 1 < (uint32_t)TILE_SIZE) {
      ctx->rawOut->write(reinterpret_cast<uint8_t*>(ctx->row), TILE_SIZE * 2);
      ctx->lastY++;
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

static bool decodePngToRawFile(const char* pngPath, const char* rawPath) {
  File in = LittleFS.open(pngPath, "r");
  if (!in || in.size() < 33 || !pngPathLooksComplete(pngPath)) {
    Serial.printf("  png incomplete/missing %s sz=%u\n", pngPath,
                  in ? (unsigned)in.size() : 0);
    if (in) {
      in.close();
    }
    return false;
  }
  // 重新打开（校验已关闭原 handle）
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
  in.close();  // 避免与 raw 写入并发占用 LittleFS

  LittleFS.remove(rawPath);
  File raw = LittleFS.open(rawPath, "w");
  if (!raw) {
    free(pngData);
    return false;
  }

  logHeap("pngle-before");
  pngle_t* pngle = lgfx_pngle_new();
  if (!pngle) {
    Serial.printf("  pngle_new fail max=%u\n", ESP.getMaxAllocHeap());
    free(pngData);
    raw.close();
    return false;
  }

  PngDecodeCtx ctx{};
  ctx.data = pngData;
  ctx.len = pngLen;
  ctx.pos = 0;
  ctx.rawOut = &raw;
  ctx.lastY = 0;
  ctx.rowDirty = false;
  ctx.started = false;
  ctx.pixels = 0;
  memset(ctx.row, 0, sizeof(ctx.row));

  if (lgfx_pngle_prepare(pngle, pngReadCb, &ctx) < 0) {
    Serial.println("  pngle_prepare fail");
    lgfx_pngle_destroy(pngle);
    free(pngData);
    raw.close();
    LittleFS.remove(rawPath);
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
    return false;
  }

  const int rc = lgfx_pngle_decomp(pngle, pngDrawCb);
  pngFlushRow(&ctx);
  memset(ctx.row, 0, sizeof(ctx.row));
  uint32_t rowsWritten = ctx.started ? (ctx.lastY + 1) : 0;
  while (rowsWritten < (uint32_t)TILE_SIZE) {
    raw.write(reinterpret_cast<uint8_t*>(ctx.row), TILE_SIZE * 2);
    rowsWritten++;
  }

  lgfx_pngle_destroy(pngle);
  free(pngData);
  raw.flush();
  const size_t sz = raw.size();
  raw.close();

  if (rc < 0 || sz != (size_t)TILE_SIZE * TILE_SIZE * 2 || ctx.pixels == 0) {
    Serial.printf("  png decomp fail rc=%d sz=%u w=%d h=%d pix=%u\n", rc,
                  (unsigned)sz, w, h, (unsigned)ctx.pixels);
    LittleFS.remove(rawPath);
    return false;
  }
  Serial.printf("  png ok %dx%d pix=%u\n", w, h, (unsigned)ctx.pixels);
  return true;
}

static bool loadRawTile(const char* rawPath, uint16_t* tileBuf) {
  File f = LittleFS.open(rawPath, "r");
  if (!f || f.size() != (size_t)TILE_SIZE * TILE_SIZE * 2) {
    if (f) {
      f.close();
    }
    return false;
  }
  const size_t got =
      f.read(reinterpret_cast<uint8_t*>(tileBuf), TILE_SIZE * TILE_SIZE * 2);
  f.close();
  return got == (size_t)TILE_SIZE * TILE_SIZE * 2;
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

  // 先全部 PNG→raw（此时不占 112KB 帧），再 malloc 帧一次性贴图
  char pngPath[48];
  char rawPath[48];
  struct PendingStamp {
    char raw[48];
    int ox;
    int oy;
    int scale;
    bool alpha;
  };
  PendingStamp pending[16];
  int pendingN = 0;

  auto queueRaw = [&](bool isRadar, int tx, int ty, int sc, bool alpha) -> bool {
    if (pendingN >= (int)(sizeof(pending) / sizeof(pending[0]))) {
      return false;
    }
    frameCacheTilePath(zoom, isRadar, tx, ty, pngPath, sizeof(pngPath));
    if (!LittleFS.exists(pngPath)) {
      return false;
    }
    snprintf(rawPath, sizeof(rawPath), "/frames/z%02d/%c_%d_%d.raw", zoom,
             isRadar ? 'r' : 'b', tx, ty);
    logHeap(isRadar ? "decode-radar" : "decode-base");
    if (!decodePngToRawFile(pngPath, rawPath)) {
      return false;
    }
    PendingStamp& p = pending[pendingN++];
    strncpy(p.raw, rawPath, sizeof(p.raw) - 1);
    p.raw[sizeof(p.raw) - 1] = 0;
    p.ox = (int)lround((double)tx * TILE_SIZE * sc - vp.origin_px);
    p.oy = (int)lround((double)ty * TILE_SIZE * sc - vp.origin_py);
    p.scale = sc;
    p.alpha = alpha;
    return true;
  };

  for (int ty = vp.ty0; ty <= vp.ty1; ++ty) {
    for (int tx = vp.tx0; tx <= vp.tx1; ++tx) {
      pollButtonDuringCompose();
      if (composeAbortRequested()) {
        return false;
      }
      queueRaw(false, tx, ty, 1, false);
    }
  }
  if (radarOk > 0) {
    for (int ty = rty0; ty <= rty1; ++ty) {
      for (int tx = rtx0; tx <= rtx1; ++tx) {
        pollButtonDuringCompose();
        if (composeAbortRequested()) {
          return false;
        }
        queueRaw(true, tx, ty, scale, true);
      }
    }
  }

  if (pendingN == 0) {
    Serial.println("Stamped tiles=0");
    return false;
  }

  logHeap("malloc-frame");
  uint16_t* frame = (uint16_t*)malloc(FRAME_RGB565_BYTES);
  if (!frame) {
    Serial.printf("frame malloc fail max=%u\n", ESP.getMaxAllocHeap());
    for (int i = 0; i < pendingN; ++i) {
      LittleFS.remove(pending[i].raw);
    }
    return false;
  }
  const uint16_t bg = backdropColor(lcd);
  for (size_t i = 0; i < (size_t)LCD_WIDTH * LCD_HEIGHT; ++i) {
    frame[i] = bg;
  }

  int stamped = 0;
  for (int i = 0; i < pendingN; ++i) {
    pollButtonDuringCompose();
    if (composeAbortRequested()) {
      for (int j = i; j < pendingN; ++j) {
        LittleFS.remove(pending[j].raw);
      }
      free(frame);
      return false;
    }
    if (frameCacheStampRawToBuffer(frame, pending[i].raw, pending[i].ox,
                                   pending[i].oy, pending[i].scale,
                                   pending[i].alpha)) {
      ++stamped;
    }
    LittleFS.remove(pending[i].raw);
  }

  Serial.printf("Stamped tiles=%d\n", stamped);
  if (stamped == 0) {
    free(frame);
    return false;
  }

  frameCacheDrawCrosshairBuf(frame, lcd->color565(255, 255, 255));

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
