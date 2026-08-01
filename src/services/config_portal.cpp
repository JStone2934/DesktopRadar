#include "config_portal.h"

#include <WebServer.h>
#include <WiFi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button.h"
#include "config.h"
#include "setup_screen.h"
#include "wifi_sta.h"

static WebServer* s_server = nullptr;
static volatile bool s_saved = false;
static AppConfig s_formCfg;
static AppConfig s_seedCfg;

static void handleRoot() {
  if (!s_server) {
    return;
  }
  char latBuf[24];
  char lonBuf[24];
  snprintf(latBuf, sizeof(latBuf), "%.5f", s_seedCfg.lat);
  snprintf(lonBuf, sizeof(lonBuf), "%.5f", s_seedCfg.lon);
  const bool peap = (s_seedCfg.wifi_mode == APP_WIFI_PEAP);

  String html;
  html.reserve(2200);
  html += F("<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Radar Setup</title><style>"
            "body{font-family:sans-serif;max-width:420px;margin:12px auto;padding:0 12px;"
            "background:#111;color:#eee}h1{font-size:1.2rem}"
            "label{display:block;margin:10px 0 4px;font-size:.9rem}"
            "input,select{width:100%;box-sizing:border-box;padding:8px;border-radius:6px;"
            "border:1px solid #444;background:#222;color:#eee}"
            ".hint{color:#aaa;font-size:.8rem;margin:4px 0 12px}"
            "button{width:100%;padding:12px;margin-top:16px;border:0;border-radius:8px;"
            "background:#2a7;color:#fff;font-size:1rem}"
            ".peap-only{display:none}</style></head><body>"
            "<h1>桌面雷达设置</h1>"
            "<p class=\"hint\">连上热点后在此保存；缩放/刷新仍用固件默认。</p>"
            "<form method=\"POST\" action=\"/save\">"
            "<label>WiFi 类型</label><select name=\"mode\" id=\"mode\" onchange=\"tog()\">");
  html += peap ? F("<option value=\"0\">家用 WiFi (PSK)</option>"
                   "<option value=\"1\" selected>校园/企业 (PEAP)</option>")
               : F("<option value=\"0\" selected>家用 WiFi (PSK)</option>"
                   "<option value=\"1\">校园/企业 (PEAP)</option>");
  html += F("</select><label>SSID</label><input name=\"ssid\" required maxlength=\"32\" value=\"");
  html += s_seedCfg.ssid;
  html += F("\"><div class=\"peap-only\" id=\"idRow\"><label>Identity（学号/工号）</label>"
            "<input name=\"identity\" id=\"identity\" maxlength=\"63\" value=\"");
  html += s_seedCfg.identity;
  html += F("\"></div><label>Password</label>"
            "<input name=\"pass\" type=\"password\" maxlength=\"64\" value=\"");
  html += s_seedCfg.pass;
  html += F("\"><label>纬度</label><input name=\"lat\" type=\"number\" step=\"any\" required value=\"");
  html += latBuf;
  html += F("\"><label>经度</label><input name=\"lon\" type=\"number\" step=\"any\" required value=\"");
  html += lonBuf;
  html += F("\"><button type=\"submit\">保存并继续</button></form>"
            "<script>function tog(){var p=document.getElementById('mode').value==='1';"
            "document.getElementById('idRow').style.display=p?'block':'none';"
            "document.getElementById('identity').required=p;}tog();</script>"
            "</body></html>");
  s_server->send(200, "text/html", html);
}

static const char kSavedHtml[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title>
<style>body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:40px}</style>
</head><body><h1>已保存</h1><p>设备将关闭热点并继续运行…</p></body></html>
)HTML";

