#include "config_portal.h"

#include <WebServer.h>
#include <WiFi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button.h"
#include "config.h"
#include "setup_screen.h"
#include "wifi_sta.h"

static WebServer* s_server = nullptr;
static volatile bool s_saved = false;
static volatile bool s_webActive = false;
static uint32_t s_saveRedirectAt = 0;  // POST 成功后等待 /done；超时兜底关门户
static AppConfig s_formCfg;
static AppConfig s_seedCfg;

static void floatToBuf(float v, char* buf, size_t n) {
  if (!buf || n < 8) {
    return;
  }
  // Serial.printf 的 %f 已验证可用；width=0 的 dtostrf 在部分库会得到空串
  snprintf(buf, n, "%.5f", (double)v);
  // 去掉首尾空白
  char* p = buf;
  while (*p == ' ') {
    ++p;
  }
  if (p != buf) {
    memmove(buf, p, strlen(p) + 1);
  }
}

static void appendEscaped(String& out, const char* s) {
  if (!s) {
    return;
  }
  for (const char* p = s; *p; ++p) {
    if (*p == '&') {
      out += F("&amp;");
    } else if (*p == '"') {
      out += F("&quot;");
    } else if (*p == '<') {
      out += F("&lt;");
    } else if (*p == '>') {
      out += F("&gt;");
    } else {
      out += *p;
    }
  }
}

static bool isCaptiveProbe(const String& uri, const String& host) {
  if (uri.indexOf("generate_204") >= 0 || uri.indexOf("gen_204") >= 0) {
    return true;
  }
  if (uri.indexOf("hotspot-detect") >= 0 || uri.indexOf("connecttest") >= 0) {
    return true;
  }
  if (uri.indexOf("ncsi") >= 0 || uri.indexOf("success.txt") >= 0) {
    return true;
  }
  if (uri == "/fwlink" || uri.indexOf("canonical.html") >= 0) {
    return true;
  }
  if (host.length() && host.indexOf("192.168.4.1") < 0 &&
      host.indexOf("radar") < 0) {
    return true;
  }
  return false;
}

/** 禁止浏览器/强制门户缓存「已保存」页，避免下次打开仍显示完成态。 */
static void sendNoStoreHeaders() {
  if (!s_server) {
    return;
  }
  s_server->sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  s_server->sendHeader("Pragma", "no-cache");
  s_server->sendHeader("Expires", "0");
}

