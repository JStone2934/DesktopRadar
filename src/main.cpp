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
static uint32_t s_labelUntil = 0;
static uint32_t s_lastRefresh = 0;
static bool s_busyCompose = false;
static bool s_wifiOk = false;
static bool s_statusScreen = false;  // Fetching/Baking 黑底，圆环不 blit 地图
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

static void clearAllFrameCaches() {
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; z++) {
    frameCacheRemove(z);
  }
  s_displayedZoom = -1;
  Serial.println("frame caches cleared (location change)");
}

static bool nearlySameLoc(float aLat, float aLon, float bLat, float bLon) {
  return fabsf(aLat - bLat) < 1e-5f && fabsf(aLon - bLon) < 1e-5f;
}

static void overlayZoomLabel(int zoom, const char* note = nullptr) {
  char buf[24];
  if (note && note[0]) {
    snprintf(buf, sizeof(buf), "z%d %s", zoom, note);
  } else {
    snprintf(buf, sizeof(buf), "z%d", zoom);
  }
  lcd.setTextDatum(TC_DATUM);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setFont(&fonts::Font4);
  lcd.drawString(buf, LCD_WIDTH / 2, 18);
  s_labelUntil = millis() + ZOOM_LABEL_MS;
}

static void clearLabelIfDue() {
  if (s_labelUntil == 0 || millis() < s_labelUntil) {
    return;
  }
  s_labelUntil = 0;
  if (s_displayedZoom >= 0 && frameCacheHas(s_displayedZoom)) {
    frameCacheBlit(&lcd, s_displayedZoom);
    refreshProgressRing();
  }
}

static bool showCached(int zoom, bool withLabel) {
  const uint32_t t0 = millis();
  if (!frameCacheBlit(&lcd, zoom)) {
    return false;
  }
  Serial.printf("blit z%d %lums\n", zoom, (unsigned long)(millis() - t0));
  s_displayedZoom = zoom;
  s_statusScreen = false;
  if (withLabel) {
    overlayZoomLabel(zoom);
  }
  refreshProgressRing();
  return true;
}

/** 造片阻塞中短按：已缓存则立刻秒切，否则先打档位标签，避免“按了没反应”。 */
static void onPendingZoomFeedback(int zoom) {
  if (frameCacheHas(zoom)) {
    showCached(zoom, true);
    return;
  }
  overlayZoomLabel(zoom, "...");
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

  if (pushToDisplay) {
    char line2[20];
    snprintf(line2, sizeof(line2), "zoom %d", zoom);
    s_statusScreen = true;
    showStatus("Fetching...", line2);
    refreshProgressRing();
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
    s_statusScreen = false;
    refreshProgressRing();
    return false;
  }

  if (pushToDisplay) {
    s_displayedZoom = zoom;
    s_statusScreen = false;
    // compose 已 pushImage；叠档位标签后再画剩余圆环
    overlayZoomLabel(zoom);
  }
  refreshProgressRing();
  return true;
}

static void ensureZoomVisible(int zoom, bool userInitiated) {
  zoomSetCurrent(zoom);

  if (frameCacheHas(zoom)) {
    showCached(zoom, userInitiated);
    zoomPrefetchResetAround(zoom);
    return;
  }

  if (userInitiated && s_busyCompose) {
    composeRequestAbort();
  }
  zoomPrefetchClear();

  if (!buildAndCache(zoom, true)) {
    if (!zoomHasPending()) {
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
      clearAllFrameCaches();
    }
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

  clearLabelIfDue();

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnect(&s_cfg)) {
      ensureZoomVisible(zoomCurrent(), true);
      s_lastRefresh = millis();
    }
    delay(2000);
    return;
  }

  // 当前档无成品时定期重试（避免首次造片失败后干等 15 分钟刷新）
  if (!s_busyCompose && !frameCacheHas(zoomCurrent()) &&
      zoomCanCompose(zoomCurrent())) {
    static uint32_t lastUncachedRetry = 0;
    if (millis() - lastUncachedRetry >= 15000) {
      lastUncachedRetry = millis();
      Serial.printf("retry uncached z%d\n", zoomCurrent());
      ensureZoomVisible(zoomCurrent(), true);
      s_lastRefresh = millis();
    }
  }

  if (millis() - s_lastRefresh >= RADAR_REFRESH_MS) {
    s_lastRefresh = millis();
    const int z = zoomCurrent();
    if (zoomCanCompose(z)) {
      Serial.printf("scheduled refresh z%d\n", z);
      frameCacheRemove(z);
      buildAndCache(z, true);
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
