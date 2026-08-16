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

static ComposeProgressFn s_progressFn = nullptr;
static ComposeDisplayFn s_displayFn = nullptr;
static bool s_crosshairVisible = true;
// 仅在后台造片时使用；静态分配避免在 38.4KB 分段帧已分配后产生堆碎片。
static RadarColorGrid s_radarColorGrid;

void composeSetProgressFn(ComposeProgressFn fn) { s_progressFn = fn; }
void composeSetDisplayFn(ComposeDisplayFn fn) { s_displayFn = fn; }
void composeSetCrosshairVisible(bool visible) { s_crosshairVisible = visible; }

static void reportComposeProgress(int zoom, float local01) {
  if (!s_progressFn) {
    return;
  }
  if (local01 < 0.0f) {
    local01 = 0.0f;
  }
  if (local01 > 1.0f) {
    local01 = 1.0f;
  }
  s_progressFn(zoom, local01);
}

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
    if (WiFi.status() != WL_CONNECTED) {
      Serial.printf("  %s stop: WiFi offline\n", tag);
      return false;
    }
    size_t len = 0;
    File tileOut;
    if (!frameCacheOpenTileWrite(zoom, isRadar, tx, ty, &tileOut)) {
      Serial.printf("  %s open tile file fail try=%d\n", tag, attempt);
      return false;
    }
    const bool fetched = httpFetchToFile(url.c_str(), referer, tileOut,
                                         HTTP_MAX_TILE_BYTES, &len,
                                         TILE_TIMEOUT_MS);
    tileOut.close();
    char tilePath[48];
    frameCacheTilePath(zoom, isRadar, tx, ty, tilePath, sizeof(tilePath));
    if (!fetched || len < 8) {
      Serial.printf("  %s http fail len=%u try=%d\n", tag, (unsigned)len, attempt);
      LittleFS.remove(tilePath);
      if (composeAbortRequested()) {
        return false;
      }
      if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("  %s stop retry: WiFi offline\n", tag);
        return false;
      }
      const uint32_t waitUntil = millis() + 200UL * attempt;
      while ((int32_t)(millis() - waitUntil) < 0) {
        inputServiceDuringBlock();
        if (composeAbortRequested()) {
          return false;
        }
        delay(10);
      }
      continue;
    }
    if (!pngPathLooksComplete(tilePath)) {
      Serial.printf("  %s invalid png len=%u\n", tag, (unsigned)len);
      LittleFS.remove(tilePath);
      return false;
    }
    if (len < minBytes) {
      Serial.printf("  %s small png len=%u accepted\n", tag, (unsigned)len);
    }
    Serial.printf("  %s saved %u bytes\n", tag, (unsigned)len);
    const uint32_t settleUntil = millis() + 40UL;
    while ((int32_t)(millis() - settleUntil) < 0) {
      inputServiceDuringBlock();
      if (composeAbortRequested()) {
        return false;
      }
      delay(5);
    }
    return true;
  }
  return false;
}

struct PngDecodeCtx {
  File* in;  // 流式读 PNG，避免整文件进 RAM 撑爆 maxAlloc
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
  if (!ctx || !ctx->in || composeAbortRequested()) {
    return 0;
  }
  if (!buf) {
    // pngle 用空 buf 表示跳过
    const size_t pos = ctx->in->position();
    ctx->in->seek(pos + len);
    return len;
  }
  const int n = ctx->in->read(buf, len);
  return n > 0 ? (uint32_t)n : 0;
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
  if (!ctx || composeAbortRequested()) {
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
  in.seek(0);

  LittleFS.remove(rawPath);
  if (alphaPath) {
    LittleFS.remove(alphaPath);
  }
  File raw = LittleFS.open(rawPath, "w");
  if (!raw) {
    in.close();
    return false;
  }
  File alphaFile;
  File* alphaPtr = nullptr;
  if (alphaPath) {
    alphaFile = LittleFS.open(alphaPath, "w");
    if (!alphaFile) {
      in.close();
      raw.close();
      LittleFS.remove(rawPath);
      return false;
    }
    alphaPtr = &alphaFile;
  }

  // 给 pngle 腾连续堆：先丢掉其它临时碎片机会
  logHeap("pngle-before");
  pngle_t* pngle = lgfx_pngle_new();
  if (!pngle) {
    // 再试一次：短延时后重试（偶发碎片）
    delay(20);
    pngle = lgfx_pngle_new();
  }
  if (!pngle) {
    Serial.printf("  pngle_new fail max=%u\n", ESP.getMaxAllocHeap());
    in.close();
    raw.close();
    if (alphaPtr) {
      alphaPtr->close();
      LittleFS.remove(alphaPath);
    }
    LittleFS.remove(rawPath);
    return false;
  }

  PngDecodeCtx ctx{};
  ctx.in = &in;
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
    in.close();
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
    in.close();
    raw.close();
    LittleFS.remove(rawPath);
    if (alphaPtr) {
      alphaPtr->close();
      LittleFS.remove(alphaPath);
    }
    return false;
  }