static void handleRoot() {
  if (!s_server) {
    return;
  }

  const String uri = s_server->uri();
  const String host = s_server->hostHeader();
  if (!isCaptiveProbe(uri, host)) {
    s_webActive = true;
    Serial.println("portal: setup page opened, timeout disabled");
  }

  // 输入框只填绝对值；正负由北纬/南纬、东经/西经决定
  char latBuf[24];
  char lonBuf[24];
  floatToBuf(fabsf(s_seedCfg.lat), latBuf, sizeof(latBuf));
  floatToBuf(fabsf(s_seedCfg.lon), lonBuf, sizeof(lonBuf));
  const bool peap = (s_seedCfg.wifi_mode == APP_WIFI_PEAP);
  const bool latSouth = s_seedCfg.lat < 0.0f;
  const bool lonWest = s_seedCfg.lon < 0.0f;

  String html;
  html.reserve(5200);
  html += F("<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<meta http-equiv=\"Cache-Control\" content=\"no-store\">"
            "<title>Radar Setup</title><style>"
            "body{font-family:sans-serif;max-width:420px;margin:12px auto;padding:0 12px;"
            "background:#111;color:#eee}h1{font-size:1.2rem}"
            "label{display:block;margin:10px 0 4px;font-size:.9rem}"
            "input,select{width:100%;box-sizing:border-box;padding:8px;border-radius:6px;"
            "border:1px solid #444;background:#222;color:#eee}"
            ".row{display:flex;gap:8px;align-items:stretch}"
            ".row select{width:7.2em;flex:0 0 auto}"
            ".row input{flex:1;min-width:0}"
            ".hint{color:#aaa;font-size:.8rem;margin:4px 0 12px}"
            "button{width:100%;padding:12px;margin-top:16px;border:0;border-radius:8px;"
            "background:#2a7;color:#fff;font-size:1rem}"
            "button.btn-geo{margin-top:10px;background:#444;font-size:.95rem}"
            "button.btn-geo:disabled{opacity:.6}"
            ".peap-only{display:none}</style></head><body>"
            "<h1>桌面雷达设置</h1>"
            "<p class=\"hint\">连热点 Radar-Setup 后填写。密码留空=不修改。"
            "坐标填绝对值，再用北纬/南纬、东经/西经；小数点如 23.1291</p>"
            "<form method=\"POST\" action=\"/save\" accept-charset=\"UTF-8\" "
            "autocomplete=\"off\">"
            "<label>WiFi 类型</label><select name=\"mode\" id=\"mode\" autocomplete=\"off\" "
            "onchange=\"tog()\">");
  html += peap ? F("<option value=\"0\">家用 WiFi (PSK)</option>"
                   "<option value=\"1\" selected>校园/企业 (PEAP)</option>")
               : F("<option value=\"0\" selected>家用 WiFi (PSK)</option>"
                   "<option value=\"1\">校园/企业 (PEAP)</option>");
  html += F("</select><label>SSID</label><input name=\"ssid\" required maxlength=\"32\" "
            "autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\" value=\"");
  appendEscaped(html, s_seedCfg.ssid);
  html += F("\"><div class=\"peap-only\" id=\"idRow\"><label>用户名</label>"
            "<input name=\"identity\" id=\"identity\" maxlength=\"63\" autocomplete=\"off\" "
            "autocapitalize=\"none\" spellcheck=\"false\" value=\"");
  appendEscaped(html, s_seedCfg.identity);
  // 密码不回填；留空则保留原密码。new-password 降低浏览器自动填充旧会话密码
  html += F("\"></div><label>Password（留空不改）</label>"
            "<input name=\"pass\" type=\"password\" maxlength=\"64\" value=\"\" "
            "autocomplete=\"new-password\">"
            "<label>纬度</label><div class=\"row\">"
            "<select name=\"lat_hem\" id=\"lat_hem\" autocomplete=\"off\">");
  html += latSouth ? F("<option value=\"N\">北纬</option>"
                       "<option value=\"S\" selected>南纬</option>")
                   : F("<option value=\"N\" selected>北纬</option>"
                       "<option value=\"S\">南纬</option>");
  html += F("</select><input name=\"lat\" id=\"lat\" inputmode=\"decimal\" required "
            "maxlength=\"16\" autocomplete=\"off\" placeholder=\"0~90\" value=\"");
  html += latBuf;
  html += F("\"></div><label>经度</label><div class=\"row\">"
            "<select name=\"lon_hem\" id=\"lon_hem\" autocomplete=\"off\">");
  html += lonWest ? F("<option value=\"E\">东经</option>"
                      "<option value=\"W\" selected>西经</option>")
                  : F("<option value=\"E\" selected>东经</option>"
                      "<option value=\"W\">西经</option>");
  html += F("</select><input name=\"lon\" id=\"lon\" inputmode=\"decimal\" required "
            "maxlength=\"16\" autocomplete=\"off\" placeholder=\"0~180\" value=\"");
  html += lonBuf;
  html += F("\"></div>"
            "<button type=\"button\" class=\"btn-geo\" id=\"geoBtn\" "
            "onclick=\"doGeo()\">获取当前位置</button>"
            "<p class=\"hint\" id=\"geoHint\"></p>"
            "<label>显示进度环</label><select name=\"show_ring\" "
            "autocomplete=\"off\">");
  html += s_seedCfg.show_progress
              ? F("<option value=\"1\" selected>显示</option>"
                  "<option value=\"0\">隐藏</option>")
              : F("<option value=\"1\">显示</option>"
                  "<option value=\"0\" selected>隐藏</option>");
  html += F("</select><button type=\"submit\">保存并继续</button></form>"
            "<script>"
            "function tog(){var p=document.getElementById('mode').value==='1';"
            "document.getElementById('idRow').style.display=p?'block':'none';"
            "document.getElementById('identity').required=p;}"
            "function geoFail(){var h=document.getElementById('geoHint');"
            "h.textContent='当前浏览器不支持，请手动填写或从地图复制';"
            "var b=document.getElementById('geoBtn');b.disabled=false;"
            "b.textContent='获取当前位置';}"
            "function doGeo(){"
            "var h=document.getElementById('geoHint');h.textContent='';"
            "if(!navigator.geolocation){geoFail();return;}"
            "var b=document.getElementById('geoBtn');b.disabled=true;"
            "b.textContent='定位中…';"
            "navigator.geolocation.getCurrentPosition(function(pos){"
            "var la=pos.coords.latitude,lo=pos.coords.longitude;"
            "document.getElementById('lat_hem').value=la<0?'S':'N';"
            "document.getElementById('lon_hem').value=lo<0?'W':'E';"
            "document.getElementById('lat').value=Math.abs(la).toFixed(5);"
            "document.getElementById('lon').value=Math.abs(lo).toFixed(5);"
            "h.textContent='已填入当前坐标';"
            "b.disabled=false;b.textContent='获取当前位置';"
            "},function(){geoFail();},"
            "{enableHighAccuracy:true,timeout:15000,maximumAge:0});}"
            "tog();</script>"
            "</body></html>");
  sendNoStoreHeaders();
  s_server->send(200, "text/html; charset=utf-8", html);
}

