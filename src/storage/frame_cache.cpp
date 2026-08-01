#include "frame_cache.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// bit0 = ZOOM_MIN … 避免每秒 open 缺失 ready 刷屏
static uint16_t s_readyMask = 0;

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
    }
  }
  return true;
}

bool frameCacheHas(int zoom) {
  return readyMaskGet(zoom);
}

int frameCacheZoomSlots() { return ZOOM_MAX - ZOOM_MIN + 1; }

int frameCacheCountReady() {
  int n = 0;
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; ++z) {
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
  char path[40];
  rgbPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) {
    return false;
  }
  // 缓存内存放 native RGB565（与 color565 一致）；
  // LovyanGFX 默认将 uint16_t* 当作 swap565，必须 setSwapBytes(true)
  const bool prevSwap = lcd->getSwapBytes();
  lcd->setSwapBytes(true);
  uint16_t row[LCD_WIDTH];
  for (int y = 0; y < LCD_HEIGHT; ++y) {
    if (f.read(reinterpret_cast<uint8_t*>(row), FRAME_ROW_BYTES) !=
        (int)FRAME_ROW_BYTES) {
      lcd->setSwapBytes(prevSwap);
      f.close();
      return false;
    }
    lcd->pushImage(0, y, LCD_WIDTH, 1, row);
  }
  lcd->setSwapBytes(prevSwap);
  f.close();
  return true;
}

bool frameCacheRemove(int zoom) {
  scrubTemp(zoom);
  char path[40];
  rgbPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  readyPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  readyMaskSet(zoom, false);
  return true;
}

bool frameCachePrepare(int zoom) {
  frameCacheRemove(zoom);
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

bool frameCacheWriteRgb565(int zoom, const uint16_t* frame) {
  if (!frame) {
    return false;
  }
  char path[40];
  rgbPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "w");
  if (!f) {
    return false;
  }
  const size_t wrote =
      f.write(reinterpret_cast<const uint8_t*>(frame), FRAME_RGB565_BYTES);
  f.flush();
  f.close();
  if (wrote != FRAME_RGB565_BYTES) {
    LittleFS.remove(path);
    return false;
  }
  return true;
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

/** 对齐 DesktopRadar prepare_basemap_tile：tile×t + backdrop×(1-t)，在 8-bit 计算。 */
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

bool frameCacheStampRawToBuffer(uint16_t* frame, const char* rawPath,
                                const char* alphaPath, int pasteX, int pasteY,
                                int scale, bool alphaKey) {
  if (!frame || !rawPath || scale < 1) {
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
      if (y < 0 || y >= LCD_HEIGHT) {
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
      uint16_t* dst = frame + y * LCD_WIDTH;
      for (int dx = 0; dx < TILE_SIZE; ++dx) {
        const int x = pasteX + dx;
        if (x < 0 || x >= LCD_WIDTH) {
          continue;
        }
        const uint16_t pix = row0[dx];
        if (useAlpha) {
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
    if (y < 0 || y >= LCD_HEIGHT) {
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
    uint16_t* dst = frame + y * LCD_WIDTH;
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
        dst[x] = blend565(pix, dst[x], (uint8_t)(aa > 255 ? 255 : aa));
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

static void fillCrosshairDotBuf(uint16_t* frame, int cx, int cy,
                                uint16_t red) {
  for (int dy = -3; dy <= 3; ++dy) {
    for (int dx = -3; dx <= 3; ++dx) {
      if (dx * dx + dy * dy > 9) {
        continue;
      }
      const int x = cx + dx;
      const int y = cy + dy;
      if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT) {
        frame[y * LCD_WIDTH + x] = red;
      }
    }
  }
}

void frameCacheDrawCrosshairBuf(uint16_t* frame, uint16_t color) {
  if (!frame) {
    return;
  }
  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  for (int x = cx - 8; x <= cx + 8; ++x) {
    if (x >= 0 && x < LCD_WIDTH) {
      frame[cy * LCD_WIDTH + x] = color;
    }
  }
  for (int y = cy - 8; y <= cy + 8; ++y) {
    if (y >= 0 && y < LCD_HEIGHT) {
      frame[y * LCD_WIDTH + cx] = color;
    }
  }
  fillCrosshairDotBuf(frame, cx, cy, crosshairRed565());
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

void frameCacheDrawOverlayBuf(uint16_t* frame, uint32_t frameTs) {
  if (!frame) {
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

  // 复用帧缓冲，避免再堆分配 112KB sprite
  lgfx::LGFX_Sprite spr;
  spr.setColorDepth(16);
  spr.setBuffer(frame, LCD_WIDTH, LCD_HEIGHT, 16);

  const int barH = 24;  // Font2 ≈16 + 8
  spr.fillRect(0, LCD_HEIGHT - barH, LCD_WIDTH, barH, TFT_BLACK);
  spr.setFont(&fonts::Font2);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.drawString(label, LCD_WIDTH / 2, LCD_HEIGHT - barH / 2);
}

bool frameCacheCommit(int zoom) {
  char path[40];
  rgbPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f || f.size() != FRAME_RGB565_BYTES) {
    Serial.printf("commit reject z%d size=%u\n", zoom,
                  f ? (unsigned)f.size() : 0);
    if (f) {
      f.close();
    }
    return false;
  }
  f.close();

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

  readyPath(zoom, path, sizeof(path));
  File r = LittleFS.open(path, "w");
  if (!r) {
    Serial.printf("commit ready open fail z%d\n", zoom);
    return false;
  }
  r.print("1");
  r.close();
  readyMaskSet(zoom, true);
  Serial.printf("frameCache commit z%d rgb565 ok\n", zoom);
  return true;
}
