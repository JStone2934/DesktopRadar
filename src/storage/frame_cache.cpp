#include "frame_cache.h"

#include "button.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// bit0 = ZOOM_MIN … 避免每秒 open 缺失 ready 刷屏
static uint16_t s_readyMask = 0;
// 新鲜度掩码：bit=1 表示该档为本轮已重建（新雷达）；ready 仅表示成品存在（可能过时）
static uint16_t s_freshMask = 0;
static int s_protectedZoom = -1;

static constexpr int kBlitBandRows = 16;
static uint16_t s_blitBand[LCD_WIDTH * kBlitBandRows];

static inline void freshMaskSet(int zoom, bool on) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return;
  }
  const uint16_t bit = (uint16_t)(1u << (zoom - ZOOM_MIN));
  if (on) {
    s_freshMask |= bit;
  } else {
    s_freshMask &= (uint16_t)~bit;
  }
}

static inline bool freshMaskGet(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }
  return (s_freshMask & (uint16_t)(1u << (zoom - ZOOM_MIN))) != 0;
}

static inline int zoomBit(int zoom) { return zoom - ZOOM_MIN; }

static void readyMaskSet(int zoom, bool on) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return;
  }
  const uint16_t bit = (uint16_t)(1u << zoomBit(zoom));
  if (on) {
    s_readyMask |= bit;
  } else {
    s_readyMask &= (uint16_t)~bit;
  }
}

static bool readyMaskGet(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }
  return (s_readyMask & (uint16_t)(1u << zoomBit(zoom))) != 0;
}

static void dirPath(int zoom, char* out, size_t n) {
  snprintf(out, n, "/frames/z%02d", zoom);
}

static void readyPath(int zoom, char* out, size_t n) {
  // 与 rgb565 同级，避免 commit 清理临时目录后无法创建
  snprintf(out, n, "/frames/z%02d.ready", zoom);
}

static void metaPath(int zoom, char* out, size_t n) {
  snprintf(out, n, "/frames/z%02d/meta.txt", zoom);
}

static void rgbPath(int zoom, char* out, size_t n) {
  snprintf(out, n, "/frames/z%02d.rgb565", zoom);
}

static void rgbNewPath(int zoom, char* out, size_t n) {
  snprintf(out, n, "/frames/z%02d.rgb565.new", zoom);
}

static void alertPath(int zoom, char* out, size_t n) {
  // 与 rgb565 / ready 同级，commit 清 zXX/ 临时目录后仍保留
  snprintf(out, n, "/frames/z%02d.alert", zoom);
}

static void radarTimePath(int zoom, char* out, size_t n) {
  snprintf(out, n, "/frames/z%02d.time", zoom);
}

static void radarTimeNewPath(int zoom, char* out, size_t n) {
  snprintf(out, n, "/frames/z%02d.time.new", zoom);
}

/** 打开并校验成品长度；成功时返回已打开的 File（调用方关闭）。 */
static bool openRgb565IfValid(int zoom, File* out) {
  if (!out) {
    return false;
  }
  char path[40];
  rgbPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f || f.size() != FRAME_RGB565_BYTES) {
    if (f) {
      f.close();
    }
    return false;
  }
  *out = f;
  return true;
}

static bool blitRgb565File(LGFX* lcd, File& f) {
  const bool prevSwap = lcd->getSwapBytes();
  lcd->setSwapBytes(true);
  for (int y = 0; y < LCD_HEIGHT; y += kBlitBandRows) {
    // 每批前后都采样按键；16 行传输远短于消抖阈值，仍可覆盖快速短按。
    buttonService();
    const int rows = min(kBlitBandRows, LCD_HEIGHT - y);
    const size_t bytes = (size_t)rows * FRAME_ROW_BYTES;
    if (f.read(reinterpret_cast<uint8_t*>(s_blitBand), bytes) != (int)bytes) {
      lcd->setSwapBytes(prevSwap);
      return false;
    }
    lcd->pushImage(0, y, LCD_WIDTH, rows, s_blitBand);
    buttonService();
  }
  lcd->setSwapBytes(prevSwap);
  return true;
}

void frameCacheTilePath(int zoom, bool isRadar, int tx, int ty, char* out,
                        size_t outLen) {
  snprintf(out, outLen, "/frames/z%02d/%c_%d_%d.png", zoom, isRadar ? 'r' : 'b',
           tx, ty);
}

static void removeTilesInRange(int zoom, bool isRadar, int tx0, int ty0, int tx1,
                               int ty1) {
  char path[48];
  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      frameCacheTilePath(zoom, isRadar, tx, ty, path, sizeof(path));
      LittleFS.remove(path);
    }
  }
}

static void scrubTemp(int zoom) {
  char path[48];
  readyPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  metaPath(zoom, path, sizeof(path));

  Viewport vp{};
  bool haveRadar = false;
  int overlayZoom = 0, scale = 1;
  int rtx0 = 0, rty0 = 0, rtx1 = 0, rty1 = 0;
  if (frameCacheReadMeta(zoom, &vp, &haveRadar, &overlayZoom, &scale, &rtx0,
                         &rty0, &rtx1, &rty1)) {
    removeTilesInRange(zoom, false, vp.tx0, vp.ty0, vp.tx1, vp.ty1);
    if (haveRadar) {
      removeTilesInRange(zoom, true, rtx0, rty0, rtx1, rty1);
    }
  }
  LittleFS.remove(path);
  char dir[32];
  dirPath(zoom, dir, sizeof(dir));
  // 尽力删除目录内残留
  File d = LittleFS.open(dir);
  if (d && d.isDirectory()) {
    File f = d.openNextFile();
    while (f) {
      char child[64];
      snprintf(child, sizeof(child), "%s/%s", dir, f.name());
      f.close();
      LittleFS.remove(child);
      f = d.openNextFile();
    }
  }
  if (d) {
    d.close();
  }
  LittleFS.rmdir(dir);
}

/** 只清临时瓦片目录/meta，保留已 commit 的 rgb565 + ready（回收失败造片残留）。 */
static void scrubTempDirKeepReady(int zoom) {
  char path[40];
  metaPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  char dir[32];
  dirPath(zoom, dir, sizeof(dir));
  File d = LittleFS.open(dir);
  if (d && d.isDirectory()) {
    File f = d.openNextFile();
    while (f) {
      char child[64];
      snprintf(child, sizeof(child), "%s/%s", dir, f.name());
      f.close();
      LittleFS.remove(child);
      f = d.openNextFile();
    }
  }
  if (d) {
    d.close();
  }
  LittleFS.rmdir(dir);
}

void frameCacheScrubOrphans() {
  frameCacheScrubOrphansExcept(-1);
}

void frameCacheScrubOrphansExcept(int keepZoom) {
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; ++z) {
    if (z == keepZoom) {
      continue;
    }
    scrubTempDirKeepReady(z);
  }
  Serial.printf("LittleFS scrub orphans keep=z%d used=%u total=%u\n", keepZoom,
                (unsigned)LittleFS.usedBytes(),
                (unsigned)LittleFS.totalBytes());
}