static const char kSavedHtml[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Cache-Control" content="no-store">
<title>Saved</title>
<style>body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:40px}</style>
</head><body><h1>已保存</h1><p>设备将关闭热点并继续运行…</p>
<p style="color:#888;font-size:.85rem">下次请打开 http://192.168.4.1/ （不要用 /done）</p>
</body></html>
)HTML";

static void handleDone() {
  if (!s_server) {
    return;
  }
  s_webActive = true;
  sendNoStoreHeaders();

  // 仅「刚 POST 保存成功」才出完成页；历史记录/强制门户再开 /done 时回表单，
  // 避免非无痕浏览器一连热点就跳到「已保存」并误关门户。
  if (s_saveRedirectAt == 0) {
    Serial.println("portal: /done without pending save -> /");
    s_server->sendHeader("Location", "/", true);
    s_server->send(302, "text/plain", "");
    return;
  }

  s_server->sendHeader("Clear-Site-Data", "\"cache\"");
  s_server->send_P(200, "text/html; charset=utf-8", kSavedHtml);
  // 浏览器跟完 PRG 拿到完成页后再关热点，避免历史记录卡在 POST
  s_saved = true;
  Serial.println("portal: /done served, closing soon");
}
static float parseCoord(String s, bool* ok) {
  s.trim();
  s.replace(',', '.');  // 部分手机用逗号作小数点
  if (s.length() == 0) {
    if (ok) {
      *ok = false;
    }
    return 0;
  }
  char* end = nullptr;
  const float v = strtof(s.c_str(), &end);
  if (!end || end == s.c_str() || *end != '\0') {
    if (ok) {
      *ok = false;
    }
    return 0;
  }
  if (ok) {
    *ok = true;
  }
  return v;
}