  const int rc = lgfx_pngle_decomp(pngle, pngDrawCb);
  if (composeAbortRequested()) {
    lgfx_pngle_destroy(pngle);
    in.close();
    raw.close();
    LittleFS.remove(rawPath);
    if (alphaPtr) {
      alphaPtr->close();
      LittleFS.remove(alphaPath);
    }
    return false;
  }
  pngFlushRow(&ctx);
  uint32_t rowsWritten = ctx.started ? (ctx.lastY + 1) : 0;
  if (rowsWritten < (uint32_t)TILE_SIZE) {
    pngWritePadRows(&ctx, (uint32_t)TILE_SIZE - rowsWritten);
  }

  lgfx_pngle_destroy(pngle);
  in.close();
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

ComposeResult composeRadarFrame(LGFX* lcd, float lat, float lon, int zoom,
                                bool pushToDisplay) {
  if (!lcd) {
    return ComposeResult::Failed;
  }
  if (!zoomCanCompose(zoom)) {
    Serial.printf("compose reject zoom=%d\n", zoom);
    return ComposeResult::Failed;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("compose z%d skipped: WiFi offline\n", zoom);
    return ComposeResult::Failed;
  }

  logHeap("compose-start");
  composeClearAbort();

  // 先取时间再动工作区。已有可信成品且时间未变化时，不清临时目录、
  // 不下载、不写盘，也不触碰屏幕和风场轨迹。
  RainviewerFrame meta;
  const bool haveRadarMeta = rainviewerFetchLatest(&meta);
  if (composeAbortRequested()) {
    return ComposeResult::Failed;
  }
  if (!haveRadarMeta || meta.time == 0) {
    Serial.printf("compose z%d failed: RainViewer metadata unavailable\n", zoom);
    return ComposeResult::Failed;
  }
  if (frameCacheHas(zoom)) {
    uint32_t cachedTime = 0;
    if (frameCacheReadRadarTime(zoom, &cachedTime) &&
        cachedTime == meta.time) {
      Serial.printf("compose unchanged z%d radar_t=%lu\n", zoom,
                    (unsigned long)meta.time);
      return ComposeResult::Unchanged;
    }
  }
  logHeap("after-meta");

  if (!frameCachePrepare(zoom)) {
    Serial.println("frameCachePrepare fail");
    return ComposeResult::Failed;
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

  // 不中途黑屏：已有画面时保持 LCD；仅结束时 pushImage 替换
  reportComposeProgress(zoom, 0.02f);

  // 先腾出其它档临时文件，再下载，避免后下的底图因 Flash 满失败
  frameCacheScrubOrphansExcept(zoom);
  if (composeAbortRequested()) {
    return ComposeResult::Failed;
  }
  // 三个临时帧带 + 单张压缩瓦片 + 单张 radar raw/alpha，并保留
  // LittleFS 元数据余量。逐瓦片删除 PNG 后才改写帧带，因此 PNG 峰值与
  // 帧带写时复制峰值不会叠加；任何时候都不删除其它档的正式缓存。
  constexpr size_t kBakeWorkspaceBytes =
      FRAME_RGB565_BYTES + HTTP_MAX_TILE_BYTES +
      (size_t)TILE_SIZE * TILE_SIZE * 3U + 48U * 1024U;
  if (!frameCacheEnsureBakeSpace(kBakeWorkspaceBytes, zoom)) {
    Serial.printf("compose z%d deferred: keep completed zoom caches\n", zoom);
    return ComposeResult::Failed;
  }
  reportComposeProgress(zoom, 0.06f);

  const int baseTiles =
      (vp.tx1 - vp.tx0 + 1) * (vp.ty1 - vp.ty0 + 1);
  const int radarTiles =
      haveRadarMeta ? (rtx1 - rtx0 + 1) * (rty1 - rty0 + 1) : 0;
  const int workTotal = baseTiles + radarTiles;
  int workDone = 0;
  auto afterTile = [&]() {
    ++workDone;
    const float frac =
        workTotal > 0 ? (float)workDone / (float)workTotal : 1.0f;
    reportComposeProgress(zoom, 0.06f + 0.87f * frac);
  };

  auto fetchBasemapOnce = [&](int tx, int ty) -> bool {
    Serial.printf("  dl basemap z=%d x=%d y=%d\n", zoom, tx, ty);
    return fetchTileToFs(zoom, false, tx, ty, basemapTileUrl(zoom, tx, ty),
                         AMAP_REFERER, 800, "basemap");
  };

  char pngPath[48];
  char rawPath[48];
  char alphaPathBuf[48];

  auto cleanupTile = [&](bool isRadar, int tx, int ty, bool withAlpha) {
    frameCacheTilePath(zoom, isRadar, tx, ty, pngPath, sizeof(pngPath));
    LittleFS.remove(pngPath);
    snprintf(rawPath, sizeof(rawPath), "/frames/z%02d/%c_%d_%d.raw", zoom,
             isRadar ? 'r' : 'b', tx, ty);
    LittleFS.remove(rawPath);
    if (withAlpha) {
      snprintf(alphaPathBuf, sizeof(alphaPathBuf), "/frames/z%02d/%c_%d_%d.a",
               zoom, isRadar ? 'r' : 'b', tx, ty);
      LittleFS.remove(alphaPathBuf);
    }
  };

  // 解码成功后立刻删除 PNG，再申请帧带并改写 LittleFS，避免压缩图与
  // 写时复制峰值重叠。
  auto decodeOne = [&](bool isRadar, int tx, int ty, bool withAlpha) -> bool {
    frameCacheTilePath(zoom, isRadar, tx, ty, pngPath, sizeof(pngPath));
    if (!LittleFS.exists(pngPath)) {
      Serial.printf("  missing png %s\n", pngPath);
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
    if (!LittleFS.remove(pngPath) && LittleFS.exists(pngPath)) {
      Serial.printf("  png cleanup fail %s\n", pngPath);
      return false;
    }
    return true;
  };

  constexpr int kComposeBandRows = FRAME_COMPOSE_BAND_ROWS;
  const size_t composeBandBytes =
      (size_t)LCD_WIDTH * kComposeBandRows * sizeof(uint16_t);

  const uint16_t bg = backdropColor(lcd);
  uint16_t* frameBand = nullptr;

  auto allocFrameBand = [&]() -> bool {
    logHeap("malloc-frame-band");
    frameBand = (uint16_t*)malloc(composeBandBytes);
    if (!frameBand) {
      Serial.printf("frame band malloc fail need=%u max=%u\n",
                    (unsigned)composeBandBytes, ESP.getMaxAllocHeap());
      return false;
    }
    return true;
  };

  struct BandGuard {
    int zoom;
    bool active;
    ~BandGuard() {
      if (active) frameCacheRemoveComposeBands(zoom);
    }
  } bandGuard{zoom, true};

  // 三个独立小文件代替一个反复改写的大文件；每段写后立即读回比对。
  if (!allocFrameBand()) {
    return ComposeResult::Failed;
  }
  for (int band = 0; band < FRAME_COMPOSE_BAND_COUNT; ++band) {
    const int bandY = band * kComposeBandRows;
    const int bandRows = min(kComposeBandRows, LCD_HEIGHT - bandY);
    const size_t bandPixels = (size_t)LCD_WIDTH * bandRows;
    for (size_t i = 0; i < bandPixels; ++i) {
      frameBand[i] = bg;
    }
    if (!frameCacheWriteComposeBand(zoom, band, frameBand)) {
      Serial.printf("init compose band fail b%d\n", band);
      free(frameBand);
      return ComposeResult::Failed;
    }
  }
  free(frameBand);
  frameBand = nullptr;

  RadarCenterSample centerSample;
  radarCenterSampleReset(&centerSample);
  memset(&s_radarColorGrid, 0, sizeof(s_radarColorGrid));

  auto bakeOneTile = [&](bool isRadar, int tx, int ty, int sc,
                         bool withAlpha, RadarCenterSample* centerOut) -> bool {
    // 此时 frameBand 必须为空，确保 pngle 能取得最大的连续堆块。
    if (frameBand || !decodeOne(isRadar, tx, ty, withAlpha)) {
      cleanupTile(isRadar, tx, ty, withAlpha);
      return false;
    }
    logHeap("after-png-decode");
    if (!allocFrameBand()) {
      cleanupTile(isRadar, tx, ty, withAlpha);
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
    const int ox = (int)lround((double)tx * TILE_SIZE * sc - vp.origin_px);
    const int oy = (int)lround((double)ty * TILE_SIZE * sc - vp.origin_py);
    const int outBottom = oy + TILE_SIZE * sc;
    bool ok = true;
    for (int band = 0; band < FRAME_COMPOSE_BAND_COUNT; ++band) {
      const int bandY = band * kComposeBandRows;
      const int bandRows = min(kComposeBandRows, LCD_HEIGHT - bandY);
      if (oy >= bandY + bandRows || outBottom <= bandY) {
        continue;
      }
      if (!frameCacheReadComposeBand(zoom, band, frameBand) ||
          !frameCacheStampRawToBand(
              frameBand, bandY, bandRows, rawPath, ap, ox, oy, sc, withAlpha,
              centerOut, withAlpha ? &s_radarColorGrid : nullptr) ||
          !frameCacheWriteComposeBand(zoom, band, frameBand)) {
        Serial.printf("  tile stamp FAIL z=%d type=%c x=%d y=%d bandY=%d\n",
                      zoom, isRadar ? 'r' : 'b', tx, ty, bandY);
        ok = false;
        break;
      }
    }
    free(frameBand);
    frameBand = nullptr;
    cleanupTile(isRadar, tx, ty, withAlpha);
    return ok;
  };

  int baseOk = 0;
  int baseStamped = 0;
  for (int ty = vp.ty0; ty <= vp.ty1; ++ty) {
    for (int tx = vp.tx0; tx <= vp.tx1; ++tx) {
      if (composeAbortRequested()) return ComposeResult::Failed;
      bool fetched = fetchBasemapOnce(tx, ty);
      if (!fetched && WiFi.status() == WL_CONNECTED) {
        Serial.printf("  retry basemap z=%d x=%d y=%d\n", zoom, tx, ty);
        fetched = fetchBasemapOnce(tx, ty);
      }
      if (!fetched || !bakeOneTile(false, tx, ty, 1, false, nullptr)) {
        cleanupTile(false, tx, ty, false);
        Serial.printf("basemap pipeline fail z=%d x=%d y=%d\n", zoom, tx, ty);
        return ComposeResult::Failed;
      }
      ++baseOk;
      ++baseStamped;
      afterTile();
    }
  }
  Serial.printf("Basemap pipeline ok=%d/%d\n", baseOk, baseTiles);

  int radarOk = 0;
  int radarStamped = 0;
  if (haveRadarMeta && !composeAbortRequested()) {
    for (int ty = rty0; ty <= rty1; ++ty) {
      for (int tx = rtx0; tx <= rtx1; ++tx) {
        if (composeAbortRequested()) return ComposeResult::Failed;
        Serial.printf("  dl radar oz=%d x=%d y=%d\n", overlayZoom, tx, ty);
        const bool fetched = fetchTileToFs(
            zoom, true, tx, ty, rainviewerTileUrl(meta, overlayZoom, tx, ty),
            nullptr, 200, "radar");
        if (fetched) {
          if (!bakeOneTile(true, tx, ty, scale, true, &centerSample)) {
            cleanupTile(true, tx, ty, true);
            Serial.printf("radar pipeline fail z=%d x=%d y=%d\n", zoom, tx,
                          ty);
            return ComposeResult::Failed;
          }
          ++radarOk;
          ++radarStamped;
        }
        afterTile();
      }
    }
  }
  Serial.printf("Radar pipeline ok=%d/%d\n", radarOk, radarTiles);
  if (baseOk != baseTiles || baseStamped != baseTiles || radarOk == 0 ||
      radarStamped != radarOk) {
    Serial.printf("pipeline incomplete base=%d/%d radar=%d/%d\n", baseStamped,
                  baseTiles, radarStamped, radarTiles);
    return ComposeResult::Failed;
  }

  const bool allowCommit = haveRadarMeta && radarOk > 0;
  if (!frameCacheWriteMeta(zoom, vp, true, overlayZoom, scale, rtx0, rty0,
                           rtx1, rty1)) {
    Serial.println("write compose meta fail");
    return ComposeResult::Failed;
  }

  // 所有瓦片完成后才顺序拼成 .new；每段只追加一次，避免大文件反复
  // 写时复制。写后先校验，再允许上屏和 commit。
  if (!frameCacheBeginRgb565New(zoom)) {
    Serial.println("beginRgb565New fail");
    return ComposeResult::Failed;
  }
  if (!allocFrameBand()) {
    return ComposeResult::Failed;
  }
  for (int band = 0; band < FRAME_COMPOSE_BAND_COUNT; ++band) {
    const int bandY = band * kComposeBandRows;
    const int bandRows = min(kComposeBandRows, LCD_HEIGHT - bandY);
    if (!frameCacheReadComposeBand(zoom, band, frameBand)) {
      Serial.printf("final scratch read fail b%d\n", band);
      free(frameBand);
      return ComposeResult::Failed;
    }
    if (s_crosshairVisible) {
      frameCacheDrawCrosshairBand(frameBand, bandY, bandRows,
                                  lcd->color565(255, 255, 255));
    }
    frameCacheDrawOverlayBand(frameBand, bandY, bandRows, meta.time);
    if (!frameCacheWriteRgb565Band(zoom, bandY, bandRows, frameBand)) {
      Serial.printf("final band write fail y=%d rows=%d\n", bandY, bandRows);
      free(frameBand);
      return ComposeResult::Failed;
    }
  }

  if (!frameCacheValidateRgb565New(zoom, bg)) {
    free(frameBand);
    return ComposeResult::Failed;
  }
  frameCacheRemoveComposeBands(zoom);
  bandGuard.active = false;

  if (pushToDisplay) {
    const bool prevSwap = lcd->getSwapBytes();
    lcd->setSwapBytes(true);
    bool displayOk = true;
    for (int band = 0; band < FRAME_COMPOSE_BAND_COUNT; ++band) {
      const int bandY = band * kComposeBandRows;
      const int bandRows = min(kComposeBandRows, LCD_HEIGHT - bandY);
      if (!frameCacheReadRgb565NewBand(zoom, bandY, bandRows, frameBand)) {
        displayOk = false;
        break;
      }
      lcd->pushImage(0, bandY, LCD_WIDTH, bandRows, frameBand);
    }
    lcd->setSwapBytes(prevSwap);
    if (!displayOk) {
      free(frameBand);
      return ComposeResult::Failed;
    }
    if (s_displayFn) {
      s_displayFn(zoom);
    }
  }
  free(frameBand);
  frameBand = nullptr;

  const int stamped = baseStamped + radarStamped;

  Serial.printf("Stamped tiles=%d (base=%d/%d) fs used=%u/%u\n", stamped,
                baseStamped, baseTiles, (unsigned)LittleFS.usedBytes(),
                (unsigned)LittleFS.totalBytes());

  reportComposeProgress(zoom, 0.97f);

  if (!allowCommit) {
    Serial.println("compose display-only (no radar cache commit)");
    return ComposeResult::Failed;
  }

  // commit 会替换正式文件；进入这个很短的原子阶段前最后一次让按键抢占。
  inputServiceDuringBlock();
  if (composeAbortRequested()) {
    Serial.printf("compose z%d preempted before commit\n", zoom);
    return ComposeResult::Failed;
  }

  if (!frameCacheCommit(zoom)) {
    Serial.println("commit fail");
    return ComposeResult::Failed;
  }

  // 新图 ready 后再提交边车；时间最后写，只有图像和预警均完整时才成为
  // 下一轮“时间相同可跳过”的可信标记。旧 4B 时间也在此惰性迁移。
  frameCacheRemoveRadarTime(zoom);
  frameCacheRemoveAlert(zoom);
  frameCacheRemoveRadarColorGrid(zoom);
  bool hasCloud = false;
  uint16_t alertColor = 0;
  radarCenterSampleFinalize(&centerSample, &hasCloud, &alertColor);
  const bool alertOk = frameCacheWriteAlert(zoom, hasCloud, alertColor);
  const bool colorGridOk =
      frameCacheWriteRadarColorGrid(zoom, &s_radarColorGrid, meta.time);
  Serial.printf("center alert hasCloud=%d color=%04x maxA=%u ok=%d\n",
                (int)hasCloud, (unsigned)alertColor,
                (unsigned)centerSample.maxAlpha, (int)alertOk);
  Serial.printf("radar color grid z%d bytes=%u ok=%d\n", zoom,
                (unsigned)(sizeof(s_radarColorGrid) + 12U),
                (int)colorGridOk);
  if (alertOk && colorGridOk && meta.time != 0 &&
      !frameCacheWriteRadarTime(zoom, meta.time)) {
    Serial.printf("radar time commit fail z%d t=%lu\n", zoom,
                  (unsigned long)meta.time);
  }

  reportComposeProgress(zoom, 1.0f);
  logHeap("compose-done");
  Serial.printf("compose done z=%d ok=1\n", zoom);
  return ComposeResult::Updated;
}
