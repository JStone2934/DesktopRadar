/**
 * ESP32-C3 桌面气象雷达 — SoftAP 配网 + RGB565 全档缓存
 */

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <math.h>
#include <stdio.h>

#include "LGFX_GC9A01.hpp"
#include "app_config.h"
#include "button.h"
#include "compose.h"
#include "config.h"
#include "config_portal.h"
#include "frame_cache.h"
#include "progress_ring.h"
#include "wifi_sta.h"
#include "zoom_ctrl.h"

static LGFX lcd;
static AppConfig s_cfg;

static int s_displayedZoom = -1;
static uint32_t s_lastRefresh = 0;
static uint32_t s_refreshFailAt = 0;  // 非 0：上次定时刷新失败，待短重试
static bool s_busyCompose = false;
static bool s_wifiOk = false;
static bool s_statusScreen = false;  // 首次 Fetching 黑底，圆环不 blit 地图
static int s_bakeZoom = -1;
static float s_bakeLocal = 0.0f;

static float globalCacheDone01() {
  const int slots = frameCacheZoomSlots();
  if (slots <= 0) {
    return 1.0f;
  }
  float ready = (float)frameCacheCountReady();
  if (s_bakeZoom >= ZOOM_MIN && s_bakeZoom <= ZOOM_MAX &&
      !frameCacheHas(s_bakeZoom)) {
    ready += s_bakeLocal;
  }
  if (ready > (float)slots) {
    ready = (float)slots;
  }
  return ready / (float)slots;
}

static void refreshProgressRing() {
  if (!s_cfg.show_progress) {
    return;
  }
  const int under = s_statusScreen ? -1 : s_displayedZoom;
  progressRingUpdate(&lcd, globalCacheDone01(), under);
}

static void onComposeProgress(int zoom, float local01) {
  s_bakeZoom = zoom;
  s_bakeLocal = local01;
  refreshProgressRing();
}

static void showStatus(const char* line1, const char* line2 = nullptr) {
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextDatum(MC_DATUM);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setFont(&fonts::Font4);
  lcd.drawString(line1, LCD_WIDTH / 2, LCD_HEIGHT / 2 - (line2 ? 12 : 0));
  if (line2) {
    lcd.setFont(&fonts::Font2);
    lcd.drawString(line2, LCD_WIDTH / 2, LCD_HEIGHT / 2 + 16);
  }
}

static void clearAllFrameCaches(const char* reason) {
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; z++) {
    frameCacheRemove(z);
  }
  s_displayedZoom = -1;
  Serial.printf("frame caches cleared (%s)\n",
                reason && reason[0] ? reason : "all");
}

static void clearPeerFrameCaches(int keepZoom, const char* reason) {
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; z++) {
    if (z == keepZoom) {
      continue;
    }
    frameCacheRemove(z);
  }
  Serial.printf("peer frame caches cleared keep=z%d (%s)\n", keepZoom,
                reason && reason[0] ? reason : "peers");
}

static bool nearlySameLoc(float aLat, float aLon, float bLat, float bLon) {
  return fabsf(aLat - bLat) < 1e-5f && fabsf(aLon - bLon) < 1e-5f;
}

static bool showCached(int zoom) {
  const uint32_t t0 = millis();
  if (!frameCacheBlit(&lcd, zoom)) {
    return false;
  }
  Serial.printf("blit z%d %lums\n", zoom, (unsigned long)(millis() - t0));
  s_displayedZoom = zoom;
  s_statusScreen = false;
  refreshProgressRing();
  return true;
}

/** 造片阻塞中短按：已缓存则立刻秒切。 */
static void onPendingZoomFeedback(int zoom) {
  if (frameCacheHas(zoom)) {
    showCached(zoom);
    return;
  }
  // 未缓存：保持当前画面，仅靠进度环反映全局缓存
  refreshProgressRing();
}

