#include "wind_field.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "config.h"
#include "http_fetch.h"
#include "zoom_ctrl.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr uint32_t kDiskMagic = 0x31444E57UL;  // WND1
constexpr uint16_t kDiskVersion = 1;
constexpr int kNodes = WIND_GRID_N * WIND_GRID_N;

struct __attribute__((packed)) WindDisk {
  uint32_t magic;
  uint16_t version;
  int16_t zoom;
  float centerLat;
  float centerLon;
  uint32_t modelTime;
  int16_t east10[kNodes];
  int16_t south10[kNodes];
  uint32_t checksum;
};

static bool s_enabled = false;
static bool s_fetching = false;
static bool s_valid = false;
static bool s_waitingFirstAttempt = false;
static float s_lat = 0.0f;
static float s_lon = 0.0f;
static int s_zoom = -1;
static uint32_t s_fetchDueAt = 0;
static uint32_t s_revision = 0;
static uint32_t s_modelTime = 0;
static uint8_t s_consecutiveFailures = 0;
static int16_t s_east10[kNodes];
static int16_t s_south10[kNodes];

static bool reached(uint32_t when) {
  return when != 0 && (int32_t)(millis() - when) >= 0;
}

static uint32_t fnv1a(const uint8_t* data, size_t n) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < n; ++i) {
    h ^= data[i];
    h *= 16777619UL;
  }
  return h;
}

static void cachePath(int zoom, bool temp, char* out, size_t outLen) {
  snprintf(out, outLen, temp ? "/wind/z%02d.bin.tmp" : "/wind/z%02d.bin",
           zoom);
}

static bool sameLocation(float aLat, float aLon, float bLat, float bLon) {
  return fabsf(aLat - bLat) < 1e-5f && fabsf(aLon - bLon) < 1e-5f;
}

static bool loadCache() {
  if (s_zoom < ZOOM_MIN || s_zoom > ZOOM_MAX) {
    return false;
  }
  char path[32];
  cachePath(s_zoom, false, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f || f.size() != sizeof(WindDisk)) {
    if (f) {
      f.close();
    }
    return false;
  }
  WindDisk disk;
  const size_t nr = f.read(reinterpret_cast<uint8_t*>(&disk), sizeof(disk));
  f.close();
  if (nr != sizeof(disk) || disk.magic != kDiskMagic ||
      disk.version != kDiskVersion || disk.zoom != s_zoom ||
      !sameLocation(disk.centerLat, disk.centerLon, s_lat, s_lon)) {
    return false;
  }
  const uint32_t sum =
      fnv1a(reinterpret_cast<const uint8_t*>(&disk), offsetof(WindDisk, checksum));
  if (sum != disk.checksum || disk.modelTime == 0) {
    return false;
  }
  memcpy(s_east10, disk.east10, sizeof(s_east10));
  memcpy(s_south10, disk.south10, sizeof(s_south10));
  s_modelTime = disk.modelTime;
  s_valid = true;
  ++s_revision;
  Serial.printf("wind cache load z%d time=%lu rev=%lu\n", s_zoom,
                (unsigned long)s_modelTime, (unsigned long)s_revision);
  return true;
}

static bool saveCache() {
  if (!s_valid || s_zoom < ZOOM_MIN || s_zoom > ZOOM_MAX) {
    return false;
  }
  if (!LittleFS.exists("/wind") && !LittleFS.mkdir("/wind")) {
    return false;
  }
  WindDisk disk;
  memset(&disk, 0, sizeof(disk));
  disk.magic = kDiskMagic;
  disk.version = kDiskVersion;
  disk.zoom = (int16_t)s_zoom;
  disk.centerLat = s_lat;
  disk.centerLon = s_lon;
  disk.modelTime = s_modelTime;
  memcpy(disk.east10, s_east10, sizeof(s_east10));
  memcpy(disk.south10, s_south10, sizeof(s_south10));
  disk.checksum =
      fnv1a(reinterpret_cast<const uint8_t*>(&disk), offsetof(WindDisk, checksum));

  char path[32];
  char tmp[32];
  cachePath(s_zoom, false, path, sizeof(path));
  cachePath(s_zoom, true, tmp, sizeof(tmp));
  LittleFS.remove(tmp);
  File f = LittleFS.open(tmp, "w");
  if (!f) {
    return false;
  }
  const size_t nw = f.write(reinterpret_cast<const uint8_t*>(&disk), sizeof(disk));
  f.flush();
  f.close();
  if (nw != sizeof(disk)) {
    LittleFS.remove(tmp);
    return false;
  }
  LittleFS.remove(path);
  if (!LittleFS.rename(tmp, path)) {
    LittleFS.remove(tmp);
    return false;
  }
  return true;
}

