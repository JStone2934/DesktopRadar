#include "config_portal.h"

#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button.h"
#include "config.h"
#include "setup_screen.h"
#include "zoom_ctrl.h"

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

static void resetPortalWifiRadio() {
  WiFi.scanDelete();
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.softAPdisconnect(true);
  delay(120);
  WiFi.disconnect(true, true);
  delay(120);
  WiFi.mode(WIFI_OFF);
  delay(350);
}

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
  html.reserve(16000);
  html += F("<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<meta http-equiv=\"Cache-Control\" content=\"no-store\">"
            "<title>风暴眼-桌面雷达设置</title><style>"
            ":root{color-scheme:light}"
            "*{box-sizing:border-box}"
            "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
            "max-width:460px;margin:0 auto;padding:18px 14px 30px;"
            "background:linear-gradient(180deg,#f4fbf7 0,#f7f8fb 210px,#f2f4f7 100%);"
            "color:#17212b;line-height:1.45}"
            ".hero{padding:12px 4px 14px}"
            "h1{font-size:1.58rem;margin:0 0 6px;letter-spacing:-.03em;color:#102018}"
            ".lead{margin:0;color:#5b6978;font-size:.94rem}"
            ".brand{display:inline-flex;align-items:center;gap:6px;margin-bottom:10px;"
            "padding:5px 10px;border-radius:999px;background:#dcf8e9;color:#167548;"
            "font-size:.78rem;font-weight:700}"
            ".dot{width:8px;height:8px;border-radius:50%;background:#23b76d;"
            "box-shadow:0 0 0 4px #23b76d22}"
            ".card{background:#fff;border:1px solid #e3e8ef;border-radius:20px;"
            "padding:17px;margin:12px 0;box-shadow:0 10px 28px rgba(25,42,62,.08)}"
            ".card h2{font-size:1.02rem;margin:0 0 12px;color:#17212b}"
            "label{display:block;margin:12px 0 5px;font-size:.88rem;color:#344255;font-weight:650}"
            "input,select{width:100%;padding:12px;border-radius:13px;"
            "border:1px solid #d5dde8;background:#fbfcfe;color:#17212b;font-size:1rem}"
            "input:focus,select:focus{outline:3px solid #23b76d25;border-color:#23b76d;background:#fff}"
            ".row{display:flex;gap:8px;align-items:stretch}"
            ".row select{width:6.6em;flex:0 0 auto}"
            ".row input{flex:1;min-width:0}"
            ".hint{color:#667789;font-size:.82rem;margin:7px 0 0}"
            ".mini{color:#7a8795;font-size:.78rem;margin:8px 0 0}"
            ".pill{display:inline-block;padding:2px 8px;border-radius:999px;"
            "background:#eef3f8;color:#657386;font-size:.72rem;margin-left:6px}"
            "button{width:100%;padding:14px;margin-top:16px;border:0;border-radius:15px;"
            "background:#19a763;color:#fff;font-size:1rem;font-weight:800;"
            "box-shadow:0 10px 20px rgba(25,167,99,.22)}"
            "button.btn-geo{margin-top:10px;background:#eef3f8;color:#243447;box-shadow:none;font-size:.95rem}"
            "button.btn-geo:disabled{opacity:.6}"
            ".check{display:flex;gap:8px;align-items:center;margin:10px 0 0;color:#5b6978;font-size:.86rem}"
            ".check input{width:auto}"
            "details{margin-top:14px;border-top:1px solid #edf1f5;padding-top:12px}"
            "summary{cursor:pointer;color:#506174;font-size:.9rem}"
            ".guide{margin-top:14px;padding:13px;border-radius:16px;background:#f0fbf5;"
            "border:1px solid #cbeedb;color:#506174;font-size:.82rem}"
            ".guide b{display:block;color:#163f2b;margin:8px 0 2px}"
            ".guide b:first-child{margin-top:0}"
            ".peap-only{display:none}"
            ".actions{padding:0 2px}"
            ".footerhint{background:#eaf8f1;border:1px solid #cbeedb;color:#35614a;"
            "border-radius:14px;padding:11px 12px;margin-top:10px}"
            ".footer{text-align:center;margin:18px 0 0;color:#718092;font-size:.82rem}"
            ".footer .url{display:block;color:#167548;word-break:break-all;margin-top:4px}"
            "</style></head><body>"
            "<div class=\"hero\"><div class=\"brand\"><span class=\"dot\"></span>RadarSetup 配置热点</div>"
            "<h1>风暴眼-桌面雷达设置</h1>"
            "<p class=\"lead\">选择要连接的 WiFi，确认雷达中心位置，保存后设备会自动继续运行。</p></div>"
            "<form method=\"POST\" action=\"/save\" accept-charset=\"UTF-8\" "
            "autocomplete=\"off\">"
            "<section class=\"card\"><h2>1. 连接网络</h2>"
            "<label>附近的 WiFi</label><select id=\"scan\" autocomplete=\"off\" "
            "onchange=\"pickScan()\"><option value=\"\">手动输入或保持当前网络</option>");
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
  html += F("</select><p class=\"hint\">列表按信号强度排列；如果没看到目标网络，可以直接在下面输入名称。</p>"
            "<label>网络名称 SSID</label><input name=\"ssid\" id=\"ssid\" required maxlength=\"32\" "
            "autocomplete=\"off\" autocapitalize=\"none\" autocorrect=\"off\" "
            "spellcheck=\"false\" value=\"");
  appendEscaped(html, s_seedCfg.ssid);
  // 密码不回填；留空则保留原密码。new-password 降低浏览器自动填充旧会话密码
  html += F("\"><label id=\"passLabel\">WiFi 密码</label>"
            "<input name=\"pass\" id=\"pass\" type=\"password\" maxlength=\"64\" "
            "value=\"\" autocomplete=\"new-password\" autocapitalize=\"none\" "
            "autocorrect=\"off\" spellcheck=\"false\">"
            "<p class=\"hint\" id=\"passHint\">如果已保存同一个网络，可以留空继续使用原密码。</p>"
            "<label class=\"check\"><input type=\"checkbox\" onclick=\""
            "document.getElementById('pass').type=this.checked?'text':'password'\">"
            "显示密码</label>"
            "<details id=\"advanced\"><summary>高级网络设置 <span class=\"pill\">若使用企业认证网络</span></summary>"
            "<label>认证类型</label><select name=\"mode\" id=\"mode\" "
            "autocomplete=\"off\" onchange=\"netChanged(true)\">");
  html += s_seedCfg.wifi_mode == APP_WIFI_PSK
              ? F("<option value=\"0\" selected>普通密码 WiFi</option>")
              : F("<option value=\"0\">普通密码 WiFi</option>");
  html += s_seedCfg.wifi_mode == APP_WIFI_PEAP
              ? F("<option value=\"1\" selected>企业 WiFi / 校园网 PEAP</option>")
              : F("<option value=\"1\">企业 WiFi / 校园网 PEAP</option>");
  html += s_seedCfg.wifi_mode == APP_WIFI_OPEN
              ? F("<option value=\"2\" selected>开放网络</option>")
              : F("<option value=\"2\">开放网络</option>");
  html += F("</select>"
            "<p class=\"mini\">学校或公司网络通常选择企业 WiFi；家用路由器通常选择普通密码 WiFi。</p>"
            "<div class=\"peap-only\" id=\"idRow\"><label>PEAP 用户名</label>"
            "<input name=\"identity\" id=\"identity\" maxlength=\"63\" autocomplete=\"off\" "
            "autocapitalize=\"none\" spellcheck=\"false\" value=\"");
  appendEscaped(html, s_seedCfg.identity);
  html += F("\"><label>外层 Identity（可选）</label>"
            "<input name=\"outer_identity\" id=\"outer_identity\" maxlength=\"63\" "
            "autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\" "
            "placeholder=\"留空则使用 PEAP 用户名\" value=\"");
  appendEscaped(html, s_seedCfg.outer_identity);
  html += F("\"></div></details></section>"
            "<section class=\"card\"><h2>2. 设备位置</h2>"
            "<p class=\"hint\">用于决定雷达图中心。中国大陆通常选择北纬、东经；小数点示例：23.12910。</p>"
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
            "<p class=\"hint\" id=\"geoHint\"></p></section>"
            "<section class=\"card\"><h2>3. 显示选项</h2>"
            "<label>显示进度条</label><select name=\"show_ring\" "
            "autocomplete=\"off\">");
  html += s_seedCfg.show_progress
              ? F("<option value=\"1\" selected>显示</option>"
                  "<option value=\"0\">隐藏</option>")
              : F("<option value=\"1\">显示</option>"
                  "<option value=\"0\" selected>隐藏</option>");
  html += F("</select>"
            "<p class=\"hint\">它表示设备正在下载、合成并缓存雷达画面；关闭后只是不显示提示，不影响自动更新。</p>"
            "<label>天气预警环</label><select name=\"show_alert\" "
            "autocomplete=\"off\">");
  html += s_seedCfg.show_alert_ring
              ? F("<option value=\"1\" selected>开启</option>"
                  "<option value=\"0\">关闭</option>")
              : F("<option value=\"1\">开启</option>"
                  "<option value=\"0\" selected>关闭</option>");
  html += F("</select>"
            "<p class=\"hint\">显示你附近的天气情况。</p>"
            "<label>中心十字</label><select name=\"show_crosshair\" "
            "autocomplete=\"off\">");
  html += s_seedCfg.show_crosshair
              ? F("<option value=\"1\" selected>显示</option>"
                  "<option value=\"0\">隐藏</option>")
              : F("<option value=\"1\">显示</option>"
                  "<option value=\"0\" selected>隐藏</option>");
  html += F("</select>"
            "<p class=\"hint\">用于标记雷达画面中心位置；隐藏后不影响天气数据更新和缩放切换。</p>"
            "<label>默认打开的缩放等级</label><select name=\"default_zoom\" "
            "autocomplete=\"off\">");
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; ++z) {
    if (!zoomCanCompose(z)) {
      continue;
    }
    html += F("<option value=\"");
    html += String(z);
    html += s_seedCfg.default_zoom == z ? F("\" selected>z") : F("\">z");
    html += String(z);
    html += F("</option>");
  }
  html += F("</select>"
            "<p class=\"hint\">设备启动后会先打开这个缩放等级；运行中按住 S键 2 秒也会直接跳回这里。z3 视野最大、范围最广；z12 放大最多、细节最多。</p>"
            "<div class=\"guide\"><b>切换缩放有时卡顿怎么办？</b>"
            "短按 S键会切换缩放；如果刚好在下载或生成缓存，可能会慢几秒。等进度结束或再短按一次即可，通常不需要重新配置 WiFi。</div>"
            "</section><div class=\"actions\"><button type=\"submit\">保存设置并连接 WiFi</button>"
            "<p class=\"hint footerhint\">保存后手机会从 RadarSetup 热点断开，这是正常现象；设备会开始连接你选择的 WiFi。</p></div></form>"
            "<footer class=\"footer\">项目 GitHub<span class=\"url\">"
            "https://github.com/JStone2934/DesktopRadar/tree/esp32c3</span></footer>"
            "<script>"
            "function pickScan(){var s=document.getElementById('scan'),x=s.options[s.selectedIndex];"
            "if(!x||!x.value)return;document.getElementById('ssid').value=x.value;"
            "if(x.dataset.mode)document.getElementById('mode').value=x.dataset.mode;netChanged(true);}"
            "function netChanged(openAdvanced){var m=document.getElementById('mode').value;"
            "var p=m==='1',o=m==='2';"
            "if(p&&openAdvanced)document.getElementById('advanced').open=true;"
            "document.getElementById('idRow').style.display=p?'block':'none';"
            "document.getElementById('identity').required=p;"
            "document.getElementById('outer_identity').disabled=!p;"
            "document.getElementById('pass').disabled=o;"
            "document.getElementById('passLabel').textContent=o?'WiFi 密码':'WiFi 密码';"
            "document.getElementById('passHint').textContent=o?'开放网络无需填写密码。':"
            "(p?'如果已保存同一个网络和用户名，可以留空继续使用原密码。':'如果已保存同一个网络，可以留空继续使用原密码。');}"
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
            "netChanged(false);</script>"
            "</body></html>");
  sendNoStoreHeaders();
  s_server->send(200, "text/html; charset=utf-8", html);
}