bool frameCacheBegin() {
  s_readyMask = 0;
  s_freshMask = 0;
  if (LittleFS.begin(false)) {
    if (!LittleFS.exists("/frames")) {
      LittleFS.mkdir("/frames");
    }
    Serial.printf("LittleFS mounted total=%u used=%u\n",
                  (unsigned)LittleFS.totalBytes(),
                  (unsigned)LittleFS.usedBytes());
  } else {
    Serial.println("LittleFS mount fail, formatting...");
    if (!LittleFS.begin(true)) {
      Serial.println("LittleFS format+mount fail");
      return false;
    }
    LittleFS.mkdir("/frames");
    Serial.printf("LittleFS formatted total=%u\n",
                  (unsigned)LittleFS.totalBytes());
  }
  // 动画系统已下线：回收旧 /anim 目录释放空间
  if (LittleFS.exists("/anim")) {
    File ad = LittleFS.open("/anim");
    if (ad && ad.isDirectory()) {
      File ef = ad.openNextFile();
      while (ef) {
        char child[48];
        snprintf(child, sizeof(child), "/anim/%s", ef.name());
        const bool isDir = ef.isDirectory();
        ef.close();
        if (isDir) {
          File sd = LittleFS.open(child);
          if (sd && sd.isDirectory()) {
            File sf = sd.openNextFile();
            while (sf) {
              char gc[64];
              snprintf(gc, sizeof(gc), "%s/%s", child, sf.name());
              sf.close();
              LittleFS.remove(gc);
              sf = sd.openNextFile();
            }
          }
          if (sd) {
            sd.close();
          }
          LittleFS.rmdir(child);
        } else {
          LittleFS.remove(child);
        }
        ef = ad.openNextFile();
      }
    }
    if (ad) {
      ad.close();
    }
    LittleFS.rmdir("/anim");
    Serial.println("removed legacy /anim dir");
  }

  // 缓存世代：作废旧的无雷达成品
  int gen = -1;
  File gf = LittleFS.open("/frames/gen", "r");
  if (gf) {
    gen = gf.parseInt();
    gf.close();
  }
  if (gen != FRAME_CACHE_GEN) {
    Serial.printf("frame cache gen %d -> %d, wiping\n", gen, FRAME_CACHE_GEN);
    for (int z = ZOOM_MIN; z <= ZOOM_MAX; ++z) {
      frameCacheRemove(z);
    }
    File gw = LittleFS.open("/frames/gen", "w");
    if (gw) {
      gw.printf("%d\n", FRAME_CACHE_GEN);
      gw.close();
    }
  }

  // 回收失败造片残留的 png/raw/alpha，避免 LittleFS 撑满导致无法 commit
  frameCacheScrubOrphans();

  for (int z = ZOOM_MIN; z <= ZOOM_MAX; ++z) {
    char rpath[40];
    char fpath[40];
    readyPath(z, rpath, sizeof(rpath));
    rgbPath(z, fpath, sizeof(fpath));
    File r = LittleFS.open(rpath, "r");
    if (!r) {
      continue;
    }
    r.close();
    File f = LittleFS.open(fpath, "r");
    if (!f) {
      continue;
    }
    const bool ok = f.size() == FRAME_RGB565_BYTES;
    f.close();
    if (ok) {
      readyMaskSet(z, true);
      // 启动时把磁盘上有效成品视为新鲜（首次刷新前不重建）
      freshMaskSet(z, true);
    }
  }

  return true;
}

bool frameCacheHas(int zoom) {
  return readyMaskGet(zoom);
}

int frameCacheZoomSlots() {
  int n = ZOOM_MAX - ZOOM_MIN + 1;
  if (ZOOM_SKIP >= ZOOM_MIN && ZOOM_SKIP <= ZOOM_MAX) {
    --n;
  }
  return n;
}

int frameCacheCountReady() {
  int n = 0;
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; ++z) {
    if (z == ZOOM_SKIP) {
      continue;
    }
    if (readyMaskGet(z)) {
      ++n;
    }
  }
  return n;
}

bool frameCacheBlit(LGFX* lcd, int zoom) {
  if (!lcd || !frameCacheHas(zoom)) {
    return false;
  }
  File f;
  if (!openRgb565IfValid(zoom, &f)) {
    return false;
  }
  // 缓存内存放 native RGB565（与 color565 一致）；
  // LovyanGFX 默认将 uint16_t* 当作 swap565，必须 setSwapBytes(true)
  const bool ok = blitRgb565File(lcd, f);
  f.close();
  return ok;
}

bool frameCacheRestoreRect(LGFX* lcd, int zoom, int x, int y, int w, int h) {
  if (!lcd || w <= 0 || h <= 0) {
    return false;
  }
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x >= LCD_WIDTH || y >= LCD_HEIGHT || w <= 0 || h <= 0) {
    return false;
  }
  if (x + w > LCD_WIDTH) {
    w = LCD_WIDTH - x;
  }
  if (y + h > LCD_HEIGHT) {
    h = LCD_HEIGHT - y;
  }

  File f;
  if (!openRgb565IfValid(zoom, &f)) {
    return false;
  }
  const bool prevSwap = lcd->getSwapBytes();
  lcd->setSwapBytes(true);
  uint16_t row[LCD_WIDTH];
  for (int yy = 0; yy < h; ++yy) {
    const size_t ofs = ((size_t)(y + yy) * LCD_WIDTH + (size_t)x) *
                       sizeof(uint16_t);
    if (!f.seek(ofs) ||
        f.read(reinterpret_cast<uint8_t*>(row), (size_t)w * 2) != w * 2) {
      lcd->setSwapBytes(prevSwap);
      f.close();
      return false;
    }
    lcd->pushImage(x, y + yy, w, 1, row);
  }
  lcd->setSwapBytes(prevSwap);
  f.close();
  return true;
}

static inline bool crosshairPixelMask(int dx, int dy) {
  return (dx * dx + dy * dy <= 9) || (dy == 0 && abs(dx) <= 8) ||
         (dx == 0 && abs(dy) <= 8);
}

bool frameCacheHideCrosshair(LGFX* lcd, int zoom) {
  if (!lcd || zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }

  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  constexpr int kPad = 12;
  constexpr int kSide = kPad * 2 + 1;
  const int x0 = cx - kPad;
  const int y0 = cy - kPad;
  uint16_t rect[kSide * kSide];

  File f;
  if (!openRgb565IfValid(zoom, &f)) {
    return false;
  }
  for (int yy = 0; yy < kSide; ++yy) {
    const size_t ofs = ((size_t)(y0 + yy) * LCD_WIDTH + (size_t)x0) *
                       sizeof(uint16_t);
    if (!f.seek(ofs) ||
        f.read(reinterpret_cast<uint8_t*>(&rect[yy * kSide]),
               kSide * sizeof(uint16_t)) != kSide * 2) {
      f.close();
      return false;
    }
  }
  f.close();

  for (int yy = 0; yy < kSide; ++yy) {
    for (int xx = 0; xx < kSide; ++xx) {
      const int dx = xx - kPad;
      const int dy = yy - kPad;
      if (!crosshairPixelMask(dx, dy)) {
        continue;
      }

      int sx = xx;
      int sy = yy;
      if (abs(dx) >= abs(dy)) {
        sy += dy <= 0 ? -5 : 5;
      } else {
        sx += dx <= 0 ? -5 : 5;
      }
      if (sx < 0) sx = 0;
      if (sx >= kSide) sx = kSide - 1;
      if (sy < 0) sy = 0;
      if (sy >= kSide) sy = kSide - 1;

      if (crosshairPixelMask(sx - kPad, sy - kPad)) {
        bool found = false;
        for (int r = 1; r <= kPad && !found; ++r) {
          for (int oy = -r; oy <= r && !found; ++oy) {
            for (int ox = -r; ox <= r; ++ox) {
              if (abs(ox) != r && abs(oy) != r) {
                continue;
              }
              const int tx = xx + ox;
              const int ty = yy + oy;
              if (tx < 0 || tx >= kSide || ty < 0 || ty >= kSide ||
                  crosshairPixelMask(tx - kPad, ty - kPad)) {
                continue;
              }
              sx = tx;
              sy = ty;
              found = true;
              break;
            }
          }
        }
      }
      rect[yy * kSide + xx] = rect[sy * kSide + sx];
    }
  }

  const bool prevSwap = lcd->getSwapBytes();
  lcd->setSwapBytes(true);
  for (int yy = 0; yy < kSide; ++yy) {
    lcd->pushImage(x0, y0 + yy, kSide, 1, &rect[yy * kSide]);
  }
  lcd->setSwapBytes(prevSwap);
  return true;
}

