#include "wifi_sta.h"

#include <WiFi.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#if __has_include("esp_wpa2.h")
#include "esp_wpa2.h"
#define RADAR_HAS_WPA2_ENT 1
#elif __has_include("esp_eap_client.h")
#include "esp_eap_client.h"
#define RADAR_HAS_WPA2_ENT 2
#else
#define RADAR_HAS_WPA2_ENT 0
#endif

static volatile bool s_eventGotIp = false;
static volatile bool s_eventStaConnected = false;
static volatile bool s_eventStaDisconnected = false;
static volatile uint8_t s_lastDisconnectReason = 0;
static volatile int8_t s_lastDisconnectRssi = 0;
static bool s_eventRegistered = false;

struct ApChoice {
  bool found;
  uint8_t bssid[6];
  uint8_t channel;
  int32_t rssi;
  wifi_auth_mode_t auth;
};

static void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      s_eventStaConnected = true;
      s_eventStaDisconnected = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      s_eventGotIp = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      s_eventStaConnected = false;
      s_eventGotIp = false;
      s_eventStaDisconnected = true;
      s_lastDisconnectReason = info.wifi_sta_disconnected.reason;
      s_lastDisconnectRssi = info.wifi_sta_disconnected.rssi;
      break;
    default:
      break;
  }
}

static void ensureWifiEvents() {
  if (!s_eventRegistered) {
    WiFi.onEvent(wifiEvent);
    s_eventRegistered = true;
  }
}

static void resetWifiEvents() {
  s_eventGotIp = false;
  s_eventStaConnected = false;
  s_eventStaDisconnected = false;
  s_lastDisconnectReason = 0;
  s_lastDisconnectRssi = 0;
}

static void logEspErr(const char* label, esp_err_t err) {
  if (err != ESP_OK) {
    Serial.printf("%s failed: %s (0x%x)\n", label, esp_err_to_name(err),
                  (unsigned)err);
  }
}

static void wifiDisableEnterprise() {
#if RADAR_HAS_WPA2_ENT == 1
  esp_wifi_sta_wpa2_ent_disable();
  esp_wifi_sta_wpa2_ent_clear_identity();
  esp_wifi_sta_wpa2_ent_clear_username();
  esp_wifi_sta_wpa2_ent_clear_password();
  esp_wifi_sta_wpa2_ent_clear_new_password();
  esp_wifi_sta_wpa2_ent_clear_ca_cert();
  esp_wifi_sta_wpa2_ent_clear_cert_key();
#elif RADAR_HAS_WPA2_ENT == 2
  esp_wifi_sta_enterprise_disable();
  esp_eap_client_clear_identity();
  esp_eap_client_clear_username();
  esp_eap_client_clear_password();
  esp_eap_client_clear_new_password();
  esp_eap_client_clear_ca_cert();
  esp_eap_client_clear_certificate_and_key();
#endif
}

void wifiDisconnectClean() {
  ensureWifiEvents();
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(100);
  wifiDisableEnterprise();
  resetWifiEvents();
}

static bool prepareStaRadio() {
  ensureWifiEvents();
  resetWifiEvents();

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(300);
  wifiDisableEnterprise();
  esp_wifi_disconnect();
  esp_wifi_stop();
  delay(300);
  WiFi.mode(WIFI_OFF);
  delay(400);

  bool staOk = false;
  for (uint8_t i = 0; i < 3; ++i) {
    if (WiFi.mode(WIFI_STA)) {
      staOk = true;
      break;
    }
    Serial.printf("WiFi: STA mode retry %u\n", (unsigned)(i + 1));
    delay(300);
  }
  if (!staOk) {
    Serial.println("WiFi: failed to enter STA mode after retries");
    return false;
  }
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setHostname("esp32-radar");
  logEspErr("esp_wifi_set_storage", esp_wifi_set_storage(WIFI_STORAGE_RAM));
  delay(100);
  return true;
}

static const char* authName(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN:
      return "OPEN";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2_ENT";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/WPA3_PSK";
    case WIFI_AUTH_WPA3_ENT_192:
      return "WPA3_ENT";
    default:
      return "UNKNOWN";
  }
}