static const char kSavedHtml[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Cache-Control" content="no-store">
<title>设置已保存</title>
<style>
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:linear-gradient(180deg,#f4fbf7 0,#f7f8fb 210px,#f2f4f7 100%);color:#17212b;margin:0;padding:28px 16px;text-align:center}
.card{max-width:420px;margin:36px auto 0;background:#fff;border:1px solid #e3e8ef;border-radius:20px;padding:24px 18px;box-shadow:0 10px 28px rgba(25,42,62,.08)}
h1{font-size:1.45rem;margin:0 0 10px}.ok{font-size:2rem;margin-bottom:8px;color:#19a763}
p{color:#5b6978;line-height:1.5;margin:8px 0}.small{font-size:.84rem;color:#7a8795;margin-top:18px}
</style>
</head><body><div class="card"><div class="ok">✓</div><h1>设置已保存</h1>
<p>手机会从 RadarSetup 热点断开，这是正常现象。</p>
<p>设备正在关闭热点，并连接你选择的 WiFi。</p>
<p class="small">如果下次需要重新设置，请再次进入配置模式后打开 http://192.168.4.1/。</p>
</div>
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
  cfg->show_crosshair = (s_server->arg("show_crosshair") != "0");

  String defaultZoomStr = s_server->arg("default_zoom");
  defaultZoomStr.trim();
  if (defaultZoomStr.length() == 0) {
    cfg->default_zoom = s_seedCfg.default_zoom;
  } else {
    char* end = nullptr;
    const long z = strtol(defaultZoomStr.c_str(), &end, 10);
    if (!end || end == defaultZoomStr.c_str() || *end != '\0' ||
        !zoomCanCompose((int)z)) {
      return "默认缩放等级无效";
    }
    cfg->default_zoom = (int)z;
  }
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
    String html;
    html.reserve(1800);
    html += F("<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<meta http-equiv=\"Cache-Control\" content=\"no-store\">"
              "<title>保存失败</title><style>"
              "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
              "background:linear-gradient(180deg,#f4fbf7 0,#f7f8fb 210px,#f2f4f7 100%);"
              "color:#17212b;margin:0;padding:28px 16px}"
              ".card{max-width:420px;margin:28px auto;background:#fff;border:1px solid #f0c4cb;"
              "border-radius:20px;padding:22px 18px;box-shadow:0 10px 28px rgba(25,42,62,.08)}"
              "h1{font-size:1.35rem;margin:0 0 10px}.reason{background:#fff1f3;border:1px solid #f0c4cb;"
              "color:#a43647;border-radius:14px;padding:12px;margin:14px 0}"
              "p{color:#5b6978;line-height:1.5}a{display:block;text-align:center;text-decoration:none;"
              "background:#19a763;color:#fff;font-weight:700;border-radius:14px;padding:13px;margin-top:18px}"
              "</style></head><body><div class=\"card\"><h1>保存失败</h1>"
              "<p>有一项设置需要修改后才能继续。</p><div class=\"reason\">");
    appendEscaped(html, err);
    html += F("</div><p>请返回上一页检查输入。纬度/经度请使用英文小数点，例如 23.12910。</p>"
              "<a href=\"/\">返回修改</a></div></body></html>");
    s_server->send(400, "text/html; charset=utf-8", html);
    return;
  }
  if (!appConfigSave(&cfg)) {
    sendNoStoreHeaders();
    s_server->send(500, "text/plain; charset=utf-8", "NVS 保存失败");
    return;
  }
  s_formCfg = cfg;
  Serial.printf("config saved: mode=%u ssid=%s lat=%.4f lon=%.4f ring=%d alert=%d cross=%d defZoom=%d\n",
                (unsigned)cfg.wifi_mode, cfg.ssid, cfg.lat, cfg.lon,
                (int)cfg.show_progress, (int)cfg.show_alert_ring,
                (int)cfg.show_crosshair, cfg.default_zoom);
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

  resetPortalWifiRadio();

  // scanNetworks() 会打开 STA。扫描必须发生在 SoftAP 启动前，避免设置
  // 热点变成 AP+STA 混合状态，保存后再切企业 WiFi 时继承不干净的射频状态。
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  delay(100);
  scanPortalNetworks();
  resetPortalWifiRadio();

  // 使用 WPA2 配置热点，保留手机端稳定拿 IP 的改动；但这里明确只进入
  // AP 模式。保存后会完整关闭 AP，再由 wifi_sta.cpp 单独启动 STA/PEAP。
  WiFi.mode(WIFI_AP);
  delay(50);
  const IPAddress apIP(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  const IPAddress leaseStart(192, 168, 4, 20);
  const bool apConfigOk = WiFi.softAPConfig(apIP, gateway, subnet, leaseStart);
  const bool apOk = WiFi.softAP(SOFTAP_SSID, SOFTAP_PASS, 6, 0, 4);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  delay(250);

  const IPAddress ip = WiFi.softAPIP();
  const bool modeOk = (WiFi.getMode() & WIFI_AP) != 0;
  const bool configOk = ip == apIP;
  Serial.printf(
      "SoftAP %s WPA2 channel=6 mode=%d ip=%d dhcp=%d ap=%d IP=%s "
      "leaseStart=%s\n",
      SOFTAP_SSID, (int)modeOk, (int)configOk, (int)apConfigOk, (int)apOk,
      ip.toString().c_str(), leaseStart.toString().c_str());

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
      Serial.println("portal skipped by S key");
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
  resetPortalWifiRadio();

  if (outCfg) {
    if (result == PortalResult::Saved) {
      *outCfg = s_formCfg;
    } else if (!appConfigLoad(outCfg)) {
      appConfigSetDefaults(outCfg);
    }
  }

  return result;
}