struct LatLon {
  double lat;
  double lon;
};

static LatLon screenToLatLon(int sx, int sy) {
  const double world = (double)TILE_SIZE * (double)(1UL << s_zoom);
  const double centerX = (s_lon + 180.0) / 360.0 * world;
  const double latRad = (double)s_lat * M_PI / 180.0;
  const double centerY =
      (1.0 - log(tan(latRad) + 1.0 / cos(latRad)) / M_PI) * 0.5 * world;
  const double gx = centerX - (double)VIEW_HALF + (double)sx;
  const double gy = centerY - (double)VIEW_HALF + (double)sy;
  LatLon out;
  out.lon = gx / world * 360.0 - 180.0;
  const double n = M_PI - 2.0 * M_PI * gy / world;
  out.lat = 180.0 / M_PI * atan(sinh(n));
  if (out.lat > 85.0) {
    out.lat = 85.0;
  } else if (out.lat < -85.0) {
    out.lat = -85.0;
  }
  return out;
}

static String buildUrl(int pointLimit) {
  if (pointLimit < 1 || pointLimit > kNodes) {
    pointLimit = kNodes;
  }
  String lats;
  String lons;
  lats.reserve(520);
  lons.reserve(560);
  int points = 0;
  for (int gy = 0; gy < WIND_GRID_N; ++gy) {
    const int sy = (gy * (LCD_HEIGHT - 1) + (WIND_GRID_N - 2) / 2) /
                   (WIND_GRID_N - 1);
    for (int gx = 0; gx < WIND_GRID_N; ++gx) {
      if (points >= pointLimit) {
        break;
      }
      const int sx = (gx * (LCD_WIDTH - 1) + (WIND_GRID_N - 2) / 2) /
                     (WIND_GRID_N - 1);
      const LatLon ll = screenToLatLon(sx, sy);
      if (lats.length()) {
        lats += ',';
        lons += ',';
      }
      lats += String(ll.lat, 4);
      lons += String(ll.lon, 4);
      ++points;
    }
    if (points >= pointLimit) {
      break;
    }
  }

  String url;
  url.reserve(1250);
  url += WIND_API;
  url += "?latitude=";
  url += lats;
  url += "&longitude=";
  url += lons;
  url += "&hourly=wind_speed_10m,wind_direction_10m";
  url += "&forecast_hours=1&wind_speed_unit=ms&timeformat=unixtime";
  url += "&timezone=GMT&cell_selection=nearest";
  return url;
}

static int16_t q10(float v) {
  if (v > 120.0f) {
    v = 120.0f;
  } else if (v < -120.0f) {
    v = -120.0f;
  }
  return (int16_t)lroundf(v * 10.0f);
}