/** 返回 nullptr 成功，否则为错误说明。 */
static const char* parseForm(AppConfig* cfg) {
  if (!s_server || !cfg) {
    return "server";
  }
  memset(cfg, 0, sizeof(*cfg));

  Serial.printf("POST args=%d\n", s_server->args());
  for (int i = 0; i < s_server->args(); ++i) {
    const String name = s_server->argName(i);
    // 密码永不打明文；只记是否填写与长度
    if (name.equalsIgnoreCase("pass") || name.equalsIgnoreCase("password")) {
      const String v = s_server->arg(i);
      Serial.printf("  %s=[%s len=%u]\n", name.c_str(),
                    v.length() ? "***" : "(empty)", (unsigned)v.length());
    } else {
      Serial.printf("  %s=[%s]\n", name.c_str(), s_server->arg(i).c_str());
    }
  }

  const String modeStr = s_server->arg("mode");
  cfg->wifi_mode = (modeStr == "1") ? APP_WIFI_PEAP : APP_WIFI_PSK;

  String ssid = s_server->arg("ssid");
  ssid.trim();
  if (ssid.length() == 0) {
    return "SSID 为空";
  }
  if (ssid.length() > 32) {
    return "SSID 过长";
  }
  strncpy(cfg->ssid, ssid.c_str(), sizeof(cfg->ssid) - 1);

  String pass = s_server->arg("pass");
  if (pass.length() > 64) {
    return "密码过长";
  }
  if (pass.length() == 0) {
    // 留空：保留已有密码；若 NVS/默认也空则用 config.h（仅 PSK）
    strncpy(cfg->pass, s_seedCfg.pass, sizeof(cfg->pass) - 1);
    if (cfg->wifi_mode == APP_WIFI_PSK && cfg->pass[0] == '\0') {
      strncpy(cfg->pass, WIFI_PASS, sizeof(cfg->pass) - 1);
    }
  } else {
    strncpy(cfg->pass, pass.c_str(), sizeof(cfg->pass) - 1);
  }
  if (cfg->wifi_mode == APP_WIFI_PSK && cfg->pass[0] == '\0') {
    return "PSK 密码不能为空";
  }

  String id = s_server->arg("identity");
  id.trim();
  if (id.length() > 63) {
    return "Identity 过长";
  }
  strncpy(cfg->identity, id.c_str(), sizeof(cfg->identity) - 1);

  if (cfg->wifi_mode == APP_WIFI_PEAP && cfg->identity[0] == '\0') {
    return "PEAP 需要 Identity";
  }

  bool latOk = false;
  bool lonOk = false;
  float lat = parseCoord(s_server->arg("lat"), &latOk);
  float lon = parseCoord(s_server->arg("lon"), &lonOk);
  if (!latOk) {
    return "纬度格式错误(用小数点如23.1291)";
  }
  if (!lonOk) {
    return "经度格式错误(用小数点如113.2644)";
  }
  // 输入可带符号；最终符号以北纬/南纬、东经/西经为准
  lat = fabsf(lat);
  lon = fabsf(lon);
  if (lat > 90.0f) {
    return "纬度超出范围(0~90)";
  }
  if (lon > 180.0f) {
    return "经度超出范围(0~180)";
  }
  const String latHem = s_server->arg("lat_hem");
  const String lonHem = s_server->arg("lon_hem");
  if (latHem == "S") {
    lat = -lat;
  } else if (latHem != "N" && latHem.length() > 0) {
    return "纬度半球无效";
  }
  if (lonHem == "W") {
    lon = -lon;
  } else if (lonHem != "E" && lonHem.length() > 0) {
    return "经度半球无效";
  }
  // 缺省按北纬/东经（国内默认）
  cfg->lat = lat;
  cfg->lon = lon;
  cfg->show_progress = (s_server->arg("show_ring") != "0");
  return nullptr;
}

static void handleSave() {
  if (!s_server) {
    return;
  }
  s_webActive = true;
  AppConfig cfg;
  const char* err = parseForm(&cfg);
  if (err) {
    Serial.printf("form reject: %s\n", err);
    sendNoStoreHeaders();
    String msg = String("保存失败: ") + err +
                 "\n\n请返回重试。纬度/经度请用英文小数点。";
    s_server->send(400, "text/plain; charset=utf-8", msg);
    return;
  }
  if (!appConfigSave(&cfg)) {
    sendNoStoreHeaders();
    s_server->send(500, "text/plain; charset=utf-8", "NVS 保存失败");
    return;
  }
  s_formCfg = cfg;
  Serial.printf("config saved: mode=%u ssid=%s lat=%.4f lon=%.4f ring=%d\n",
                (unsigned)cfg.wifi_mode, cfg.ssid, cfg.lat, cfg.lon,
                (int)cfg.show_progress);
  // PRG：303 到 /done，避免刷新/历史记录重复 POST，也不把「已保存」绑在 POST 上缓存
  sendNoStoreHeaders();
  s_server->sendHeader("Location", "/done", true);
  s_server->send(303, "text/plain", "");
  s_saveRedirectAt = millis() == 0 ? 1 : millis();
}

