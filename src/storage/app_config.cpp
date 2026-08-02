#include "app_config.h"

#include <Preferences.h>
#include <string.h>

#include "config.h"

static constexpr const char* kNs = "radar";
static constexpr const char* kKeySaved = "saved";
static constexpr const char* kKeyMode = "mode";
static constexpr const char* kKeySsid = "ssid";
static constexpr const char* kKeyPass = "pass";
static constexpr const char* kKeyId = "id";
static constexpr const char* kKeyLat = "lat";
static constexpr const char* kKeyLon = "lon";
static constexpr const char* kKeyShowRing = "show_ring";

void appConfigSetDefaults(AppConfig* cfg) {
  if (!cfg) {
    return;
  }
  memset(cfg, 0, sizeof(*cfg));
  cfg->wifi_mode = APP_WIFI_PSK;
  strncpy(cfg->ssid, WIFI_SSID, sizeof(cfg->ssid) - 1);
  strncpy(cfg->pass, WIFI_PASS, sizeof(cfg->pass) - 1);
  cfg->identity[0] = '\0';
  cfg->lat = MAP_LAT;
  cfg->lon = MAP_LON;
  cfg->show_progress = true;
}

bool appConfigHasSaved() {
  Preferences prefs;
  if (!prefs.begin(kNs, true)) {
    return false;
  }
  const bool saved = prefs.getBool(kKeySaved, false);
  prefs.end();
  return saved;
}

bool appConfigLoad(AppConfig* cfg) {
  if (!cfg) {
    return false;
  }
  appConfigSetDefaults(cfg);

  Preferences prefs;
  if (!prefs.begin(kNs, true)) {
    return false;
  }
  if (!prefs.getBool(kKeySaved, false)) {
    prefs.end();
    return false;
  }

  cfg->wifi_mode =
      static_cast<AppWifiMode>(prefs.getUChar(kKeyMode, APP_WIFI_PSK));
  if (cfg->wifi_mode != APP_WIFI_PSK && cfg->wifi_mode != APP_WIFI_PEAP) {
    cfg->wifi_mode = APP_WIFI_PSK;
  }

  String ssid = prefs.getString(kKeySsid, cfg->ssid);
  String pass = prefs.getString(kKeyPass, cfg->pass);
  String id = prefs.getString(kKeyId, "");
  cfg->lat = prefs.getFloat(kKeyLat, MAP_LAT);
  cfg->lon = prefs.getFloat(kKeyLon, MAP_LON);
  cfg->show_progress = prefs.getBool(kKeyShowRing, true);
  prefs.end();

  strncpy(cfg->ssid, ssid.c_str(), sizeof(cfg->ssid) - 1);
  cfg->ssid[sizeof(cfg->ssid) - 1] = '\0';
  strncpy(cfg->pass, pass.c_str(), sizeof(cfg->pass) - 1);
  cfg->pass[sizeof(cfg->pass) - 1] = '\0';
  strncpy(cfg->identity, id.c_str(), sizeof(cfg->identity) - 1);
  cfg->identity[sizeof(cfg->identity) - 1] = '\0';

  if (cfg->ssid[0] == '\0') {
    appConfigSetDefaults(cfg);
    return false;
  }
  // 旧版表单曾把空密码写入 NVS；PSK 空密码时回退 config.h 默认
  if (cfg->wifi_mode == APP_WIFI_PSK && cfg->pass[0] == '\0') {
    Serial.println("appConfig: NVS PSK pass empty, fallback WIFI_PASS");
    strncpy(cfg->pass, WIFI_PASS, sizeof(cfg->pass) - 1);
    cfg->pass[sizeof(cfg->pass) - 1] = '\0';
  }
  return true;
}

bool appConfigSave(const AppConfig* cfg) {
  if (!cfg || cfg->ssid[0] == '\0') {
    return false;
  }
  Preferences prefs;
  if (!prefs.begin(kNs, false)) {
    return false;
  }
  prefs.putBool(kKeySaved, true);
  prefs.putUChar(kKeyMode, static_cast<uint8_t>(cfg->wifi_mode));
  prefs.putString(kKeySsid, cfg->ssid);
  prefs.putString(kKeyPass, cfg->pass);
  prefs.putString(kKeyId, cfg->identity);
  prefs.putFloat(kKeyLat, cfg->lat);
  prefs.putFloat(kKeyLon, cfg->lon);
  prefs.putBool(kKeyShowRing, cfg->show_progress);
  prefs.end();
  return true;
}
