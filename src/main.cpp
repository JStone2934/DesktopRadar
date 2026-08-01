/**
 * ESP32-C3 桌面气象雷达 — 底图 + 最新雷达静帧
 */

#include <Arduino.h>
#include <WiFi.h>

#include "LGFX_GC9A01.hpp"
#include "compose.h"
#include "config.h"
#include "wifi_sta.h"

static LGFX lcd;
static LGFX_Sprite frame(&lcd);  // 保留参数兼容；当前直绘 LCD

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

static bool refreshFrame() {
  Serial.println("--- compose frame ---");
  Serial.printf("Free heap before: %u max=%u\n", ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());

  showStatus("Fetching...", "basemap+radar");
  const bool ok = composeRadarFrame(&lcd, &frame, MAP_LAT, MAP_LON, MAP_ZOOM);
  Serial.printf("Free heap after: %u max=%u\n", ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());

  if (!ok) {
    showStatus("Compose fail", "see serial");
    return false;
  }

  Serial.println("Frame displayed");
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("ESP32-C3 Radar static frame");

  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(255);

  showStatus("Connecting...", WIFI_SSID);
  if (!wifiConnect()) {
    showStatus("WiFi fail", WIFI_SSID);
    return;
  }

  char ipBuf[24];
  snprintf(ipBuf, sizeof(ipBuf), "%s", WiFi.localIP().toString().c_str());
  showStatus("WiFi OK", ipBuf);
  delay(600);

  refreshFrame();
}

void loop() {
  static uint32_t lastRefresh = millis();
  static uint32_t lastBeat = 0;

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnect()) {
      refreshFrame();
      lastRefresh = millis();
    }
    delay(2000);
    return;
  }

  if (millis() - lastRefresh >= RADAR_REFRESH_MS) {
    lastRefresh = millis();
    refreshFrame();
  }

  if (millis() - lastBeat >= 5000) {
    lastBeat = millis();
    Serial.printf("[%lu] heap=%u max=%u wifi=%d\n", millis() / 1000,
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.RSSI());
  }
}