bool frameCacheBlitUnderlay(LGFX* lcd, int zoom) {
  if (!lcd || zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }
  File f;
  if (!openRgb565IfValid(zoom, &f)) {
    return false;
  }
  const bool ok = blitRgb565File(lcd, f);
  f.close();
  return ok;
}

static inline int expand5(int v) { return (v << 3) | (v >> 2); }
static inline int expand6(int v) { return (v << 2) | (v >> 4); }

static inline uint16_t pack565(int r8, int g8, int b8) {
  if (r8 < 0) {
    r8 = 0;
  } else if (r8 > 255) {
    r8 = 255;
  }
  if (g8 < 0) {
    g8 = 0;
  } else if (g8 > 255) {
    g8 = 255;
  }
  if (b8 < 0) {
    b8 = 0;
  } else if (b8 > 255) {
    b8 = 255;
  }
  return (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
}

/** 8-bit 域 alpha 混合，避免 RGB565 绿通道偏多造成发绿。 */
static inline uint16_t blend565(uint16_t src, uint16_t dst, uint8_t a) {
  if (a == 0) {
    return dst;
  }
  if (a == 255) {
    return src;
  }
  const int inv = 255 - a;
  const int r = (expand5((src >> 11) & 0x1F) * a +
                 expand5((dst >> 11) & 0x1F) * inv + 127) /
                255;
  const int g = (expand6((src >> 5) & 0x3F) * a +
                 expand6((dst >> 5) & 0x3F) * inv + 127) /
                255;
  const int b =
      (expand5(src & 0x1F) * a + expand5(dst & 0x1F) * inv + 127) / 255;
  return pack565(r, g, b);
}

bool frameCachePaintAlertRing(LGFX* lcd, int zoom, uint16_t color565,
                              uint8_t alpha) {
  if (!lcd || zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }

  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  const int rOuter = (LCD_WIDTH / 2) - 1;
  const int rInner = rOuter - 3;

  if (alpha >= 255) {
    lcd->fillArc(cx, cy, rOuter, rInner, 0.0f, 360.0f, color565);
    return true;
  }

  File f;
  if (!openRgb565IfValid(zoom, &f)) {
    return false;
  }

  const int rO2 = rOuter * rOuter;
  const int rI2 = rInner * rInner;
  const int y0 = cy - rOuter;
  const int y1 = cy + rOuter;
  if (y0 > 0) {
    if (!f.seek((size_t)y0 * FRAME_ROW_BYTES)) {
      f.close();
      return false;
    }
  }

  uint16_t row[LCD_WIDTH];
  const bool prevSwap = lcd->getSwapBytes();
  lcd->setSwapBytes(true);

  for (int y = y0; y <= y1; ++y) {
    if (y < 0 || y >= LCD_HEIGHT) {
      continue;
    }
    if (f.read(reinterpret_cast<uint8_t*>(row), FRAME_ROW_BYTES) !=
        (int)FRAME_ROW_BYTES) {
      lcd->setSwapBytes(prevSwap);
      f.close();
      return false;
    }
    const int dy = y - cy;
    const int dy2 = dy * dy;
    if (dy2 > rO2) {
      continue;
    }
    int spanStart = -1;
    for (int x = 0; x <= LCD_WIDTH; ++x) {
      bool inRing = false;
      if (x < LCD_WIDTH) {
        const int dx = x - cx;
        const int d2 = dx * dx + dy2;
        inRing = (d2 <= rO2 && d2 >= rI2);
        if (inRing) {
          row[x] = (alpha == 0) ? row[x] : blend565(color565, row[x], alpha);
        }
      }
      if (inRing) {
        if (spanStart < 0) {
          spanStart = x;
        }
      } else if (spanStart >= 0) {
        lcd->pushImage(spanStart, y, x - spanStart, 1, row + spanStart);
        spanStart = -1;
      }
    }
  }

  lcd->setSwapBytes(prevSwap);
  f.close();
  return true;
}

bool frameCacheSampleAlertRing(int zoom, uint16_t* pix, uint8_t* xs, uint8_t* ys,
                               int cap, int* outCount) {
  if (!pix || !xs || !ys || !outCount || cap <= 0 || zoom < ZOOM_MIN ||
      zoom > ZOOM_MAX) {
    return false;
  }
  *outCount = 0;

  File f;
  if (!openRgb565IfValid(zoom, &f)) {
    return false;
  }

  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  const int rOuter = (LCD_WIDTH / 2) - 1;
  const int rInner = rOuter - 3;
  const int rO2 = rOuter * rOuter;
  const int rI2 = rInner * rInner;
  const int y0 = cy - rOuter;
  const int y1 = cy + rOuter;
  if (y0 > 0) {
    if (!f.seek((size_t)y0 * FRAME_ROW_BYTES)) {
      f.close();
      return false;
    }
  }

  uint16_t row[LCD_WIDTH];
  int n = 0;
  for (int y = y0; y <= y1; ++y) {
    if (y < 0 || y >= LCD_HEIGHT) {
      continue;
    }
    if (f.read(reinterpret_cast<uint8_t*>(row), FRAME_ROW_BYTES) !=
        (int)FRAME_ROW_BYTES) {
      f.close();
      return false;
    }
    const int dy = y - cy;
    const int dy2 = dy * dy;
    if (dy2 > rO2) {
      continue;
    }
    for (int x = 0; x < LCD_WIDTH; ++x) {
      const int dx = x - cx;
      const int d2 = dx * dx + dy2;
      if (d2 > rO2 || d2 < rI2) {
        continue;
      }
      if (n >= cap) {
        f.close();
        return false;
      }
      pix[n] = row[x];
      xs[n] = (uint8_t)x;
      ys[n] = (uint8_t)y;
      ++n;
    }
  }
  f.close();
  *outCount = n;
  return n > 0;
}

bool frameCacheRemove(int zoom) {
  scrubTemp(zoom);
  char path[40];
  rgbPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  rgbNewPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  readyPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  alertPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  radarTimePath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  radarTimeNewPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  readyMaskSet(zoom, false);
  freshMaskSet(zoom, false);
  return true;
}

bool frameCachePrepare(int zoom) {
  // 清临时瓦片目录与 .new，保留旧 .rgb565 + .ready 供刷新期间秒切
  // （新帧写 .rgb565.new，commit 时原子替换；旧 .rgb565 始终可 blit）
  scrubTempDirKeepReady(zoom);
  char path[40];
  rgbNewPath(zoom, path, sizeof(path));
  LittleFS.remove(path);

  // 旧 .rgb565 无效时才清 ready（首次造片/损坏时）
  File chk;
  if (!openRgb565IfValid(zoom, &chk)) {
    readyMaskSet(zoom, false);
    char rp[40];
    readyPath(zoom, rp, sizeof(rp));
    LittleFS.remove(rp);
  } else {
    chk.close();
  }

  if (!LittleFS.exists("/frames")) {
    LittleFS.mkdir("/frames");
  }
  char dir[32];
  dirPath(zoom, dir, sizeof(dir));
  if (!LittleFS.mkdir(dir)) {
    // 可能已存在
    if (!LittleFS.exists(dir)) {
      Serial.printf("mkdir fail %s\n", dir);
      return false;
    }
  }
  return true;
}

bool frameCacheRestoreStale(int zoom) {
  char path[40];
  rgbNewPath(zoom, path, sizeof(path));
  LittleFS.remove(path);

  File f;
  if (!openRgb565IfValid(zoom, &f)) {
    return false;
  }
  f.close();

  readyPath(zoom, path, sizeof(path));
  File r = LittleFS.open(path, "w");
  if (!r) {
    Serial.printf("restore ready open fail z%d\n", zoom);
    return false;
  }
  r.print("1");
  r.close();
  readyMaskSet(zoom, true);
  freshMaskSet(zoom, false);
  Serial.printf("frameCache restore stale z%d\n", zoom);
  return true;
}

bool frameCacheSaveTile(int zoom, bool isRadar, int tx, int ty,
                        const uint8_t* data, size_t len) {
  if (!data || len < 8) {
    return false;
  }
  char path[48];
  frameCacheTilePath(zoom, isRadar, tx, ty, path, sizeof(path));
  File f = LittleFS.open(path, "w");
  if (!f) {
    return false;
  }
  const size_t wrote = f.write(data, len);
  f.flush();
  f.close();
  if (wrote != len) {
    LittleFS.remove(path);
    return false;
  }
  return true;
}

bool frameCacheOpenTileWrite(int zoom, bool isRadar, int tx, int ty,
                             fs::File* out) {
  if (!out) {
    return false;
  }
  char path[48];
  frameCacheTilePath(zoom, isRadar, tx, ty, path, sizeof(path));
  *out = LittleFS.open(path, "w");
  return (bool)(*out);
}

bool frameCacheWriteMeta(int zoom, const Viewport& vp, bool haveRadar,
                         int overlayZoom, int scale, int rtx0, int rty0,
                         int rtx1, int rty1) {
  char path[40];
  metaPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "w");
  if (!f) {
    return false;
  }
  f.printf("%d\n", zoom);
  f.printf("%d %d %d %d\n", vp.tx0, vp.ty0, vp.tx1, vp.ty1);
  f.printf("%.4f %.4f\n", vp.origin_px, vp.origin_py);
  f.printf("%d\n", haveRadar ? 1 : 0);
  f.printf("%d %d\n", overlayZoom, scale);
  f.printf("%d %d %d %d\n", rtx0, rty0, rtx1, rty1);
  f.flush();
  f.close();
  return true;
}

