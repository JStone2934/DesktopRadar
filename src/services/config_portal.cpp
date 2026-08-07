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
static LGFX* s_portalLcd = nullptr;
static volatile bool s_saved = false;
static volatile bool s_webActive = false;
static uint32_t s_saveRedirectAt = 0;  // POST 成功后等待 /done；超时兜底关门户
static AppConfig s_formCfg;
static AppConfig s_seedCfg;

struct PortalNetwork {
  String ssid;
  int32_t rssi;
  wifi_auth_mode_t auth;
  AppWifiMode mode;
};

static PortalNetwork s_portalNetworks[24];
static int s_portalNetworkCount = 0;

static AppWifiMode portalModeForAuth(wifi_auth_mode_t auth) {
  if (auth == WIFI_AUTH_OPEN) {
    return APP_WIFI_OPEN;
  }
  if (auth == WIFI_AUTH_WPA2_ENTERPRISE || auth == WIFI_AUTH_WPA3_ENT_192) {
    return APP_WIFI_PEAP;
  }
  return APP_WIFI_PSK;
}

static int portalModeRank(AppWifiMode mode) {
  if (mode == APP_WIFI_PEAP) {
    return 3;
  }
  if (mode == APP_WIFI_PSK) {
    return 2;
  }
  if (mode == APP_WIFI_OPEN) {
    return 1;
  }
  return 0;
}

static const char* portalAuthLabel(AppWifiMode mode) {
  if (mode == APP_WIFI_OPEN) {
    return "开放";
  }
  if (mode == APP_WIFI_PEAP) {
    return "企业";
  }
  return "加密";
}

static void scanPortalNetworks() {
  s_portalNetworkCount = 0;
  for (PortalNetwork& network : s_portalNetworks) {
    network.ssid = "";
  }

  Serial.println("portal: scanning nearby WiFi networks ...");
  const int found = WiFi.scanNetworks(false, true);
  for (int i = 0; i < found; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0 || ssid == SOFTAP_SSID) {
      continue;
    }
    const int32_t rssi = WiFi.RSSI(i);
    const wifi_auth_mode_t auth = WiFi.encryptionType(i);
    int existing = -1;
    for (int j = 0; j < s_portalNetworkCount; ++j) {
      if (s_portalNetworks[j].ssid == ssid) {
        existing = j;
        break;
      }
    }
    if (existing >= 0) {
      if (rssi > s_portalNetworks[existing].rssi) {
        s_portalNetworks[existing].rssi = rssi;
        s_portalNetworks[existing].auth = auth;
      }
      const AppWifiMode candidateMode = portalModeForAuth(auth);
      if (portalModeRank(candidateMode) >
          portalModeRank(s_portalNetworks[existing].mode)) {
        s_portalNetworks[existing].mode = candidateMode;
      }
      continue;
    }
    if (s_portalNetworkCount >=
        (int)(sizeof(s_portalNetworks) / sizeof(s_portalNetworks[0]))) {
      continue;
    }
    PortalNetwork& network = s_portalNetworks[s_portalNetworkCount++];
    network.ssid = ssid;
    network.rssi = rssi;
    network.auth = auth;
    network.mode = portalModeForAuth(auth);
  }
  WiFi.scanDelete();

  bool seedFound = false;
  for (int i = 0; i < s_portalNetworkCount; ++i) {
    if (s_portalNetworks[i].ssid == s_seedCfg.ssid) {
      seedFound = true;
      // 对已保存的 SSID，保留用户上一次确认过的认证类型。校园网同一
      // SSID 往往有多个 BSSID，扫描结果里的 auth 不应覆盖已知配置。
      s_portalNetworks[i].mode = s_seedCfg.wifi_mode;
    }
  }
  if (!seedFound && s_seedCfg.ssid[0] != '\0' &&
      s_portalNetworkCount <
          (int)(sizeof(s_portalNetworks) / sizeof(s_portalNetworks[0]))) {
    PortalNetwork& saved = s_portalNetworks[s_portalNetworkCount++];
    saved.ssid = s_seedCfg.ssid;
    saved.rssi = -127;
    saved.auth = WIFI_AUTH_MAX;
    saved.mode = s_seedCfg.wifi_mode;
  }

  for (int i = 0; i < s_portalNetworkCount - 1; ++i) {
    for (int j = i + 1; j < s_portalNetworkCount; ++j) {
      if (s_portalNetworks[j].rssi > s_portalNetworks[i].rssi) {
        const PortalNetwork tmp = s_portalNetworks[i];
        s_portalNetworks[i] = s_portalNetworks[j];
        s_portalNetworks[j] = tmp;
      }
    }
  }
  Serial.printf("portal: WiFi choices=%d\n", s_portalNetworkCount);
}

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