static bool fetchField() {
  const String url = buildUrl(kNodes);
  Serial.printf("wind fetch z%d points=%d urlLen=%u heap=%u max=%u\n", s_zoom,
                kNodes, (unsigned)url.length(), ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());

  size_t len = 0;
  uint8_t* data = httpFetch(url.c_str(), nullptr, WIND_MAX_JSON_BYTES, &len,
                            WIND_HTTP_TIMEOUT_MS);
  if (!data) {
    Serial.println("wind fetch HTTP fail");
    return false;
  }

  JsonDocument filter;
  JsonArray filterArray = filter.to<JsonArray>();
  JsonObject filterItem = filterArray.add<JsonObject>();
  filterItem["hourly"]["time"] = true;
  filterItem["hourly"]["wind_speed_10m"] = true;
  filterItem["hourly"]["wind_direction_10m"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, data, len, DeserializationOption::Filter(filter));
  free(data);
  if (err) {
    Serial.printf("wind JSON error: %s len=%u\n", err.c_str(), (unsigned)len);
    return false;
  }

  JsonArrayConst locations = doc.as<JsonArrayConst>();
  if (locations.size() != kNodes) {
    Serial.printf("wind locations mismatch %u/%d\n", (unsigned)locations.size(),
                  kNodes);
    return false;
  }

  int16_t east[kNodes];
  int16_t south[kNodes];
  uint32_t modelTime = 0;
  for (int i = 0; i < kNodes; ++i) {
    JsonObjectConst hourly = locations[i]["hourly"].as<JsonObjectConst>();
    JsonArrayConst times = hourly["time"].as<JsonArrayConst>();
    JsonArrayConst speeds = hourly["wind_speed_10m"].as<JsonArrayConst>();
    JsonArrayConst directions =
        hourly["wind_direction_10m"].as<JsonArrayConst>();
    if (times.size() < 1 || speeds.size() < 1 || directions.size() < 1) {
      Serial.printf("wind node %d missing\n", i);
      return false;
    }
    const uint32_t t = times[0] | 0UL;
    const float speed = speeds[0].as<float>();
    const float direction = directions[0].as<float>();
    if (t == 0 || !isfinite(speed) || !isfinite(direction)) {
      Serial.printf("wind node %d invalid\n", i);
      return false;
    }
    if (modelTime == 0) {
      modelTime = t;
    }
    const float rad = direction * (float)M_PI / 180.0f;
    // 气象风向表示“风从哪里来”；屏幕 +x 向东、+y 向南。
    east[i] = q10(-speed * sinf(rad));
    south[i] = q10(speed * cosf(rad));
  }

  memcpy(s_east10, east, sizeof(s_east10));
  memcpy(s_south10, south, sizeof(s_south10));
  s_modelTime = modelTime;
  s_valid = true;
  ++s_revision;
  saveCache();
  Serial.printf("wind fetch ok z%d bytes=%u time=%lu rev=%lu\n", s_zoom,
                (unsigned)len, (unsigned long)s_modelTime,
                (unsigned long)s_revision);
  return true;
}

}  // namespace

bool windFieldBegin() {
  if (!LittleFS.exists("/wind") && !LittleFS.mkdir("/wind")) {
    Serial.println("wind cache mkdir fail");
    return false;
  }
  return true;
}

void windFieldSetEnabled(bool enabled) {
  s_enabled = enabled;
  if (!enabled) {
    s_waitingFirstAttempt = false;
    s_consecutiveFailures = 0;
  }
}

bool windFieldEnabled() { return s_enabled; }

void windFieldSelect(float lat, float lon, int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return;
  }
  if (s_zoom == zoom && sameLocation(s_lat, s_lon, lat, lon)) {
    return;
  }
  s_lat = lat;
  s_lon = lon;
  s_zoom = zoom;
  s_valid = false;
  s_modelTime = 0;
  s_consecutiveFailures = 0;
  const bool cached = loadCache();
  const uint32_t now = millis();
  s_fetchDueAt = now + (cached ? 5000UL : WIND_FIELD_SETTLE_MS);
  if (s_fetchDueAt == 0) {
    s_fetchDueAt = 1;
  }
  s_waitingFirstAttempt = !cached;
  Serial.printf("wind select z%d cached=%d due=%lums\n", s_zoom, (int)cached,
                (unsigned long)(cached ? 5000UL : WIND_FIELD_SETTLE_MS));
}