bool frameCacheReadMeta(int zoom, Viewport* vp, bool* haveRadar,
                        int* overlayZoom, int* scale, int* rtx0, int* rty0,
                        int* rtx1, int* rty1) {
  char path[40];
  metaPath(zoom, path, sizeof(path));
  File meta = LittleFS.open(path, "r");
  if (!meta) {
    return false;
  }
  int z = 0, tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0, hr = 0;
  int oz = 0, sc = 1;
  int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
  double opx = 0, opy = 0;
  String line = meta.readStringUntil('\n');
  sscanf(line.c_str(), "%d", &z);
  line = meta.readStringUntil('\n');
  sscanf(line.c_str(), "%d %d %d %d", &tx0, &ty0, &tx1, &ty1);
  line = meta.readStringUntil('\n');
  sscanf(line.c_str(), "%lf %lf", &opx, &opy);
  line = meta.readStringUntil('\n');
  sscanf(line.c_str(), "%d", &hr);
  if (meta.available()) {
    line = meta.readStringUntil('\n');
    sscanf(line.c_str(), "%d %d", &oz, &sc);
    line = meta.readStringUntil('\n');
    sscanf(line.c_str(), "%d %d %d %d", &rx0, &ry0, &rx1, &ry1);
  } else {
    oz = z;
    sc = 1;
    rx0 = tx0;
    ry0 = ty0;
    rx1 = tx1;
    ry1 = ty1;
  }
  meta.close();
  if (vp) {
    vp->tx0 = tx0;
    vp->ty0 = ty0;
    vp->tx1 = tx1;
    vp->ty1 = ty1;
    vp->origin_px = opx;
    vp->origin_py = opy;
  }
  if (haveRadar) {
    *haveRadar = hr != 0;
  }
  if (overlayZoom) {
    *overlayZoom = oz;
  }
  if (scale) {
    *scale = sc < 1 ? 1 : sc;
  }
  if (rtx0) {
    *rtx0 = rx0;
  }
  if (rty0) {
    *rty0 = ry0;
  }
  if (rtx1) {
    *rtx1 = rx1;
  }
  if (rty1) {
    *rty1 = ry1;
  }
  return true;
}

bool frameCacheCreateRgb565(int zoom, uint16_t backdropColor) {
  char path[40];
  rgbPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.println("createRgb565 open fail");
    return false;
  }
  uint16_t row[LCD_WIDTH];
  for (int x = 0; x < LCD_WIDTH; ++x) {
    row[x] = backdropColor;
  }
  for (int y = 0; y < LCD_HEIGHT; ++y) {
    if (f.write(reinterpret_cast<uint8_t*>(row), FRAME_ROW_BYTES) !=
        FRAME_ROW_BYTES) {
      f.close();
      LittleFS.remove(path);
      return false;
    }
  }
  f.flush();
  f.close();
  File check = LittleFS.open(path, "r");
  const bool ok = check && check.size() == FRAME_RGB565_BYTES;
  if (check) {
    check.close();
  }
  if (!ok) {
    LittleFS.remove(path);
  }
  return ok;
}

bool frameCacheBeginRgb565New(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }
  char path[40];
  rgbNewPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  File f = LittleFS.open(path, "w");
  if (!f) {
    return false;
  }
  f.close();
  return true;
}

bool frameCacheWriteRgb565Band(int zoom, int startRow, int rowCount,
                               const uint16_t* frame) {
  if (!frame || zoom < ZOOM_MIN || zoom > ZOOM_MAX || startRow < 0 ||
      rowCount < 1 || startRow + rowCount > LCD_HEIGHT) {
    return false;
  }
  char path[40];
  rgbNewPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "r+");
  if (!f || !f.seek((size_t)startRow * FRAME_ROW_BYTES)) {
    if (f) {
      f.close();
    }
    return false;
  }
  const size_t bytes = (size_t)rowCount * FRAME_ROW_BYTES;
  const size_t wrote = f.write(reinterpret_cast<const uint8_t*>(frame), bytes);
  f.flush();
  f.close();
  return wrote == bytes;
}

bool frameCacheWriteRgb565(int zoom, const uint16_t* frame) {
  if (!frame || !frameCacheBeginRgb565New(zoom)) {
    return false;
  }
  if (!frameCacheWriteRgb565Band(zoom, 0, LCD_HEIGHT, frame)) {
    char path[40];
    rgbNewPath(zoom, path, sizeof(path));
    LittleFS.remove(path);
    return false;
  }
  return true;
}

bool frameCacheLoadRgb565(int zoom, uint16_t* frame) {
  if (!frame || zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }
  File f;
  if (!openRgb565IfValid(zoom, &f)) {
    return false;
  }
  const size_t n =
      f.read(reinterpret_cast<uint8_t*>(frame), FRAME_RGB565_BYTES);
  f.close();
  return n == FRAME_RGB565_BYTES;
}