/** 禁止浏览器缓存「已保存」页，避免下次打开仍显示完成态。 */
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

  s_webActive = true;
  Serial.println("portal: setup page opened manually, timeout disabled");

  // 输入框只填绝对值；正负由北纬/南纬、东经/西经决定
  char latBuf[24];
  char lonBuf[24];
  floatToBuf(fabsf(s_seedCfg.lat), latBuf, sizeof(latBuf));
  floatToBuf(fabsf(s_seedCfg.lon), lonBuf, sizeof(lonBuf));
  const bool latSouth = s_seedCfg.lat < 0.0f;
  const bool lonWest = s_seedCfg.lon < 0.0f;

  String html;
  html.reserve(11000);
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
            ".check{display:flex;gap:8px;align-items:center;margin:8px 0 0;color:#aaa;font-size:.85rem}"
            ".check input{width:auto}"
            ".peap-only{display:none}</style></head><body>"
            "<h1>桌面雷达设置</h1>"
            "<p class=\"hint\">扫描列表只负责填入 SSID；认证类型请手动确认。已保存过的同一网络密码可留空复用。"
            "坐标填绝对值，再用北纬/南纬、东经/西经；小数点如 23.1291</p>"
            "<form method=\"POST\" action=\"/save\" accept-charset=\"UTF-8\" "
            "autocomplete=\"off\">"
            "<label>扫描结果</label><select id=\"scan\" autocomplete=\"off\" "
            "onchange=\"pickScan()\"><option value=\"\">手动输入 / 保持下方 SSID</option>");
  for (int i = 0; i < s_portalNetworkCount; ++i) {
    const PortalNetwork& network = s_portalNetworks[i];
    html += F("<option value=\"");
    appendEscaped(html, network.ssid.c_str());
    html += F("\" data-mode=\"");
    html += String((unsigned)network.mode);
    html += F("\"");
    html += F(">");
    appendEscaped(html, network.ssid.c_str());
    html += F(" · ");
    html += portalAuthLabel(network.mode);
    if (network.rssi > -127) {
      html += F(" · ");
      html += String(network.rssi);
      html += F(" dBm");
    } else {
      html += F(" · 已保存/当前未发现");
    }
    html += F("</option>");
  }
  html += F("</select><p class=\"hint\">列表按信号强度排列；如果学校 WiFi 是 PEAP，请在下面手动选 PEAP。</p>"
            "<label>SSID</label><input name=\"ssid\" id=\"ssid\" required maxlength=\"32\" "
            "autocomplete=\"off\" autocapitalize=\"none\" autocorrect=\"off\" "
            "spellcheck=\"false\" value=\"");
  appendEscaped(html, s_seedCfg.ssid);
  html += F("\"><label>认证类型</label><select name=\"mode\" id=\"mode\" "
            "autocomplete=\"off\" onchange=\"netChanged()\">");
  html += s_seedCfg.wifi_mode == APP_WIFI_PSK
              ? F("<option value=\"0\" selected>普通密码 WiFi (WPA/WPA2/WPA3)</option>")
              : F("<option value=\"0\">普通密码 WiFi (WPA/WPA2/WPA3)</option>");
  html += s_seedCfg.wifi_mode == APP_WIFI_PEAP
              ? F("<option value=\"1\" selected>企业 WiFi (PEAP/MSCHAPv2)</option>")
              : F("<option value=\"1\">企业 WiFi (PEAP/MSCHAPv2)</option>");
  html += s_seedCfg.wifi_mode == APP_WIFI_OPEN
              ? F("<option value=\"2\" selected>开放网络 / MAC 白名单</option>")
              : F("<option value=\"2\">开放网络 / MAC 白名单</option>");
  html += F("</select>"
            "<div class=\"peap-only\" id=\"idRow\"><label>PEAP 用户名</label>"
            "<input name=\"identity\" id=\"identity\" maxlength=\"63\" autocomplete=\"off\" "
            "autocapitalize=\"none\" spellcheck=\"false\" value=\"");
  appendEscaped(html, s_seedCfg.identity);
  html += F("\"><label>外层 Identity（可选）</label>"
            "<input name=\"outer_identity\" id=\"outer_identity\" maxlength=\"63\" "
            "autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\" "
            "placeholder=\"留空则使用 PEAP 用户名\" value=\"");
  appendEscaped(html, s_seedCfg.outer_identity);
  // 密码不回填；留空则保留原密码。new-password 降低浏览器自动填充旧会话密码
  html += F("\"></div><label id=\"passLabel\">Password（已保存可留空）</label>"
            "<input name=\"pass\" id=\"pass\" type=\"password\" maxlength=\"64\" "
            "value=\"\" autocomplete=\"new-password\" autocapitalize=\"none\" "
            "autocorrect=\"off\" spellcheck=\"false\">"
            "<label class=\"check\"><input type=\"checkbox\" onclick=\""
            "document.getElementById('pass').type=this.checked?'text':'password'\">"
            "显示密码</label>"
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
            "<label>显示进度条</label><select name=\"show_ring\" "
            "autocomplete=\"off\">");
  html += s_seedCfg.show_progress
              ? F("<option value=\"1\" selected>显示</option>"
                  "<option value=\"0\">隐藏</option>")
              : F("<option value=\"1\">显示</option>"
                  "<option value=\"0\" selected>隐藏</option>");
  html += F("</select><label>天气预警环</label><select name=\"show_alert\" "
            "autocomplete=\"off\">");
  html += s_seedCfg.show_alert_ring
              ? F("<option value=\"1\" selected>开启</option>"
                  "<option value=\"0\">关闭</option>")
              : F("<option value=\"1\">开启</option>"
                  "<option value=\"0\" selected>关闭</option>");
  html += F("</select><p class=\"hint\">开启后：中心有云图时屏缘显示对应颜色圆环</p>"
            "<button type=\"submit\">保存并继续</button></form>"
            "<script>"
            "function pickScan(){var s=document.getElementById('scan'),x=s.options[s.selectedIndex];"
            "if(!x||!x.value)return;document.getElementById('ssid').value=x.value;"
            "if(x.dataset.mode)document.getElementById('mode').value=x.dataset.mode;netChanged();}"
            "function netChanged(){var m=document.getElementById('mode').value;"
            "var p=m==='1',o=m==='2';"
            "document.getElementById('idRow').style.display=p?'block':'none';"
            "document.getElementById('identity').required=p;"
            "document.getElementById('outer_identity').disabled=!p;"
            "document.getElementById('pass').disabled=o;"
            "document.getElementById('passLabel').textContent=o?'Password（开放网络无需填写）':'Password（已保存可留空）';}"
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
            "netChanged();</script>"
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
      Serial.printf("  %s=[%s len=%u sig=%04x]\n", name.c_str(),
                    v.length() ? "***" : "(empty)", (unsigned)v.length(),
                    (unsigned)appConfigSecretSig(v.c_str()));
    } else {
      Serial.printf("  %s=[%s]\n", name.c_str(), s_server->arg(i).c_str());
    }
  }

  const String modeStr = s_server->arg("mode");
  if (modeStr == "1") {
    cfg->wifi_mode = APP_WIFI_PEAP;
  } else if (modeStr == "2") {
    cfg->wifi_mode = APP_WIFI_OPEN;
  } else {
    cfg->wifi_mode = APP_WIFI_PSK;
  }

  String ssid = s_server->arg("ssid");
  ssid.trim();
  if (ssid.length() == 0) {
    return "SSID 为空";
  }
  if (ssid.length() > 32) {
    return "SSID 过长";
  }
  strncpy(cfg->ssid, ssid.c_str(), sizeof(cfg->ssid) - 1);

  String id = s_server->arg("identity");
  id.trim();
  if (id.length() > 63) {
    return "PEAP 用户名过长";
  }
  strncpy(cfg->identity, id.c_str(), sizeof(cfg->identity) - 1);

  String outerId = s_server->arg("outer_identity");
  outerId.trim();
  if (outerId.length() > 63) {
    return "外层 Identity 过长";
  }
  strncpy(cfg->outer_identity, outerId.c_str(),
          sizeof(cfg->outer_identity) - 1);

  if (cfg->wifi_mode != APP_WIFI_PEAP) {
    cfg->identity[0] = '\0';
    cfg->outer_identity[0] = '\0';
  }

  if (cfg->wifi_mode == APP_WIFI_PEAP && cfg->identity[0] == '\0') {
    return "PEAP 需要用户名";
  }

  const bool sameSavedNetwork =
      cfg->wifi_mode == s_seedCfg.wifi_mode && ssid == s_seedCfg.ssid;
  const bool sameSavedSecret =
      sameSavedNetwork &&
      (cfg->wifi_mode != APP_WIFI_PEAP ||
       strcmp(cfg->identity, s_seedCfg.identity) == 0);

  String pass = s_server->arg("pass");
  if (pass.length() > 64) {
    return "密码过长";
  }
  if (cfg->wifi_mode == APP_WIFI_OPEN) {
    cfg->pass[0] = '\0';
  } else if (pass.length() == 0) {
    // 只允许同一已保存网络复用密码，避免把旧网络密码误用于新 SSID。
    if (!sameSavedSecret) {
      return "首次连接该网络需要密码";
    }
    strncpy(cfg->pass, s_seedCfg.pass, sizeof(cfg->pass) - 1);
  } else {
    strncpy(cfg->pass, pass.c_str(), sizeof(cfg->pass) - 1);
  }
  Serial.printf("form password selected: len=%u sig=%04x reused=%d\n",
                (unsigned)strnlen(cfg->pass, sizeof(cfg->pass)),
                (unsigned)appConfigSecretSig(cfg->pass),
                (int)(pass.length() == 0 && sameSavedNetwork &&
                      cfg->wifi_mode != APP_WIFI_OPEN));
  if (cfg->wifi_mode == APP_WIFI_PSK && cfg->pass[0] == '\0') {
    return "PSK 密码不能为空";
  }
  if (cfg->wifi_mode == APP_WIFI_PEAP && cfg->pass[0] == '\0') {
    return "PEAP 密码不能为空";
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
  cfg->show_alert_ring = (s_server->arg("show_alert") == "1");
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
  Serial.printf("config saved: mode=%u ssid=%s lat=%.4f lon=%.4f ring=%d alert=%d\n",
                (unsigned)cfg.wifi_mode, cfg.ssid, cfg.lat, cfg.lon,
                (int)cfg.show_progress, (int)cfg.show_alert_ring);
  // PRG：303 到 /done，避免刷新/历史记录重复 POST，也不把「已保存」绑在 POST 上缓存
  sendNoStoreHeaders();
  s_server->sendHeader("Location", "/done", true);
  s_server->send(303, "text/plain", "");
  s_saveRedirectAt = millis() == 0 ? 1 : millis();
  setupScreenShowSaved(s_portalLcd, cfg.ssid);
}

