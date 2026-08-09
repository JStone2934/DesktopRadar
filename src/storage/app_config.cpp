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
static constexpr const char* kKeyOuterId = "outer_id";
static constexpr const char* kKeyLat = "lat";
static constexpr const char* kKeyLon = "lon";
static constexpr const char* kKeyShowRing = "show_ring";
static constexpr const char* kKeyAlertRing = "alert_ring";
static constexpr const char* kKeyCrosshair = "crosshair";
static constexpr const char* kKeyDefaultZoom = "def_zoom";

static int clampZoom(int zoom) {
  if (zoom < ZOOM_MIN) {
    return ZOOM_MIN;
  }
  if (zoom > ZOOM_MAX) {
    return ZOOM_MAX;
  }
  if (zoom == ZOOM_SKIP) {
    return MAP_ZOOM;
  }
  return zoom;
}

uint16_t appConfigSecretSig(const char* s) {
  uint16_t sig = 0x4d3b;
  if (!s) {
    return sig;
  }
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(s); *p; ++p) {
    sig = (uint16_t)((sig << 5) | (sig >> 11));
    sig = (uint16_t)(sig ^ *p);
    sig = (uint16_t)(sig + 0x27d4);
  }
  return sig;
}

void appConfigSetDefaults(AppConfig* cfg) {
  if (!cfg) {
    return;
  }
  memset(cfg, 0, sizeof(*cfg));
  cfg->wifi_mode = APP_WIFI_PSK;
  strncpy(cfg->ssid, WIFI_SSID, sizeof(cfg->ssid) - 1);
  strncpy(cfg->pass, WIFI_PASS, sizeof(cfg->pass) - 1);
  cfg->identity[0] = '\0';
  cfg->outer_identity[0] = '\0';
  cfg->lat = MAP_LAT;
  cfg->lon = MAP_LON;
  cfg->show_progress = true;
  cfg->show_alert_ring = false;
  cfg->show_crosshair = true;
  cfg->default_zoom = clampZoom(MAP_ZOOM);
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
  if (cfg->wifi_mode != APP_WIFI_PSK &&
      cfg->wifi_mode != APP_WIFI_PEAP &&
      cfg->wifi_mode != APP_WIFI_OPEN) {
    cfg->wifi_mode = APP_WIFI_PSK;
  }

  String ssid = prefs.getString(kKeySsid, cfg->ssid);
  String pass = prefs.getString(kKeyPass, cfg->pass);
  String id = prefs.getString(kKeyId, "");
  String outerId = prefs.getString(kKeyOuterId, "");
  cfg->lat = prefs.getFloat(kKeyLat, MAP_LAT);
  cfg->lon = prefs.getFloat(kKeyLon, MAP_LON);
  cfg->show_progress = prefs.getBool(kKeyShowRing, true);
  cfg->show_alert_ring = prefs.getBool(kKeyAlertRing, false);
  cfg->show_crosshair = prefs.getBool(kKeyCrosshair, true);
  cfg->default_zoom = clampZoom(prefs.getInt(kKeyDefaultZoom, MAP_ZOOM));
  prefs.end();

  strncpy(cfg->ssid, ssid.c_str(), sizeof(cfg->ssid) - 1);
  cfg->ssid[sizeof(cfg->ssid) - 1] = '\0';
  strncpy(cfg->pass, pass.c_str(), sizeof(cfg->pass) - 1);
  cfg->pass[sizeof(cfg->pass) - 1] = '\0';
  strncpy(cfg->identity, id.c_str(), sizeof(cfg->identity) - 1);
  cfg->identity[sizeof(cfg->identity) - 1] = '\0';
  strncpy(cfg->outer_identity, outerId.c_str(), sizeof(cfg->outer_identity) - 1);
  cfg->outer_identity[sizeof(cfg->outer_identity) - 1] = '\0';
  Serial.printf("appConfig load: mode=%u ssid=%s passLen=%u passSig=%04x id=%s outer=%s cross=%d defZoom=%d\n",
                (unsigned)cfg->wifi_mode, cfg->ssid,
                (unsigned)strnlen(cfg->pass, sizeof(cfg->pass)),
                (unsigned)appConfigSecretSig(cfg->pass), cfg->identity,
                cfg->outer_identity, (int)cfg->show_crosshair,
                cfg->default_zoom);

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
  prefs.putString(kKeyOuterId, cfg->outer_identity);
  prefs.putFloat(kKeyLat, cfg->lat);
  prefs.putFloat(kKeyLon, cfg->lon);
  prefs.putBool(kKeyShowRing, cfg->show_progress);
  prefs.putBool(kKeyAlertRing, cfg->show_alert_ring);
  prefs.putBool(kKeyCrosshair, cfg->show_crosshair);
  prefs.putInt(kKeyDefaultZoom, clampZoom(cfg->default_zoom));
  prefs.end();
  Serial.printf("appConfig save: mode=%u ssid=%s passLen=%u passSig=%04x id=%s outer=%s cross=%d defZoom=%d\n",
                (unsigned)cfg->wifi_mode, cfg->ssid,
                (unsigned)strnlen(cfg->pass, sizeof(cfg->pass)),
                (unsigned)appConfigSecretSig(cfg->pass), cfg->identity,
                cfg->outer_identity, (int)cfg->show_crosshair,
                clampZoom(cfg->default_zoom));
  return true;
}