static bool authMatchesMode(wifi_auth_mode_t auth, AppWifiMode mode) {
  if (mode == APP_WIFI_OPEN) {
    return auth == WIFI_AUTH_OPEN;
  }
  if (mode == APP_WIFI_PEAP) {
    return auth == WIFI_AUTH_WPA2_ENTERPRISE || auth == WIFI_AUTH_WPA3_ENT_192;
  }
  return auth != WIFI_AUTH_OPEN && auth != WIFI_AUTH_WPA2_ENTERPRISE &&
         auth != WIFI_AUTH_WPA3_ENT_192;
}

static ApChoice scanBestAp(const char* ssid, AppWifiMode mode) {
  ApChoice best{};
  best.rssi = -127;

  Serial.printf("WiFi scan target: ssid=%s mode=%u\n", ssid, (unsigned)mode);
  const int found = WiFi.scanNetworks(false, true);
  Serial.printf("WiFi scan found %d networks\n", found);
  for (int i = 0; i < found; ++i) {
    if (WiFi.SSID(i) != ssid) {
      continue;
    }
    uint8_t* bssid = WiFi.BSSID(i);
    const int32_t rssi = WiFi.RSSI(i);
    const int32_t channel = WiFi.channel(i);
    const wifi_auth_mode_t auth = WiFi.encryptionType(i);
    const bool usable = authMatchesMode(auth, mode);
    Serial.printf("  candidate %s ch=%d rssi=%d auth=%s usable=%d\n",
                  WiFi.BSSIDstr(i).c_str(), (int)channel, (int)rssi,
                  authName(auth), (int)usable);
    if (!usable || !bssid) {
      continue;
    }
    if (!best.found || rssi > best.rssi) {
      best.found = true;
      memcpy(best.bssid, bssid, sizeof(best.bssid));
      best.channel = (uint8_t)channel;
      best.rssi = rssi;
      best.auth = auth;
    }
  }
  WiFi.scanDelete();

  if (best.found) {
    Serial.printf("WiFi selected AP %02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d auth=%s\n",
                  best.bssid[0], best.bssid[1], best.bssid[2], best.bssid[3],
                  best.bssid[4], best.bssid[5], (unsigned)best.channel,
                  (int)best.rssi, authName(best.auth));
  } else {
    Serial.println("WiFi target AP not found; connecting without BSSID lock");
  }
  return best;
}

static bool setStaConfig(const char* ssid, const char* pass,
                         wifi_auth_mode_t threshold, const ApChoice* ap) {
  wifi_config_t conf;
  memset(&conf, 0, sizeof(conf));
  const size_t ssidLen = strlen(ssid);
  if (ssidLen == 0 || ssidLen > sizeof(conf.sta.ssid)) {
    Serial.printf("WiFi: invalid SSID length %u\n", (unsigned)ssidLen);
    return false;
  }
  memcpy(conf.sta.ssid, ssid, ssidLen);
  if (pass && pass[0]) {
    const size_t passLen = strlen(pass);
    if (passLen > sizeof(conf.sta.password)) {
      Serial.printf("WiFi: invalid password length %u\n", (unsigned)passLen);
      return false;
    }
    memcpy(conf.sta.password, pass, passLen);
  }
  conf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  if (ap && ap->found) {
    conf.sta.bssid_set = true;
    memcpy(conf.sta.bssid, ap->bssid, sizeof(conf.sta.bssid));
    conf.sta.channel = ap->channel;
    conf.sta.scan_method = WIFI_FAST_SCAN;
  }
  conf.sta.threshold.authmode = threshold;
  conf.sta.threshold.rssi = -95;
  conf.sta.pmf_cfg.capable = true;
  conf.sta.pmf_cfg.required = false;
  conf.sta.failure_retry_cnt = 2;

  const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &conf);
  logEspErr("esp_wifi_set_config", err);
  return err == ESP_OK;
}