static bool buildAndCache(int zoom, bool pushToDisplay) {
  if (!zoomCanCompose(zoom)) {
    return false;
  }
  s_busyCompose = true;
  composeClearAbort();
  s_bakeZoom = zoom;
  s_bakeLocal = 0.0f;

  if (pushToDisplay && s_displayedZoom < 0) {
    char line2[20];
    snprintf(line2, sizeof(line2), "zoom %d", zoom);
    s_statusScreen = true;
    showStatus("Fetching...", line2);
    refreshProgressRing();
  } else if (pushToDisplay) {
    Serial.printf("rebuild z%d (keep display)\n", zoom);
  } else {
    Serial.printf("prefetch bake z%d (no display)\n", zoom);
  }

  const bool ok =
      composeRadarFrame(&lcd, s_cfg.lat, s_cfg.lon, zoom, pushToDisplay);
  s_busyCompose = false;
  s_bakeZoom = -1;
  s_bakeLocal = 0.0f;

  if (!ok) {
    if (composeAbortRequested()) {
      Serial.printf("build z%d aborted\n", zoom);
    } else {
      Serial.printf("build z%d fail\n", zoom);
    }
    frameCacheRestoreStale(zoom);
    s_statusScreen = false;
    refreshProgressRing();
    return false;
  }

  if (pushToDisplay) {
    s_displayedZoom = zoom;
    s_statusScreen = false;
  }
  refreshProgressRing();
  return true;
}

static void ensureZoomVisible(int zoom, bool userInitiated) {
  zoomSetCurrent(zoom);

  if (frameCacheHas(zoom)) {
    showCached(zoom);
    zoomPrefetchResetAround(zoom);
    return;
  }

  if (userInitiated && s_busyCompose) {
    composeRequestAbort();
  }
  zoomPrefetchClear();

  if (!buildAndCache(zoom, true)) {
    if (!zoomHasPending() && s_displayedZoom < 0) {
      showStatus("Compose fail", "see serial");
    }
    return;
  }
  zoomPrefetchResetAround(zoom);
}

static void handlePendingZoom() {
  for (;;) {
    const int pending = zoomTakePending();
    if (pending < 0) {
      break;
    }
    Serial.printf("handle pending z%d\n", pending);
    ensureZoomVisible(pending, true);
  }
}

static void handleShortPress() {
  const int next = zoomCycleNext();
  Serial.printf("short press -> z%d\n", next);
  ensureZoomVisible(next, true);
  handlePendingZoom();
}

static void pumpPrefetch() {
  if (zoomHasPending()) {
    handlePendingZoom();
    return;
  }
  if (s_busyCompose || WiFi.status() != WL_CONNECTED) {
    return;
  }
  int z = 0;
  if (!zoomPrefetchPop(&z)) {
    return;
  }
  if (frameCacheHas(z)) {
    return;
  }
  buildAndCache(z, false);
  handlePendingZoom();
}

static bool tryWifiAndRadar() {
  Serial.printf("apply cfg: mode=%u ssid=%s lat=%.5f lon=%.5f\n",
                (unsigned)s_cfg.wifi_mode, s_cfg.ssid, s_cfg.lat, s_cfg.lon);
  showStatus("Connecting...", s_cfg.ssid);
  s_wifiOk = wifiConnect(&s_cfg);
  if (!s_wifiOk) {
    showStatus("WiFi fail", s_cfg.ssid);
    Serial.println("hold BOOT 10s to re-open setup");
    return false;
  }

  char ipBuf[24];
  snprintf(ipBuf, sizeof(ipBuf), "%s", WiFi.localIP().toString().c_str());
  showStatus("WiFi OK", ipBuf);
  delay(600);

  ensureZoomVisible(zoomCurrent(), true);
  if (!frameCacheHas(zoomCurrent())) {
    Serial.printf("compose miss z%d — will retry in loop\n", zoomCurrent());
  }
  s_lastRefresh = millis();
  s_refreshFailAt = 0;
  return true;
}

