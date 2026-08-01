#include "wifi_sta.h"

#include <WiFi.h>

#include "config.h"

bool wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-radar");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi connecting to %s ...\n", WIFI_SSID);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("WiFi connect timeout");
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