bool frameCacheReadPixelsSorted(int zoom, const uint16_t* keys,
                                uint16_t* colors, size_t count) {
  if ((!keys && count != 0) || (!colors && count != 0)) {
    return false;
  }
  if (count == 0) {
    return true;
  }

  File f;
  if (!openRgb565IfValid(zoom, &f)) {
    return false;
  }

  uint16_t row[LCD_WIDTH];
  size_t i = 0;
  uint16_t previous = 0;
  bool havePrevious = false;
  while (i < count) {
    const uint16_t key = keys[i];
    if (key >= LCD_WIDTH * LCD_HEIGHT ||
        (havePrevious && key < previous)) {
      f.close();
      return false;
    }
    const int y = key / LCD_WIDTH;
    const size_t rowBegin = i;
    size_t rowEnd = i + 1;
    while (rowEnd < count && keys[rowEnd] / LCD_WIDTH == y) {
      if (keys[rowEnd] < keys[rowEnd - 1]) {
        f.close();
        return false;
      }
      ++rowEnd;
    }
    const int x0 = keys[rowBegin] % LCD_WIDTH;
    const int x1 = keys[rowEnd - 1] % LCD_WIDTH;
    const size_t spanBytes = (size_t)(x1 - x0 + 1) * sizeof(uint16_t);
    const size_t offset = (size_t)y * FRAME_ROW_BYTES +
                          (size_t)x0 * sizeof(uint16_t);
    if (!f.seek(offset) ||
        f.read(reinterpret_cast<uint8_t*>(row), spanBytes) != (int)spanBytes) {
      f.close();
      return false;
    }

    while (i < rowEnd) {
      const uint16_t rowKey = keys[i];
      if (rowKey >= LCD_WIDTH * LCD_HEIGHT ||
          (havePrevious && rowKey < previous)) {
        f.close();
        return false;
      }
      colors[i] = row[rowKey % LCD_WIDTH - x0];
      previous = rowKey;
      havePrevious = true;
      ++i;
    }
  }
  f.close();
  return true;
}

bool frameCachePromoteNewNoReady(int zoom) {
  char newPath[40];
  char path[40];
  rgbNewPath(zoom, newPath, sizeof(newPath));
  File f = LittleFS.open(newPath, "r");
  if (!f || f.size() != FRAME_RGB565_BYTES) {
    if (f) {
      f.close();
    }
    return false;
  }
  f.close();
  rgbPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  if (!LittleFS.rename(newPath, path)) {
    Serial.printf("promote rename fail z%d\n", zoom);
    return false;
  }
  return true;
}

static inline uint16_t darkenBasemap565(uint16_t pix) {
  const float t = BASEMAP_BLEND;
  const float u = 1.0f - t;
  const int r = (int)lroundf(expand5((pix >> 11) & 0x1F) * t +
                             (float)BASEMAP_BACKDROP_R * u);
  const int g = (int)lroundf(expand6((pix >> 5) & 0x3F) * t +
                             (float)BASEMAP_BACKDROP_G * u);
  const int b =
      (int)lroundf(expand5(pix & 0x1F) * t + (float)BASEMAP_BACKDROP_B * u);
  return pack565(r, g, b);
}

void radarCenterSampleReset(RadarCenterSample* s) {
  if (!s) {
    return;
  }
  s->sumR = 0;
  s->sumG = 0;
  s->sumB = 0;
  s->sumA = 0;
  s->maxAlpha = 0;
}

bool radarCenterSampleFinalize(const RadarCenterSample* s, bool* hasCloud,
                               uint16_t* color565) {
  if (!s || !hasCloud || !color565) {
    return false;
  }
  *hasCloud = s->maxAlpha >= 40;
  if (!*hasCloud || s->sumA == 0) {
    *color565 = 0;
    return true;
  }
  const int r = (int)(s->sumR / s->sumA);
  const int g = (int)(s->sumG / s->sumA);
  const int b = (int)(s->sumB / s->sumA);
  *color565 = pack565(r, g, b);
  return true;
}

bool frameCacheWriteAlert(int zoom, bool hasCloud, uint16_t color565) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }
  if (!LittleFS.exists("/frames")) {
    LittleFS.mkdir("/frames");
  }
  char path[40];
  alertPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "w");
  if (!f) {
    return false;
  }
  const uint8_t has = hasCloud ? 1 : 0;
  const bool ok = f.write(&has, 1) == 1 &&
                  f.write(reinterpret_cast<const uint8_t*>(&color565), 2) == 2;
  f.close();
  return ok;
}

bool frameCacheWriteRadarTime(int zoom, uint32_t timeSec) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX || timeSec == 0) {
    return false;
  }
  if (!LittleFS.exists("/frames")) {
    LittleFS.mkdir("/frames");
  }
  // 旧版只有裸 uint32_t。新记录用 magic 作为可信 commit 标记；时间按
  // little-endian 显式编码，避免结构体填充和半写文件被误判为有效。
  static constexpr uint8_t kMagic[4] = {'R', 'T', 'M', '2'};
  uint8_t record[8] = {kMagic[0], kMagic[1], kMagic[2], kMagic[3],
                       (uint8_t)(timeSec), (uint8_t)(timeSec >> 8),
                       (uint8_t)(timeSec >> 16), (uint8_t)(timeSec >> 24)};
  char path[40];
  char newPath[40];
  radarTimePath(zoom, path, sizeof(path));
  radarTimeNewPath(zoom, newPath, sizeof(newPath));
  LittleFS.remove(newPath);
  File f = LittleFS.open(newPath, "w");
  if (!f) {
    return false;
  }
  const bool ok = f.write(record, sizeof(record)) == sizeof(record);
  f.flush();
  f.close();
  if (!ok) {
    LittleFS.remove(newPath);
    return false;
  }
  LittleFS.remove(path);
  if (!LittleFS.rename(newPath, path)) {
    LittleFS.remove(newPath);
    return false;
  }
  return true;
}

bool frameCacheReadRadarTime(int zoom, uint32_t* timeSec) {
  if (!timeSec || zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }
  *timeSec = 0;
  char path[40];
  radarTimePath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f || f.size() != 8) {
    if (f) {
      f.close();
    }
    return false;
  }
  uint8_t record[8];
  const size_t n = f.read(record, sizeof(record));
  f.close();
  if (n != sizeof(record) || record[0] != 'R' || record[1] != 'T' ||
      record[2] != 'M' || record[3] != '2') {
    return false;
  }
  *timeSec = (uint32_t)record[4] | ((uint32_t)record[5] << 8) |
             ((uint32_t)record[6] << 16) | ((uint32_t)record[7] << 24);
  return *timeSec != 0;
}

void frameCacheRemoveRadarTime(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return;
  }
  char path[40];
  radarTimePath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  radarTimeNewPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
}

bool frameCacheReadAlert(int zoom, bool* hasCloud, uint16_t* color565) {
  if (!hasCloud || !color565 || zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }
  char path[40];
  alertPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f || f.size() < 3) {
    if (f) {
      f.close();
    }
    return false;
  }
  uint8_t has = 0;
  uint16_t color = 0;
  if (f.read(&has, 1) != 1 ||
      f.read(reinterpret_cast<uint8_t*>(&color), 2) != 2) {
    f.close();
    return false;
  }
  f.close();
  *hasCloud = has != 0;
  *color565 = color;
  return true;
}

void frameCacheRemoveAlert(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return;
  }
  char path[40];
  alertPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
}

static inline void accumulateCenterSample(RadarCenterSample* centerOut, int x,
                                          int y, uint16_t pix, uint8_t a) {
  if (!centerOut || a == 0) {
    return;
  }
  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  if (x < cx - 1 || x > cx + 1 || y < cy - 1 || y > cy + 1) {
    return;
  }
  if (a > centerOut->maxAlpha) {
    centerOut->maxAlpha = a;
  }
  centerOut->sumR += (uint32_t)expand5((pix >> 11) & 0x1F) * a;
  centerOut->sumG += (uint32_t)expand6((pix >> 5) & 0x3F) * a;
  centerOut->sumB += (uint32_t)expand5(pix & 0x1F) * a;
  centerOut->sumA += a;
}