/** 跑门户；按结果更新 s_cfg，必要时清缓存并重连。 */
static void runPortalAndApply() {
  composeRequestAbort();
  zoomPrefetchClear();
  s_busyCompose = false;
  progressRingHide(&lcd, s_displayedZoom);

  const float oldLat = s_cfg.lat;
  const float oldLon = s_cfg.lon;

  const PortalResult pr =
      configPortalRun(&lcd, CONFIG_PORTAL_TIMEOUT_MS, &s_cfg);

  Serial.printf("portal result=%u ssid=%s\n", (unsigned)pr, s_cfg.ssid);

  if (pr == PortalResult::Saved) {
    if (!nearlySameLoc(oldLat, oldLon, s_cfg.lat, s_cfg.lon)) {
      clearAllFrameCaches("location change");
    }
  }

  if (!s_cfg.show_progress) {
    progressRingHide(&lcd, s_displayedZoom);
  }

  tryWifiAndRadar();
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("ESP32-C3 Radar SoftAP config + RGB565 zoom");

  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(255);

  buttonBegin();
  zoomSetCurrent(MAP_ZOOM);
  zoomSetPendingFeedback(onPendingZoomFeedback);
  composeSetProgressFn(onComposeProgress);

  showStatus("LittleFS...", "");
  if (!frameCacheBegin()) {
    showStatus("FS fail", "no cache");
  }

  // 上电 / RST：先进配置门户
  runPortalAndApply();
}

void loop() {
  static uint32_t lastBeat = 0;

  const ButtonEvent ev = buttonPoll();
  if (ev == ButtonEvent::LongPress) {
    Serial.println("long press -> config portal");
    showStatus("Setup...", "hold release ok");
    delay(200);
    runPortalAndApply();
    return;
  }
  if (ev == ButtonEvent::ShortPress) {
    if (s_wifiOk) {
      handleShortPress();
    }
  }

  if (!s_wifiOk) {
    delay(50);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnect(&s_cfg)) {
      ensureZoomVisible(zoomCurrent(), true);
      // 不重置 s_lastRefresh：避免闪断推迟整轮雷达刷新
    }
    delay(2000);
    return;
  }

  // 当前档无成品时定期重试（避免首次造片失败后干等定时刷新）
  if (!s_busyCompose && !frameCacheHas(zoomCurrent()) &&
      zoomCanCompose(zoomCurrent())) {
    static uint32_t lastUncachedRetry = 0;
    if (millis() - lastUncachedRetry >= 15000) {
      lastUncachedRetry = millis();
      Serial.printf("retry uncached z%d\n", zoomCurrent());
      ensureZoomVisible(zoomCurrent(), true);
    }
  }

  // 到点或失败短重试：作废其它档并重建当前档；成功才推进 s_lastRefresh
  // 失败退避中忽略 refreshDue，避免每圈狂刷
  bool shouldRefresh = false;
  if (s_refreshFailAt != 0) {
    shouldRefresh =
        (millis() - s_refreshFailAt >= RADAR_REFRESH_RETRY_MS);
  } else {
    shouldRefresh = (millis() - s_lastRefresh >= RADAR_REFRESH_MS);
  }
  if (!s_busyCompose && shouldRefresh) {
    const int z = zoomCurrent();
    if (zoomCanCompose(z)) {
      Serial.printf("scheduled refresh focus z%d (%s)\n", z,
                    s_refreshFailAt != 0 ? "retry" : "due");
      zoomPrefetchClear();
      clearPeerFrameCaches(z, "radar refresh");
      if (buildAndCache(z, true)) {
        s_lastRefresh = millis();
        s_refreshFailAt = 0;
      } else {
        s_refreshFailAt = millis() == 0 ? 1 : millis();
        Serial.printf("refresh fail, retry in %lus\n",
                      (unsigned long)(RADAR_REFRESH_RETRY_MS / 1000UL));
      }
      handlePendingZoom();
      zoomPrefetchResetAround(zoomCurrent());
    }
  }

  pumpPrefetch();

  if (millis() - lastBeat >= 5000) {
    lastBeat = millis();
    Serial.printf(
        "[%lu] heap=%u max=%u wifi=%d z=%d cached=%d ring=%d/%d fs=%u/%u\n",
        millis() / 1000, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.RSSI(),
        zoomCurrent(), (int)frameCacheHas(zoomCurrent()),
        frameCacheCountReady(), frameCacheZoomSlots(),
        (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  }

  delay(10);
}
