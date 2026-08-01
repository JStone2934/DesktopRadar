#include "wifi_sta.h"

#include <WiFi.h>

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

static bool wifiConnectPsk(const AppConfig& cfg) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-radar");
  WiFi.begin(cfg.ssid, cfg.pass);
  Serial.printf("WiFi PSK connecting to %s ...\n", cfg.ssid);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("WiFi PSK connect timeout");
      return false;
    }
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  Serial.printf("WiFi OK, IP=%s RSSI=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
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
  Serial.printf("WiFi PEAP connecting to %s id=%s ...\n", cfg.ssid,
                cfg.identity);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("WiFi PEAP connect timeout");
      return false;
    }
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  Serial.printf("WiFi OK (PEAP), IP=%s RSSI=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
#endif
}

bool wifiConnect(const AppConfig& cfg) {
  if (cfg.ssid[0] == '\0') {
    Serial.println("WiFi: empty SSID");
    return false;
  }
  if (cfg.wifi_mode == APP_WIFI_PEAP) {
    return wifiConnectPeap(cfg);
  }
  return wifiConnectPsk(cfg);
}