bool frameCacheStampRawToBand(uint16_t* frame, int bandY, int bandHeight,
                              const char* rawPath, const char* alphaPath,
                              int pasteX, int pasteY, int scale, bool alphaKey,
                              RadarCenterSample* centerOut) {
  if (!frame || !rawPath || scale < 1 || bandY < 0 || bandHeight < 1 ||
      bandY + bandHeight > LCD_HEIGHT) {
    return false;
  }
  File raw = LittleFS.open(rawPath, "r");
  if (!raw || raw.size() != (size_t)TILE_SIZE * TILE_SIZE * 2) {
    if (raw) {
      raw.close();
    }
    return false;
  }
  File alpha;
  const bool useAlpha = alphaPath && alphaPath[0];
  if (useAlpha) {
    alpha = LittleFS.open(alphaPath, "r");
    if (!alpha || alpha.size() != (size_t)TILE_SIZE * TILE_SIZE) {
      if (alpha) {
        alpha.close();
      }
      raw.close();
      return false;
    }
  }

  const int outW = TILE_SIZE * scale;
  const int outH = TILE_SIZE * scale;
  uint16_t row0[TILE_SIZE];
  uint16_t row1[TILE_SIZE];
  uint8_t a0[TILE_SIZE];
  uint8_t a1[TILE_SIZE];

  if (scale == 1) {
    for (int dy = 0; dy < TILE_SIZE; ++dy) {
      const int y = pasteY + dy;
      if (y < bandY || y >= bandY + bandHeight) {
        if (!raw.seek((size_t)(dy + 1) * TILE_SIZE * 2)) {
          raw.close();
          if (useAlpha) {
            alpha.close();
          }
          return false;
        }
        if (useAlpha && !alpha.seek((size_t)(dy + 1) * TILE_SIZE)) {
          raw.close();
          alpha.close();
          return false;
        }
        continue;
      }
      if (raw.read(reinterpret_cast<uint8_t*>(row0), TILE_SIZE * 2) !=
          (int)(TILE_SIZE * 2)) {
        raw.close();
        if (useAlpha) {
          alpha.close();
        }
        return false;
      }
      if (useAlpha &&
          alpha.read(a0, TILE_SIZE) != (int)TILE_SIZE) {
        raw.close();
        alpha.close();
        return false;
      }
      uint16_t* dst = frame + (y - bandY) * LCD_WIDTH;
      for (int dx = 0; dx < TILE_SIZE; ++dx) {
        const int x = pasteX + dx;
        if (x < 0 || x >= LCD_WIDTH) {
          continue;
        }
        const uint16_t pix = row0[dx];
        if (useAlpha) {
          accumulateCenterSample(centerOut, x, y, pix, a0[dx]);
          dst[x] = blend565(pix, dst[x], a0[dx]);
        } else if (alphaKey) {
          if (pix == 0) {
            continue;
          }
          dst[x] = pix;
        } else {
          dst[x] = darkenBasemap565(pix);
        }
      }
    }
    raw.close();
    if (useAlpha) {
      alpha.close();
    }
    return true;
  }

  // scale>=2：预乘 alpha 双线性，避免透明邻域拉出黑边
  int cachedY0 = -2;
  int cachedY1 = -2;
  for (int dy = 0; dy < outH; ++dy) {
    const int y = pasteY + dy;
    if (y < bandY || y >= bandY + bandHeight) {
      continue;
    }
    const float sy = ((float)dy + 0.5f) / (float)scale - 0.5f;
    int y0 = (int)floorf(sy);
    if (y0 < 0) {
      y0 = 0;
    }
    if (y0 > TILE_SIZE - 1) {
      y0 = TILE_SIZE - 1;
    }
    const int y1 = y0 < TILE_SIZE - 1 ? y0 + 1 : y0;
    if (y0 != cachedY0 || y1 != cachedY1) {
      raw.seek((size_t)y0 * TILE_SIZE * 2);
      if (raw.read(reinterpret_cast<uint8_t*>(row0), TILE_SIZE * 2) !=
          (int)(TILE_SIZE * 2)) {
        raw.close();
        if (useAlpha) {
          alpha.close();
        }
        return false;
      }
      if (y1 != y0) {
        if (raw.read(reinterpret_cast<uint8_t*>(row1), TILE_SIZE * 2) !=
            (int)(TILE_SIZE * 2)) {
          raw.close();
          if (useAlpha) {
            alpha.close();
          }
          return false;
        }
      } else {
        memcpy(row1, row0, sizeof(row0));
      }
      if (useAlpha) {
        alpha.seek((size_t)y0 * TILE_SIZE);
        if (alpha.read(a0, TILE_SIZE) != (int)TILE_SIZE) {
          raw.close();
          alpha.close();
          return false;
        }
        if (y1 != y0) {
          if (alpha.read(a1, TILE_SIZE) != (int)TILE_SIZE) {
            raw.close();
            alpha.close();
            return false;
          }
        } else {
          memcpy(a1, a0, sizeof(a0));
        }
      }
      cachedY0 = y0;
      cachedY1 = y1;
    }
    uint16_t* dst = frame + (y - bandY) * LCD_WIDTH;
    const float fy = sy - (float)y0;
    for (int dx = 0; dx < outW; ++dx) {
      const int x = pasteX + dx;
      if (x < 0 || x >= LCD_WIDTH) {
        continue;
      }
      float sxc = ((float)dx + 0.5f) / (float)scale - 0.5f;
      if (sxc < 0) {
        sxc = 0;
      }
      if (sxc > TILE_SIZE - 1) {
        sxc = TILE_SIZE - 1;
      }
      const int x0 = (int)floorf(sxc);
      const int x1 = x0 < TILE_SIZE - 1 ? x0 + 1 : x0;
      const float fx = sxc - x0;

      auto chan = [](uint16_t c, int shift, int mask) {
        return (c >> shift) & mask;
      };
      auto lerpI = [](int a, int b, float t) {
        return a + (int)lroundf((b - a) * t);
      };

      if (useAlpha) {
        const int aa00 = a0[x0], aa10 = a0[x1], aa01 = a1[x0], aa11 = a1[x1];
        // 预乘后再插值
        const int r00 = chan(row0[x0], 11, 0x1F) * aa00;
        const int r10 = chan(row0[x1], 11, 0x1F) * aa10;
        const int r01 = chan(row1[x0], 11, 0x1F) * aa01;
        const int r11 = chan(row1[x1], 11, 0x1F) * aa11;
        const int g00 = chan(row0[x0], 5, 0x3F) * aa00;
        const int g10 = chan(row0[x1], 5, 0x3F) * aa10;
        const int g01 = chan(row1[x0], 5, 0x3F) * aa01;
        const int g11 = chan(row1[x1], 5, 0x3F) * aa11;
        const int b00 = chan(row0[x0], 0, 0x1F) * aa00;
        const int b10 = chan(row0[x1], 0, 0x1F) * aa10;
        const int b01 = chan(row1[x0], 0, 0x1F) * aa01;
        const int b11 = chan(row1[x1], 0, 0x1F) * aa11;

        const int ra = lerpI(lerpI(r00, r10, fx), lerpI(r01, r11, fx), fy);
        const int ga = lerpI(lerpI(g00, g10, fx), lerpI(g01, g11, fx), fy);
        const int ba = lerpI(lerpI(b00, b10, fx), lerpI(b01, b11, fx), fy);
        const int aa = lerpI(lerpI(aa00, aa10, fx), lerpI(aa01, aa11, fx), fy);
        if (aa <= 0) {
          continue;
        }
        const uint16_t pix = (uint16_t)(((ra / aa) << 11) | ((ga / aa) << 5) | (ba / aa));
        const uint8_t a8 = (uint8_t)(aa > 255 ? 255 : aa);
        accumulateCenterSample(centerOut, x, y, pix, a8);
        dst[x] = blend565(pix, dst[x], a8);
      } else {
        const uint16_t c00 = row0[x0];
        const uint16_t c10 = row0[x1];
        const uint16_t c01 = row1[x0];
        const uint16_t c11 = row1[x1];
        const int r = lerpI(lerpI(chan(c00, 11, 0x1F), chan(c10, 11, 0x1F), fx),
                            lerpI(chan(c01, 11, 0x1F), chan(c11, 11, 0x1F), fx),
                            fy);
        const int g = lerpI(lerpI(chan(c00, 5, 0x3F), chan(c10, 5, 0x3F), fx),
                            lerpI(chan(c01, 5, 0x3F), chan(c11, 5, 0x3F), fx),
                            fy);
        const int b = lerpI(lerpI(chan(c00, 0, 0x1F), chan(c10, 0, 0x1F), fx),
                            lerpI(chan(c01, 0, 0x1F), chan(c11, 0, 0x1F), fx),
                            fy);
        const uint16_t pix = (uint16_t)((r << 11) | (g << 5) | b);
        if (alphaKey && pix == 0) {
          continue;
        }
        dst[x] = pix;
      }
    }
  }
  raw.close();
  if (useAlpha) {
    alpha.close();
  }
  return true;
}

