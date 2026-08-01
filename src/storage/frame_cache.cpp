#include "frame_cache.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dirPath(int zoom, char* out, size_t n) {
  snprintf(out, n, "/frames/z%02d", zoom);
}

static void readyPath(int zoom, char* out, size_t n) {
  snprintf(out, n, "/frames/z%02d/ready", zoom);
}

static void metaPath(int zoom, char* out, size_t n) {
  snprintf(out, n, "/frames/z%02d/meta.txt", zoom);
}

static void tilePath(int zoom, bool isRadar, int tx, int ty, char* out,
                     size_t n) {
  snprintf(out, n, "/frames/z%02d/%c_%d_%d.png", zoom, isRadar ? 'r' : 'b', tx,
           ty);
}

static void removeDirContents(int zoom) {
  char path[48];
  readyPath(zoom, path, sizeof(path));
  LittleFS.remove(path);
  metaPath(zoom, path, sizeof(path));

  File meta = LittleFS.open(path, "r");
  if (meta) {
    // 读出瓦片范围后删文件
    int z = 0, tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0, haveRadar = 0;
    double opx = 0, opy = 0;
    if (meta.available()) {
      String line = meta.readStringUntil('\n');
      sscanf(line.c_str(), "%d", &z);
      line = meta.readStringUntil('\n');
      sscanf(line.c_str(), "%d %d %d %d", &tx0, &ty0, &tx1, &ty1);
      line = meta.readStringUntil('\n');
      sscanf(line.c_str(), "%lf %lf", &opx, &opy);
      line = meta.readStringUntil('\n');
      sscanf(line.c_str(), "%d", &haveRadar);
    }
    meta.close();
    LittleFS.remove(path);
    for (int ty = ty0; ty <= ty1; ++ty) {
      for (int tx = tx0; tx <= tx1; ++tx) {
        tilePath(zoom, false, tx, ty, path, sizeof(path));
        LittleFS.remove(path);
        if (haveRadar) {
          tilePath(zoom, true, tx, ty, path, sizeof(path));
          LittleFS.remove(path);
        }
      }
    }
  } else {
    LittleFS.remove(path);
  }
}

bool frameCacheBegin() {
  if (LittleFS.begin(false)) {
    if (!LittleFS.exists("/frames")) {
      LittleFS.mkdir("/frames");
    }
    Serial.println("LittleFS mounted");
    return true;
  }
  Serial.println("LittleFS mount fail, formatting...");
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS format+mount fail");
    return false;
  }
  LittleFS.mkdir("/frames");
  Serial.println("LittleFS formatted and mounted");
  return true;
}

bool frameCacheHas(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }
  char path[40];
  readyPath(zoom, path, sizeof(path));
  return LittleFS.exists(path);
}

bool frameCacheRemove(int zoom) {
  removeDirContents(zoom);
  char dir[32];
  dirPath(zoom, dir, sizeof(dir));
  // LittleFS 可能不支持 rmdir；忽略失败
  LittleFS.rmdir(dir);
  return true;
}