static bool waitForIp(uint32_t timeoutMs, const char* label) {
  const uint32_t start = millis();
  uint8_t printedReason = 0;
  uint32_t nextDot = 0;

  while (millis() - start <= timeoutMs) {
    if (WiFi.status() == WL_CONNECTED && s_eventGotIp) {
      Serial.println();
      Serial.printf("WiFi OK (%s), IP=%s RSSI=%d\n", label,
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      return true;
    }

    const uint8_t reason = s_lastDisconnectReason;
    if (s_eventStaDisconnected && reason != 0 && reason != printedReason) {
      printedReason = reason;
      Serial.printf("\nWiFi %s disconnected: reason=%u (%s) rssi=%d\n", label,
                    (unsigned)reason,
                    WiFi.disconnectReasonName((wifi_err_reason_t)reason),
                    (int)s_lastDisconnectRssi);
    }

    if (millis() >= nextDot) {
      Serial.print('.');
      nextDot = millis() + 500;
    }
    delay(50);
  }

  Serial.println();
  Serial.printf("WiFi %s connect timeout status=%d lastReason=%u (%s)\n", label,
                (int)WiFi.status(), (unsigned)s_lastDisconnectReason,
                s_lastDisconnectReason
                    ? WiFi.disconnectReasonName(
                          (wifi_err_reason_t)s_lastDisconnectReason)
                    : "none");
  return false;
}

static bool connectWithAttempts(const char* label, uint8_t attempts,
                                uint32_t timeoutMs, uint32_t retryBaseMs,
                                bool (*beginAttempt)(void*), void* ctx) {
  for (uint8_t attempt = 1; attempt <= attempts; ++attempt) {
    Serial.printf("WiFi %s attempt %u/%u\n", label, (unsigned)attempt,
                  (unsigned)attempts);
    if (!prepareStaRadio()) {
      return false;
    }
    if (!beginAttempt(ctx)) {
      WiFi.mode(WIFI_OFF);
      delay(250);
      continue;
    }
    if (waitForIp(timeoutMs, label)) {
      return true;
    }
    WiFi.disconnect(false, false);
    delay(retryBaseMs + attempt * retryBaseMs);
  }
  WiFi.mode(WIFI_OFF);
  delay(100);
  return false;
}

struct PskContext {
  const AppConfig* cfg;
};

static bool beginPskAttempt(void* raw) {
  const PskContext* ctx = static_cast<const PskContext*>(raw);
  const AppConfig* cfg = ctx->cfg;
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
  const ApChoice ap = scanBestAp(cfg->ssid, APP_WIFI_PSK);
  if (!setStaConfig(cfg->ssid, cfg->pass, WIFI_AUTH_WPA_PSK, &ap)) {
    return false;
  }
  const esp_err_t err = esp_wifi_connect();
  logEspErr("esp_wifi_connect", err);
  return err == ESP_OK;
}

static bool wifiConnectPsk(AppConfig* cfg) {
  if (!cfg) {
    return false;
  }

  const size_t passLen = strnlen(cfg->pass, sizeof(cfg->pass));
  Serial.printf("WiFi PSK connecting to %s (passLen=%u)\n", cfg->ssid,
                (unsigned)passLen);
  if (passLen == 0) {
    Serial.println("WiFi PSK: password empty");
    return false;
  }

  PskContext ctx{cfg};
  if (connectWithAttempts("PSK", 2, WIFI_CONNECT_TIMEOUT_MS, 500,
                          beginPskAttempt, &ctx)) {
    return true;
  }

  if (strcmp(cfg->ssid, WIFI_SSID) == 0 && strcmp(cfg->pass, WIFI_PASS) != 0) {
    Serial.println("WiFi PSK: retry with config.h WIFI_PASS and repair NVS");
    char oldPass[sizeof(cfg->pass)];
    snprintf(oldPass, sizeof(oldPass), "%s", cfg->pass);
    snprintf(cfg->pass, sizeof(cfg->pass), "%s", WIFI_PASS);
    if (connectWithAttempts("PSK fallback", 1, WIFI_CONNECT_TIMEOUT_MS, 500,
                            beginPskAttempt, &ctx)) {
      if (appConfigSave(cfg)) {
        Serial.println("WiFi PSK: NVS password repaired");
      }
      return true;
    }
    snprintf(cfg->pass, sizeof(cfg->pass), "%s", oldPass);
  }
  return false;
}

struct OpenContext {
  const AppConfig* cfg;
};

static bool beginOpenAttempt(void* raw) {
  const OpenContext* ctx = static_cast<const OpenContext*>(raw);
  WiFi.setMinSecurity(WIFI_AUTH_OPEN);
  const ApChoice ap = scanBestAp(ctx->cfg->ssid, APP_WIFI_OPEN);
  if (!setStaConfig(ctx->cfg->ssid, nullptr, WIFI_AUTH_OPEN, &ap)) {
    return false;
  }
  const esp_err_t err = esp_wifi_connect();
  logEspErr("esp_wifi_connect", err);
  return err == ESP_OK;
}

static bool wifiConnectOpen(const AppConfig& cfg) {
  Serial.printf("WiFi OPEN connecting to %s (MAC whitelist)\n", cfg.ssid);
  OpenContext ctx{&cfg};
  return connectWithAttempts("OPEN", 2, WIFI_CONNECT_TIMEOUT_MS, 500,
                             beginOpenAttempt, &ctx);
}

struct PeapContext {
  const AppConfig* cfg;
};

static bool setPeapCredentials(const AppConfig& cfg) {
#if RADAR_HAS_WPA2_ENT == 0
  (void)cfg;
  Serial.println("WiFi PEAP: enterprise API not available in this core");
  return false;
#else
  const char* outer =
      cfg.outer_identity[0] ? cfg.outer_identity : cfg.identity;
  const size_t outerLen = strlen(outer);
  const size_t userLen = strlen(cfg.identity);
  const size_t passLen = strlen(cfg.pass);

#if RADAR_HAS_WPA2_ENT == 1
  logEspErr("wpa2 disable time check",
            esp_wifi_sta_wpa2_ent_set_disable_time_check(true));
  logEspErr("wpa2 set identity",
            esp_wifi_sta_wpa2_ent_set_identity(
                reinterpret_cast<const unsigned char*>(outer), outerLen));
  logEspErr("wpa2 set username",
            esp_wifi_sta_wpa2_ent_set_username(
                reinterpret_cast<const unsigned char*>(cfg.identity), userLen));
  logEspErr("wpa2 set password",
            esp_wifi_sta_wpa2_ent_set_password(
                reinterpret_cast<const unsigned char*>(cfg.pass), passLen));
  const esp_err_t err = esp_wifi_sta_wpa2_ent_enable();
  logEspErr("wpa2 enable", err);
  return err == ESP_OK;
#else
  logEspErr("eap disable time check", esp_eap_client_set_disable_time_check(true));
  logEspErr("eap methods", esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP));
  logEspErr("eap set identity",
            esp_eap_client_set_identity(
                reinterpret_cast<const uint8_t*>(outer), outerLen));
  logEspErr("eap set username",
            esp_eap_client_set_username(
                reinterpret_cast<const uint8_t*>(cfg.identity), userLen));
  logEspErr("eap set password",
            esp_eap_client_set_password(
                reinterpret_cast<const uint8_t*>(cfg.pass), passLen));
  const esp_err_t err = esp_wifi_sta_enterprise_enable();
  logEspErr("enterprise enable", err);
  return err == ESP_OK;
#endif
#endif
}