static void handleNotFound() {
  if (!s_server) {
    return;
  }
  // 旧书签 /save、/done 等一律回表单
  sendNoStoreHeaders();
  s_server->sendHeader("Location", "/", true);
  s_server->send(302, "text/plain", "");
}

PortalResult configPortalRun(LGFX* lcd, uint32_t timeoutMs, AppConfig* outCfg) {
  s_portalLcd = lcd;
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

  // scanNetworks() 会打开 STA。扫描必须发生在 SoftAP 启动前，避免设置
  // 热点变成 AP+STA 混合状态，保存后再切企业 WiFi 时继承不干净的射频状态。
  WiFi.mode(WIFI_STA);
  delay(100);
  scanPortalNetworks();
  WiFi.mode(WIFI_OFF);
  delay(200);

  // 使用 WPA2 配置热点，保留手机端稳定拿 IP 的改动；但这里明确只进入
  // AP 模式。保存后会完整关闭 AP，再由 wifi_sta.cpp 单独启动 STA/PEAP。
  WiFi.mode(WIFI_AP);
  delay(50);
  const IPAddress apIP(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gateway, subnet);
  const bool apOk = WiFi.softAP(SOFTAP_SSID, SOFTAP_PASS, 6, 0, 4);
  WiFi.setSleep(false);
  delay(250);

  const IPAddress ip = WiFi.softAPIP();
  const bool modeOk = (WiFi.getMode() & WIFI_AP) != 0;
  const bool configOk = ip == apIP;
  Serial.printf("SoftAP %s WPA2 channel=6 mode=%d config=%d ap=%d IP=%s\n",
                SOFTAP_SSID, (int)modeOk, (int)configOk, (int)apOk,
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

  // 不启动通配 DNS，也不劫持系统联网探测。手机只连接热点，用户按屏幕
  // 提示手动打开固定地址；这样不会触发缓慢的“登录网络”自动弹窗。
  Serial.printf("portal manual URL only: %s\n", CONFIG_PORTAL_URL);

  if (lcd) {
    setupScreenDraw(lcd, (int)((timeoutMs + 999) / 1000));
  }

  const uint32_t start = millis();
  int lastRemain = -1;
  bool waitingDrawn = false;
  PortalResult result = PortalResult::TimedOut;

  while (true) {
    server.handleClient();

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

    // 用户手动打开页面后不再自动关闭；页面未打开时，手机仍连接热点也
    // 暂停倒计时，避免刚准备输入时被 60 秒超时打断。
    const bool holdOpen = s_webActive || (WiFi.softAPgetStationNum() > 0);
    if (holdOpen) {
      if (lcd && !waitingDrawn && s_webActive) {
        waitingDrawn = true;
        setupScreenUpdateStatus(lcd, -1);
      }
      delay(5);
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

    delay(5);
  }

  // 先摘掉全局指针，再 close/stop，最后断 AP（顺序很重要）
  s_server = nullptr;
  s_portalLcd = nullptr;
  server.close();
  delay(50);
  server.stop();
  delay(100);
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(200);

  if (outCfg) {
    if (result == PortalResult::Saved) {
      *outCfg = s_formCfg;
    } else if (!appConfigLoad(outCfg)) {
      appConfigSetDefaults(outCfg);
    }
  }

  return result;
}