bool windFieldService() {
  if (!s_enabled || s_fetching || s_zoom < ZOOM_MIN ||
      WiFi.status() != WL_CONNECTED || !reached(s_fetchDueAt)) {
    return false;
  }
  s_fetching = true;
  composeClearAbort();
  const bool ok = fetchField();
  s_fetching = false;
  if (!ok && composeAbortRequested()) {
    // 缩放抢占不是网络故障：不累计失败、不触发 WiFi 重连，短延时后只在
    // 新档仍需要时重试。没有旧场时继续保持“首拉优先于雷达预取”。
    s_waitingFirstAttempt = !s_valid;
    s_fetchDueAt = millis() + 300UL;
    if (s_fetchDueAt == 0) {
      s_fetchDueAt = 1;
    }
    Serial.printf("wind fetch preempted by zoom z%d\n", s_zoom);
    return true;
  }
  s_waitingFirstAttempt = false;
  uint32_t wait = WIND_FIELD_REFRESH_MS;
  if (ok) {
    s_consecutiveFailures = 0;
  } else {
    if (s_consecutiveFailures < 255) {
      ++s_consecutiveFailures;
    }
    // 风场模式专用恢复：若 WiFi 表面仍在线但连续两次外网请求失败，
    // 主动重关联以刷新 DHCP 路由/DNS/NAT 状态。关闭风场时不会触发。
    if (s_consecutiveFailures == 2 && WiFi.status() == WL_CONNECTED) {
      Serial.println("wind network recovery: reconnect after 2 failures");
      WiFi.disconnect(false, false);
    }
    wait = s_consecutiveFailures <= WIND_FIELD_FAST_RETRY_LIMIT
               ? WIND_FIELD_RETRY_MS
               : WIND_FIELD_SLOW_RETRY_MS;
    Serial.printf("wind retry fail=%u in=%lus\n",
                  (unsigned)s_consecutiveFailures,
                  (unsigned long)(wait / 1000UL));
  }
  s_fetchDueAt = millis() + wait;
  if (s_fetchDueAt == 0) {
    s_fetchDueAt = 1;
  }
  return true;
}

bool windFieldBlocksPrefetch() {
  return s_enabled && s_waitingFirstAttempt && !s_valid &&
         WiFi.status() == WL_CONNECTED;
}

bool windFieldReadyFor(int zoom) {
  return s_enabled && s_valid && zoom == s_zoom;
}

bool windFieldSample(float screenX, float screenY, float* east, float* south) {
  if (!east || !south || !s_enabled || !s_valid) {
    return false;
  }
  if (screenX < 0.0f) {
    screenX = 0.0f;
  } else if (screenX > (float)(LCD_WIDTH - 1)) {
    screenX = (float)(LCD_WIDTH - 1);
  }
  if (screenY < 0.0f) {
    screenY = 0.0f;
  } else if (screenY > (float)(LCD_HEIGHT - 1)) {
    screenY = (float)(LCD_HEIGHT - 1);
  }
  const float gx = screenX * (float)(WIND_GRID_N - 1) /
                   (float)(LCD_WIDTH - 1);
  const float gy = screenY * (float)(WIND_GRID_N - 1) /
                   (float)(LCD_HEIGHT - 1);
  int x0 = (int)floorf(gx);
  int y0 = (int)floorf(gy);
  if (x0 >= WIND_GRID_N - 1) {
    x0 = WIND_GRID_N - 2;
  }
  if (y0 >= WIND_GRID_N - 1) {
    y0 = WIND_GRID_N - 2;
  }
  const float tx = gx - (float)x0;
  const float ty = gy - (float)y0;
  const int i00 = y0 * WIND_GRID_N + x0;
  const int i10 = i00 + 1;
  const int i01 = i00 + WIND_GRID_N;
  const int i11 = i01 + 1;
  const float e0 = (float)s_east10[i00] * (1.0f - tx) +
                   (float)s_east10[i10] * tx;
  const float e1 = (float)s_east10[i01] * (1.0f - tx) +
                   (float)s_east10[i11] * tx;
  const float s0 = (float)s_south10[i00] * (1.0f - tx) +
                   (float)s_south10[i10] * tx;
  const float s1 = (float)s_south10[i01] * (1.0f - tx) +
                   (float)s_south10[i11] * tx;
  *east = (e0 * (1.0f - ty) + e1 * ty) * 0.1f;
  *south = (s0 * (1.0f - ty) + s1 * ty) * 0.1f;
  return true;
}

uint32_t windFieldRevision() { return s_revision; }
uint32_t windFieldModelTime() { return s_modelTime; }