static bool parseForm(AppConfig* cfg) {
  if (!s_server || !cfg) {
    return false;
  }
  memset(cfg, 0, sizeof(*cfg));

  const String modeStr = s_server->arg("mode");
  cfg->wifi_mode =
      (modeStr == "1") ? APP_WIFI_PEAP : APP_WIFI_PSK;

  String ssid = s_server->arg("ssid");
  ssid.trim();
  if (ssid.length() == 0 || ssid.length() > 32) {
    return false;
  }
  strncpy(cfg->ssid, ssid.c_str(), sizeof(cfg->ssid) - 1);

  String pass = s_server->arg("pass");
  if (pass.length() > 64) {
    return false;
  }
  strncpy(cfg->pass, pass.c_str(), sizeof(cfg->pass) - 1);

  String id = s_server->arg("identity");
  id.trim();
  if (id.length() > 63) {
    return false;
  }
  strncpy(cfg->identity, id.c_str(), sizeof(cfg->identity) - 1);

  if (cfg->wifi_mode == APP_WIFI_PEAP && cfg->identity[0] == '\0') {
    return false;
  }

  const float lat = s_server->arg("lat").toFloat();
  const float lon = s_server->arg("lon").toFloat();
  if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
    return false;
  }
  // reject clearly empty number fields (toFloat -> 0)
  if (!s_server->hasArg("lat") || !s_server->hasArg("lon")) {
    return false;
  }
  cfg->lat = lat;
  cfg->lon = lon;
  return true;
}

static void handleSave() {
  if (!s_server) {
    return;
  }
  AppConfig cfg;
  if (!parseForm(&cfg)) {
    s_server->send(400, "text/plain", "Invalid form");
    return;
  }
  if (!appConfigSave(&cfg)) {
    s_server->send(500, "text/plain", "NVS save failed");
    return;
  }
  s_formCfg = cfg;
  s_saved = true;
  s_server->send_P(200, "text/html", kSavedHtml);
  Serial.printf("config saved: mode=%u ssid=%s lat=%.4f lon=%.4f\n",
                (unsigned)cfg.wifi_mode, cfg.ssid, cfg.lat, cfg.lon);
}

static void handleNotFound() {
  if (!s_server) {
    return;
  }
  // 轻量：未知路径回首页，便于部分手机探测
  s_server->sendHeader("Location", "/", true);
  s_server->send(302, "text/plain", "");
}

PortalResult configPortalRun(LGFX* lcd, uint32_t timeoutMs, AppConfig* outCfg) {
  s_saved = false;
  memset(&s_formCfg, 0, sizeof(s_formCfg));
  if (!appConfigLoad(&s_seedCfg)) {
    appConfigSetDefaults(&s_seedCfg);
  }

  wifiDisconnectClean();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SOFTAP_SSID);
  delay(100);

  const IPAddress ip = WiFi.softAPIP();
  Serial.printf("SoftAP %s IP=%s\n", SOFTAP_SSID, ip.toString().c_str());

  WebServer server(80);
  s_server = &server;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  const uint32_t start = millis();
  int lastRemain = -1;
  PortalResult result = PortalResult::TimedOut;

  while (true) {
    server.handleClient();

    if (s_saved) {
      result = PortalResult::Saved;
      delay(400);  // let browser receive response
      break;
    }

    const ButtonEvent ev = buttonPoll();
    if (ev == ButtonEvent::ShortPress) {
      Serial.println("portal skipped by BOOT");
      result = PortalResult::SkippedByButton;
      break;
    }

    const uint32_t elapsed = millis() - start;
    if (elapsed >= timeoutMs) {
      Serial.println("portal timeout");
      result = PortalResult::TimedOut;
      break;
    }

    const int remain = (int)((timeoutMs - elapsed + 999) / 1000);
    if (lcd && remain != lastRemain) {
      lastRemain = remain;
      setupScreenDraw(lcd, remain);
    }

    delay(2);
  }

  server.stop();
  s_server = nullptr;
  WiFi.softAPdisconnect(true);
  delay(50);
  WiFi.mode(WIFI_OFF);
  delay(50);

  if (outCfg) {
    if (result == PortalResult::Saved) {
      *outCfg = s_formCfg;
    } else if (!appConfigLoad(outCfg)) {
      appConfigSetDefaults(outCfg);
    }
  }

  return result;
}