bool frameCachePrepare(int zoom) {
  removeDirContents(zoom);
  if (!LittleFS.exists("/frames")) {
    LittleFS.mkdir("/frames");
  }
  char dir[32];
  dirPath(zoom, dir, sizeof(dir));
  if (!LittleFS.exists(dir)) {
    if (!LittleFS.mkdir(dir)) {
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
  tilePath(zoom, isRadar, tx, ty, path, sizeof(path));
  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.printf("saveTile open fail %s\n", path);
    return false;
  }
  const size_t wrote = f.write(data, len);
  f.flush();
  f.close();
  if (wrote != len) {
    Serial.printf("saveTile short %u/%u %s\n", (unsigned)wrote, (unsigned)len,
                  path);
    LittleFS.remove(path);
    return false;
  }
  return true;
}

bool frameCacheWriteMeta(int zoom, const Viewport& vp, bool haveRadar) {
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
  f.flush();
  f.close();
  return true;
}

bool frameCacheCommit(int zoom) {
  char path[40];
  readyPath(zoom, path, sizeof(path));
  File f = LittleFS.open(path, "w");
  if (!f) {
    return false;
  }
  f.print("1");
  f.close();
  Serial.printf("frameCache commit z%d\n", zoom);
  return true;
}

static bool drawPngFileClipped(LGFX* lcd, const char* path, int ox, int oy) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    return false;
  }
  const size_t len = f.size();
  if (len < 8 || len > HTTP_MAX_TILE_BYTES) {
    f.close();
    return false;
  }
  uint8_t* data = (uint8_t*)malloc(len);
  if (!data) {
    Serial.printf("drawPngFile malloc fail %u max=%u\n", (unsigned)len,
                  ESP.getMaxAllocHeap());
    f.close();
    return false;
  }
  if (f.read(data, len) != (int)len) {
    free(data);
    f.close();
    return false;
  }
  f.close();

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
  bool ok = true;
  if (dest_x < LCD_WIDTH && dest_y < LCD_HEIGHT && max_w > 0 && max_h > 0) {
    if (dest_x + max_w > LCD_WIDTH) {
      max_w = LCD_WIDTH - dest_x;
    }
    if (dest_y + max_h > LCD_HEIGHT) {
      max_h = LCD_HEIGHT - dest_y;
    }
    lcd->releasePngMemory();
    ok = lcd->drawPng(data, (uint32_t)len, dest_x, dest_y, max_w, max_h, off_x,
                      off_y, 1.0f, 1.0f);
  }
  free(data);
  delay(10);
  return ok;
}

bool frameCacheDraw(LGFX* lcd, int zoom) {
  if (!lcd || !frameCacheHas(zoom)) {
    return false;
  }
  char path[48];
  metaPath(zoom, path, sizeof(path));
  File meta = LittleFS.open(path, "r");
  if (!meta) {
    return false;
  }
  int z = 0, tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0, haveRadar = 0;
  double origin_px = 0, origin_py = 0;
  String line = meta.readStringUntil('\n');
  sscanf(line.c_str(), "%d", &z);
  line = meta.readStringUntil('\n');
  sscanf(line.c_str(), "%d %d %d %d", &tx0, &ty0, &tx1, &ty1);
  line = meta.readStringUntil('\n');
  sscanf(line.c_str(), "%lf %lf", &origin_px, &origin_py);
  line = meta.readStringUntil('\n');
  sscanf(line.c_str(), "%d", &haveRadar);
  meta.close();

  lcd->fillScreen(lcd->color565(BASEMAP_BACKDROP_R, BASEMAP_BACKDROP_G,
                                BASEMAP_BACKDROP_B));

  int ok = 0;
  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      const int ox = (int)lround(tx * TILE_SIZE - origin_px);
      const int oy = (int)lround(ty * TILE_SIZE - origin_py);
      tilePath(zoom, false, tx, ty, path, sizeof(path));
      if (drawPngFileClipped(lcd, path, ox, oy)) {
        ++ok;
      }
    }
  }
  if (haveRadar) {
    for (int ty = ty0; ty <= ty1; ++ty) {
      for (int tx = tx0; tx <= tx1; ++tx) {
        const int ox = (int)lround(tx * TILE_SIZE - origin_px);
        const int oy = (int)lround(ty * TILE_SIZE - origin_py);
        tilePath(zoom, true, tx, ty, path, sizeof(path));
        if (LittleFS.exists(path)) {
          drawPngFileClipped(lcd, path, ox, oy);
        }
      }
    }
  }

  lcd->releasePngMemory();
  lcd->drawFastHLine(LCD_WIDTH / 2 - 8, LCD_HEIGHT / 2, 17,
                     lcd->color565(255, 255, 255));
  lcd->drawFastVLine(LCD_WIDTH / 2, LCD_HEIGHT / 2 - 8, 17,
                     lcd->color565(255, 255, 255));

  Serial.printf("frameCacheDraw z%d tiles_ok=%d\n", zoom, ok);
  return ok > 0;
}
