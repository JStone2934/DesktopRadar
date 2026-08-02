#include "wifi_sta.h"

#include <WiFi.h>
#include <string.h>

#include "config.h"

#if __has_include("esp_wpa2.h")
#include "esp_wpa2.h"
#define RADAR_HAS_WPA2_ENT 1
#elif __has_include("esp_eap_client.h")
#include "esp_eap_client.h"
#include "esp_wifi.h"
#define RADAR_HAS_WPA2_ENT 2
#else
#define RADAR_HAS_WPA2_ENT 0
#endif

void wifiDisconnectClean() {
  WiFi.disconnect(true, true);
  delay(100);
}

static bool waitConnected(uint32_t timeoutMs) {
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeoutMs) {
      return false;
    }
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  return true;
}

static bool wifiConnectPsk(AppConfig* cfg) {
  if (!cfg) {
    return false;
  }

  wifiDisconnectClean();
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname("esp32-radar");
  delay(100);

  const size_t passLen = strnlen(cfg->pass, sizeof(cfg->pass));
  Serial.printf("WiFi PSK connecting to %s (passLen=%u) ...\n", cfg->ssid,
                (unsigned)passLen);
  if (passLen == 0) {
    Serial.println("WiFi PSK: password empty — check portal save / NVS");
  }
  WiFi.begin(cfg->ssid, cfg->pass);

  if (waitConnected(WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.printf("WiFi OK, IP=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }

  Serial.printf("WiFi PSK connect timeout status=%d\n", (int)WiFi.status());

  // NVS 密码错误但 SSID 仍是默认热点：回退 config.h 并写回 NVS
  if (strcmp(cfg->ssid, WIFI_SSID) == 0 && strcmp(cfg->pass, WIFI_PASS) != 0) {
    Serial.println("WiFi: retry with config.h WIFI_PASS and repair NVS");
    wifiDisconnectClean();
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    if (!waitConnected(WIFI_CONNECT_TIMEOUT_MS)) {
      Serial.printf("WiFi retry timeout status=%d\n", (int)WiFi.status());
      return false;
    }
    strncpy(cfg->pass, WIFI_PASS, sizeof(cfg->pass) - 1);
    cfg->pass[sizeof(cfg->pass) - 1] = '\0';
    if (appConfigSave(cfg)) {
      Serial.println("WiFi: NVS password repaired");
    }
    Serial.printf("WiFi OK, IP=%s RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  return false;
}

static bool wifiConnectPeap(const AppConfig& cfg) {
#if RADAR_HAS_WPA2_ENT == 0
  Serial.println("WiFi PEAP: enterprise API not available in this core");
  return false;
#else
  if (cfg.identity[0] == '\0') {
    Serial.println("WiFi PEAP: identity empty");
    return false;
  }

  wifiDisconnectClean();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-radar");

#if RADAR_HAS_WPA2_ENT == 1
  esp_wifi_sta_wpa2_ent_set_identity((uint8_t*)cfg.identity,
                                     strlen(cfg.identity));
  esp_wifi_sta_wpa2_ent_set_username((uint8_t*)cfg.identity,
                                     strlen(cfg.identity));
  esp_wifi_sta_wpa2_ent_set_password((uint8_t*)cfg.pass, strlen(cfg.pass));
  esp_wifi_sta_wpa2_ent_enable();
#else
  esp_eap_client_set_identity((uint8_t*)cfg.identity, strlen(cfg.identity));
  esp_eap_client_set_username((uint8_t*)cfg.identity, strlen(cfg.identity));
  esp_eap_client_set_password((uint8_t*)cfg.pass, strlen(cfg.pass));
  esp_wifi_sta_enterprise_enable();
#endif

  WiFi.begin(cfg.ssid);
  const size_t passLen = strnlen(cfg.pass, sizeof(cfg.pass));
  Serial.printf("WiFi PEAP connecting to %s id=%s passLen=%u ...\n", cfg.ssid,
                cfg.identity, (unsigned)passLen);
  if (passLen == 0) {
    Serial.println("WiFi PEAP: password empty — check portal save / NVS");
  }

  if (!waitConnected(WIFI_CONNECT_TIMEOUT_MS)) {
    Serial.println("WiFi PEAP connect timeout");
    return false;
  }
  Serial.printf("WiFi OK (PEAP), IP=%s RSSI=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
#endif
}

bool wifiConnect(AppConfig* cfg) {
  if (!cfg || cfg->ssid[0] == '\0') {
    Serial.println("WiFi: empty SSID");
    return false;
  }
  if (cfg->wifi_mode == APP_WIFI_PEAP) {
    return wifiConnectPeap(*cfg);
  }
  return wifiConnectPsk(cfg);
}