bool frameCacheStampRawToBuffer(uint16_t* frame, const char* rawPath,
                                const char* alphaPath, int pasteX, int pasteY,
                                int scale, bool alphaKey,
                                RadarCenterSample* centerOut) {
  return frameCacheStampRawToBand(frame, 0, LCD_HEIGHT, rawPath, alphaPath,
                                  pasteX, pasteY, scale, alphaKey, centerOut);
}

static inline uint16_t bilSample(const uint16_t* tile, float sx, float sy) {
  if (sx < 0) {
    sx = 0;
  }
  if (sy < 0) {
    sy = 0;
  }
  if (sx > TILE_SIZE - 1) {
    sx = TILE_SIZE - 1;
  }
  if (sy > TILE_SIZE - 1) {
    sy = TILE_SIZE - 1;
  }
  const int x0 = (int)floorf(sx);
  const int y0 = (int)floorf(sy);
  const int x1 = x0 < TILE_SIZE - 1 ? x0 + 1 : x0;
  const int y1 = y0 < TILE_SIZE - 1 ? y0 + 1 : y0;
  const float fx = sx - x0;
  const float fy = sy - y0;
  const uint16_t c00 = tile[y0 * TILE_SIZE + x0];
  const uint16_t c10 = tile[y0 * TILE_SIZE + x1];
  const uint16_t c01 = tile[y1 * TILE_SIZE + x0];
  const uint16_t c11 = tile[y1 * TILE_SIZE + x1];
  auto lerpChan = [](int a, int b, float t) {
    return (int)lroundf(a + (b - a) * t);
  };
  const int r = lerpChan((c00 >> 11) & 0x1F, (c10 >> 11) & 0x1F, fx);
  const int g = lerpChan((c00 >> 5) & 0x3F, (c10 >> 5) & 0x3F, fx);
  const int b = lerpChan(c00 & 0x1F, c10 & 0x1F, fx);
  const int r2 = lerpChan((c01 >> 11) & 0x1F, (c11 >> 11) & 0x1F, fx);
  const int g2 = lerpChan((c01 >> 5) & 0x3F, (c11 >> 5) & 0x3F, fx);
  const int b2 = lerpChan(c01 & 0x1F, c11 & 0x1F, fx);
  const int rr = lerpChan(r, r2, fy);
  const int gg = lerpChan(g, g2, fy);
  const int bb = lerpChan(b, b2, fy);
  return (uint16_t)((rr << 11) | (gg << 5) | bb);
}

bool frameCacheStampTile(int zoom, const uint16_t* tile256, int pasteX,
                         int pasteY, int scale, bool alphaKey) {
  if (!tile256 || scale < 1) {
    return false;
  }
  char path[40];
  rgbPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "r+");
  if (!f) {
    return false;
  }

  const int outW = TILE_SIZE * scale;
  const int outH = TILE_SIZE * scale;
  uint16_t rowBuf[LCD_WIDTH];

  for (int dy = 0; dy < outH; ++dy) {
    const int y = pasteY + dy;
    if (y < 0 || y >= LCD_HEIGHT) {
      continue;
    }
    const size_t off = (size_t)y * FRAME_ROW_BYTES;
    if (!f.seek(off)) {
      f.close();
      return false;
    }
    if (f.read(reinterpret_cast<uint8_t*>(rowBuf), FRAME_ROW_BYTES) !=
        (int)FRAME_ROW_BYTES) {
      f.close();
      return false;
    }

    for (int dx = 0; dx < outW; ++dx) {
      const int x = pasteX + dx;
      if (x < 0 || x >= LCD_WIDTH) {
        continue;
      }
      uint16_t pix;
      if (scale == 1) {
        pix = tile256[dy * TILE_SIZE + dx];
      } else {
        const float sx = ((float)dx + 0.5f) / (float)scale - 0.5f;
        const float sy = ((float)dy + 0.5f) / (float)scale - 0.5f;
        pix = bilSample(tile256, sx, sy);
      }
      if (alphaKey && pix == 0) {
        continue;  // RainViewer 透明近似为黑
      }
      // 半透明：非零像素覆盖（雷达色带通常不透明）
      rowBuf[x] = pix;
    }

    if (!f.seek(off)) {
      f.close();
      return false;
    }
    if (f.write(reinterpret_cast<uint8_t*>(rowBuf), FRAME_ROW_BYTES) !=
        FRAME_ROW_BYTES) {
      f.close();
      return false;
    }
  }
  f.flush();
  f.close();
  return true;
}

// DesktopRadar draw_overlay 红点 (255, 60, 60)
static inline uint16_t crosshairRed565() {
  return (uint16_t)(((255 & 0xF8) << 8) | ((60 & 0xFC) << 3) | (60 >> 3));
}

static inline void setBandPixel(uint16_t* frame, int bandY, int bandHeight,
                                int x, int y, uint16_t color) {
  if (x >= 0 && x < LCD_WIDTH && y >= bandY && y < bandY + bandHeight) {
    frame[(y - bandY) * LCD_WIDTH + x] = color;
  }
}

static void fillCrosshairDotBand(uint16_t* frame, int bandY, int bandHeight,
                                 int cx, int cy, uint16_t red) {
  for (int dy = -3; dy <= 3; ++dy) {
    for (int dx = -3; dx <= 3; ++dx) {
      if (dx * dx + dy * dy > 9) {
        continue;
      }
      setBandPixel(frame, bandY, bandHeight, cx + dx, cy + dy, red);
    }
  }
}

void frameCacheDrawCrosshairBand(uint16_t* frame, int bandY, int bandHeight,
                                 uint16_t color) {
  if (!frame || bandY < 0 || bandHeight < 1 ||
      bandY + bandHeight > LCD_HEIGHT) {
    return;
  }
  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  for (int x = cx - 8; x <= cx + 8; ++x) {
    setBandPixel(frame, bandY, bandHeight, x, cy, color);
  }
  for (int y = cy - 8; y <= cy + 8; ++y) {
    setBandPixel(frame, bandY, bandHeight, cx, y, color);
  }
  fillCrosshairDotBand(frame, bandY, bandHeight, cx, cy, crosshairRed565());
}

