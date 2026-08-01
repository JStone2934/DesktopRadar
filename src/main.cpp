/**
 * ESP32-C3 桌面气象雷达 — 静帧多档缩放 + 瓦片 PNG 缓存
 */

#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>

#include "LGFX_GC9A01.hpp"
#include "button.h"
#include "compose.h"
#include "config.h"
#include "frame_cache.h"
#include "wifi_sta.h"
#include "zoom_ctrl.h"

static LGFX lcd;

static int s_displayedZoom = -1;
static uint32_t s_labelUntil = 0;
static uint32_t s_lastRefresh = 0;
static bool s_busyCompose = false;

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
    frameCacheDraw(&lcd, s_displayedZoom);
  }
}

static bool showCached(int zoom, bool withLabel) {
  if (!frameCacheDraw(&lcd, zoom)) {
    return false;
  }
  s_displayedZoom = zoom;
  if (withLabel) {
    overlayZoomLabel(zoom);
  }
  return true;
}

static bool buildAndCache(int zoom, bool pushToDisplay) {
  if (!zoomCanCompose(zoom)) {
    return false;
  }
  s_busyCompose = true;
  composeClearAbort();

  if (pushToDisplay) {
    char line2[20];
    snprintf(line2, sizeof(line2), "zoom %d", zoom);
    showStatus("Fetching...", line2);
  } else {
    Serial.printf("prefetch tiles z%d (no display)\n", zoom);
  }

  const bool ok = composeRadarFrame(&lcd, MAP_LAT, MAP_LON, zoom, pushToDisplay);
  s_busyCompose = false;

  if (!ok) {
    if (composeAbortRequested()) {
      Serial.printf("build z%d aborted\n", zoom);
    } else {
      Serial.printf("build z%d fail\n", zoom);
    }
    return false;
  }

  if (pushToDisplay) {
    s_displayedZoom = zoom;
    overlayZoomLabel(zoom);
  }
  return true;
}

static void ensureZoomVisible(int zoom, bool userInitiated) {
  zoomSetCurrent(zoom);

  if (frameCacheHas(zoom)) {
    showCached(zoom, userInitiated);
    zoomPrefetchResetAround(zoom);
    return;
  }

  if (!zoomCanCompose(zoom)) {
    if (s_displayedZoom >= 0 && frameCacheHas(s_displayedZoom)) {
      showCached(s_displayedZoom, false);
    }
    overlayZoomLabel(zoom, "soon");
    Serial.printf("z%d upsample deferred\n", zoom);
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

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("ESP32-C3 Radar multi-zoom (tile cache)");

  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(255);

  buttonBegin();
  zoomSetCurrent(MAP_ZOOM);

  showStatus("LittleFS...", "");
  if (!frameCacheBegin()) {
    showStatus("FS fail", "no cache");
  }

  showStatus("Connecting...", WIFI_SSID);
  if (!wifiConnect()) {
    showStatus("WiFi fail", WIFI_SSID);
    return;
  }

  char ipBuf[24];
  snprintf(ipBuf, sizeof(ipBuf), "%s", WiFi.localIP().toString().c_str());
  showStatus("WiFi OK", ipBuf);
  delay(600);

  ensureZoomVisible(zoomCurrent(), true);
  s_lastRefresh = millis();
}

void loop() {
  static uint32_t lastBeat = 0;

  const ButtonEvent ev = buttonPoll();
  if (ev == ButtonEvent::ShortPress) {
    handleShortPress();
  }

  clearLabelIfDue();

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnect()) {
      ensureZoomVisible(zoomCurrent(), true);
      s_lastRefresh = millis();
    }
    delay(2000);
    return;
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
    Serial.printf("[%lu] heap=%u max=%u wifi=%d z=%d cached=%d\n",
                  millis() / 1000, ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                  WiFi.RSSI(), zoomCurrent(), (int)frameCacheHas(zoomCurrent()));
  }

  delay(10);
}