static void handleNotFound() {
  if (!s_server) {
    return;
  }
  const String uri = s_server->uri();
  const String host = s_server->hostHeader();
  if (isCaptiveProbe(uri, host)) {
    sendNoStoreHeaders();
    // 带时间戳，降低系统强制门户缓存旧成功页
    char loc[48];
    snprintf(loc, sizeof(loc), "http://192.168.4.1/?t=%lu",
             (unsigned long)millis());
    String body = String("<!DOCTYPE html><html><head><meta charset=utf-8>"
                         "<meta http-equiv=refresh content='0;url=") +
                  loc + "'></head><body><p>Open <a href='" + loc + "'>" + loc +
                  "</a></p></body></html>";
    s_server->send(200, "text/html; charset=utf-8", body);
    return;
  }
  // 旧书签 /save、/done 等一律回表单
  sendNoStoreHeaders();
  s_server->sendHeader("Location", "/", true);
  s_server->send(302, "text/plain", "");
}

static void pumpServer(WebServer& server) {
  for (int i = 0; i < 16; ++i) {
    server.handleClient();
  }
}

PortalResult configPortalRun(LGFX* lcd, uint32_t timeoutMs, AppConfig* outCfg) {
  s_saved = false;
  s_webActive = false;
  s_saveRedirectAt = 0;
  memset(&s_formCfg, 0, sizeof(s_formCfg));
  if (!appConfigLoad(&s_seedCfg)) {
    appConfigSetDefaults(&s_seedCfg);
  }

  wifiDisconnectClean();
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP);
  delay(50);

  const IPAddress apIP(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gateway, subnet);
  const bool apOk = WiFi.softAP(SOFTAP_SSID, nullptr, 1, 0, 4);
  delay(150);

  const IPAddress ip = WiFi.softAPIP();
  Serial.printf("SoftAP %s ok=%d IP=%s\n", SOFTAP_SSID, (int)apOk,
                ip.toString().c_str());

  WebServer server(80);
  s_server = &server;
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/done", HTTP_GET, handleDone);
  // 误用 GET /save（历史/刷新）时回到表单，不要当成已保存
  server.on("/save", HTTP_GET, []() {
    if (!s_server) {
      return;
    }
    sendNoStoreHeaders();
    s_server->sendHeader("Location", "/", true);
    s_server->send(302, "text/plain", "");
  });
  server.onNotFound(handleNotFound);
  server.begin();

  if (lcd) {
    setupScreenDraw(lcd, (int)((timeoutMs + 999) / 1000));
  }

  const uint32_t start = millis();
  int lastRemain = -1;
  bool waitingDrawn = false;
  PortalResult result = PortalResult::TimedOut;

  while (true) {
    pumpServer(server);

    // POST 已成功但浏览器未拉 /done：约 1.5s 后仍关闭门户
    if (!s_saved && s_saveRedirectAt != 0 &&
        (millis() - s_saveRedirectAt) >= 1500) {
      Serial.println("portal: save redirect timeout, closing");
      s_saved = true;
    }

    if (s_saved) {
      result = PortalResult::Saved;
      // 冲刷响应，再安全拆除（避免 WiFiServer 析构时 pbuf_free 崩溃）
      for (int i = 0; i < 30; ++i) {
        server.handleClient();
        delay(20);
      }
      break;
    }

    const ButtonEvent ev = buttonPoll();
    if (ev == ButtonEvent::ShortPress) {
      Serial.println("portal skipped by BOOT");
      result = PortalResult::SkippedByButton;
      break;
    }

    const bool holdOpen =
        s_webActive || (WiFi.softAPgetStationNum() > 0);
    if (holdOpen) {
      if (lcd && !waitingDrawn && s_webActive) {
        waitingDrawn = true;
        setupScreenUpdateStatus(lcd, -1);
      }
      delay(2);
      continue;
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
      setupScreenUpdateStatus(lcd, remain);
    }

    delay(2);
  }

  // 先摘掉全局指针，再 close/stop，最后断 AP（顺序很重要）
  s_server = nullptr;
  server.close();
  delay(50);
  server.stop();
  delay(100);
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  if (outCfg) {
    if (result == PortalResult::Saved) {
      *outCfg = s_formCfg;
    } else if (!appConfigLoad(outCfg)) {
      appConfigSetDefaults(outCfg);
    }
  }

  return result;
}