void frameCacheDrawCrosshairBuf(uint16_t* frame, uint16_t color) {
  frameCacheDrawCrosshairBand(frame, 0, LCD_HEIGHT, color);
}

bool frameCacheDrawCrosshair(int zoom, uint16_t color) {
  char path[40];
  rgbPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "r+");
  if (!f) {
    return false;
  }
  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  uint16_t rowBuf[LCD_WIDTH];

  // 横线
  if (!f.seek((size_t)cy * FRAME_ROW_BYTES)) {
    f.close();
    return false;
  }
  if (f.read(reinterpret_cast<uint8_t*>(rowBuf), FRAME_ROW_BYTES) !=
      (int)FRAME_ROW_BYTES) {
    f.close();
    return false;
  }
  for (int x = cx - 8; x <= cx + 8; ++x) {
    if (x >= 0 && x < LCD_WIDTH) {
      rowBuf[x] = color;
    }
  }
  f.seek((size_t)cy * FRAME_ROW_BYTES);
  f.write(reinterpret_cast<uint8_t*>(rowBuf), FRAME_ROW_BYTES);

  // 竖线
  for (int y = cy - 8; y <= cy + 8; ++y) {
    if (y < 0 || y >= LCD_HEIGHT) {
      continue;
    }
    f.seek((size_t)y * FRAME_ROW_BYTES + (size_t)cx * 2);
    f.write(reinterpret_cast<uint8_t*>(&color), 2);
  }

  // 中心红点（叠在十字之上）
  const uint16_t red = crosshairRed565();
  for (int dy = -3; dy <= 3; ++dy) {
    for (int dx = -3; dx <= 3; ++dx) {
      if (dx * dx + dy * dy > 9) {
        continue;
      }
      const int x = cx + dx;
      const int y = cy + dy;
      if (x < 0 || x >= LCD_WIDTH || y < 0 || y >= LCD_HEIGHT) {
        continue;
      }
      if (!f.seek((size_t)y * FRAME_ROW_BYTES + (size_t)x * 2)) {
        f.close();
        return false;
      }
      if (f.write(reinterpret_cast<const uint8_t*>(&red), 2) != 2) {
        f.close();
        return false;
      }
    }
  }

  f.flush();
  f.close();
  return true;
}

void frameCacheDrawOverlayBand(uint16_t* frame, int bandY, int bandHeight,
                               uint32_t frameTs) {
  const int overlayTop = LCD_HEIGHT - OVERLAY_BAR_H;
  if (!frame || bandY < 0 || bandHeight < 1 ||
      bandY + bandHeight > LCD_HEIGHT || bandY + bandHeight <= overlayTop ||
      bandY >= LCD_HEIGHT) {
    return;
  }

  static const char* kWday[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  char label[16];
  if (frameTs == 0) {
    snprintf(label, sizeof(label), "--- --:--");
  } else {
    const time_t local = (time_t)frameTs + (time_t)TIMEZONE_OFFSET_SEC;
    struct tm tm{};
    gmtime_r(&local, &tm);
    const char* wday = (tm.tm_wday >= 0 && tm.tm_wday <= 6) ? kWday[tm.tm_wday]
                                                            : "---";
    snprintf(label, sizeof(label), "%s %02d:%02d", wday, tm.tm_hour, tm.tm_min);
  }

  // 复用分段帧缓冲，不额外分配 sprite 像素内存。
  lgfx::LGFX_Sprite spr;
  spr.setColorDepth(16);
  spr.setBuffer(frame, LCD_WIDTH, bandHeight, 16);

  spr.fillRect(0, overlayTop - bandY, LCD_WIDTH, OVERLAY_BAR_H, TFT_BLACK);
  spr.setFont(&fonts::Font2);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.drawString(label, LCD_WIDTH / 2,
                 LCD_HEIGHT - OVERLAY_BAR_H / 2 - bandY);
}

void frameCacheDrawOverlayBuf(uint16_t* frame, uint32_t frameTs) {
  frameCacheDrawOverlayBand(frame, 0, LCD_HEIGHT, frameTs);
}

bool frameCacheCommit(int zoom) {
  if (!frameCachePromoteNewNoReady(zoom)) {
    Serial.printf("commit reject z%d (promote fail)\n", zoom);
    return false;
  }

  char path[40];
  // 删除临时瓦片（保留 rgb565）
  Viewport vp{};
  bool haveRadar = false;
  int oz = 0, sc = 1, rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
  if (frameCacheReadMeta(zoom, &vp, &haveRadar, &oz, &sc, &rx0, &ry0, &rx1,
                         &ry1)) {
    removeTilesInRange(zoom, false, vp.tx0, vp.ty0, vp.tx1, vp.ty1);
    if (haveRadar) {
      removeTilesInRange(zoom, true, rx0, ry0, rx1, ry1);
    }
  }
  metaPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  char rawTmp[40];
  snprintf(rawTmp, sizeof(rawTmp), "/frames/z%02d/tile.raw", zoom);
  LittleFS.remove(rawTmp);
  char dir[32];
  dirPath(zoom, dir, sizeof(dir));
  // 清空目录内残留后再 rmdir
  File d = LittleFS.open(dir);
  if (d && d.isDirectory()) {
    File entry = d.openNextFile();
    while (entry) {
      char child[64];
      snprintf(child, sizeof(child), "%s/%s", dir, entry.name());
      entry.close();
      LittleFS.remove(child);
      entry = d.openNextFile();
    }
  }
  if (d) {
    d.close();
  }
  LittleFS.rmdir(dir);

  readyPath(zoom, path, sizeof(path));
  File r = LittleFS.open(path, "w");
  if (!r) {
    Serial.printf("commit ready open fail z%d\n", zoom);
    return false;
  }
  r.print("1");
  r.close();
  readyMaskSet(zoom, true);
  freshMaskSet(zoom, true);
  Serial.printf("frameCache commit z%d rgb565 ok\n", zoom);
  return true;
}

// ---- 新鲜度 ----

void frameCacheMarkFresh(int zoom, bool fresh) { freshMaskSet(zoom, fresh); }

bool frameCacheIsFresh(int zoom) { return freshMaskGet(zoom); }

void frameCacheMarkAllFresh(bool fresh) {
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; ++z) {
    if (z == ZOOM_SKIP) {
      continue;
    }
    freshMaskSet(z, fresh);
  }
}

void frameCacheMarkAllStaleExcept(int keepZoom) {
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; ++z) {
    if (z == ZOOM_SKIP || z == keepZoom) {
      continue;
    }
    freshMaskSet(z, false);
  }
}

int frameCacheCountFresh() {
  int n = 0;
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; ++z) {
    if (z == ZOOM_SKIP) {
      continue;
    }
    if (freshMaskGet(z)) {
      ++n;
    }
  }
  return n;
}

void frameCacheSetProtectedZoom(int zoom) { s_protectedZoom = zoom; }

bool frameCacheEnsureBakeSpace(size_t needBytes, int keepZoom) {
  auto freeBytes = []() -> size_t {
    const size_t total = LittleFS.totalBytes();
    const size_t used = LittleFS.usedBytes();
    return total > used ? (total - used) : 0;
  };

  const bool ok = freeBytes() >= needBytes;
  if (!ok) {
    Serial.printf(
        "bake workspace short z%d protected=%d need=%u free=%u; keep all ready frames\n",
        keepZoom, s_protectedZoom, (unsigned)needBytes, (unsigned)freeBytes());
  }
  return ok;
}