static bool beginPeapAttempt(void* raw) {
  const PeapContext* ctx = static_cast<const PeapContext*>(raw);
  const AppConfig* cfg = ctx->cfg;
  const char* outer =
      cfg->outer_identity[0] ? cfg->outer_identity : cfg->identity;

  WiFi.setMinSecurity(WIFI_AUTH_OPEN);
  const ApChoice ap = scanBestAp(cfg->ssid, APP_WIFI_PEAP);
  if (!setStaConfig(cfg->ssid, nullptr, WIFI_AUTH_OPEN, &ap)) {
    return false;
  }
  if (!setPeapCredentials(*cfg)) {
    return false;
  }

  Serial.printf("WiFi PEAP configured: ssid=%s outer=%s user=%s passLen=%u\n",
                cfg->ssid, outer, cfg->identity,
                (unsigned)strnlen(cfg->pass, sizeof(cfg->pass)));

  const esp_err_t err = esp_wifi_connect();
  logEspErr("esp_wifi_connect", err);
  return err == ESP_OK;
}

static bool wifiConnectPeap(const AppConfig& cfg) {
#if RADAR_HAS_WPA2_ENT == 0
  Serial.println("WiFi PEAP: enterprise API not available in this core");
  return false;
#else
  if (cfg.identity[0] == '\0') {
    Serial.println("WiFi PEAP: username empty");
    return false;
  }
  const size_t passLen = strnlen(cfg.pass, sizeof(cfg.pass));
  if (passLen == 0) {
    Serial.println("WiFi PEAP: password empty");
    return false;
  }

  PeapContext ctx{&cfg};
  return connectWithAttempts("PEAP", 4, 60000, 2000, beginPeapAttempt, &ctx);
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
  if (cfg->wifi_mode == APP_WIFI_OPEN) {
    return wifiConnectOpen(*cfg);
  }
  return wifiConnectPsk(cfg);
}
