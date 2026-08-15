#include "ornament_portal.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <new>
#include <stdlib.h>

#include "config.h"
#include "ornament_media.h"
#include "update_manager.h"

namespace {

static WebServer* s_server = nullptr;
static AppConfig* s_cfg = nullptr;
static bool s_busy = false;
static bool s_uploadOk = false;
static char s_uploadError[128]{};
static OrnamentPortalAction s_action = OrnamentPortalAction::None;
static uint32_t s_actionAt = 0;
static constexpr int kApChannels[] = {1, 6, 11};

static void noCache() {
  if (!s_server) return;
  s_server->sendHeader("Cache-Control",
                       "no-store, no-cache, must-revalidate, max-age=0");
  s_server->sendHeader("Pragma", "no-cache");
  s_server->sendHeader("Expires", "0");
}

static void sendJson(int code, const String& body) {
  if (!s_server) return;
  noCache();
  s_server->send(code, "application/json; charset=utf-8", body);
}

static String escapedJson(const char* value) {
  String out;
  if (!value) return out;
  while (*value) {
    const char c = *value++;
    if (c == '\\' || c == '"') out += '\\';
    if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

static void handleStatus() {
  OrnamentMediaStatus media{};
  ornamentMediaGetStatus(&media);
  const size_t total = LittleFS.totalBytes();
  const size_t used = LittleFS.usedBytes();
  String json;
  json.reserve(240);
  json += F("{\"valid\":");
  json += media.valid ? F("true") : F("false");
  json += F(",\"type\":\"");
  json += media.type == OrnamentMediaType::Gif
              ? F("gif")
              : (media.type == OrnamentMediaType::Rgb565 ? F("image")
                                                          : F("none"));
  json += F("\",\"width\":");
  json += media.width;
  json += F(",\"height\":");
  json += media.height;
  json += F(",\"size\":");
  json += media.fileSize;
  json += F(",\"frames\":");
  json += media.frameCount;
  json += F(",\"paused\":");
  json += media.paused ? F("true") : F("false");
  json += F(",\"busy\":");
  json += s_busy ? F("true") : F("false");
  json += F(",\"free\":");
  json += total > used ? total - used : 0;
  json += '}';
  sendJson(200, json);
}

static const char kPageHead[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Storm Eye 摆件</title><style>
*{box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,"PingFang SC",sans-serif;max-width:480px;margin:auto;padding:18px 14px 36px;background:#dce2e9;color:#131d2b}h1{margin:4px 0;font-family:Georgia,serif}.lead{color:#536174;margin:4px 0 16px}.card{background:#f6f7f9;border:1px solid #8792a1;border-left:5px solid #17334f;padding:15px;margin:12px 0;box-shadow:4px 4px #17203322}.tag{font:700 11px ui-monospace,monospace;letter-spacing:.12em;color:#a03f46}.screen{width:240px;height:240px;border-radius:50%;overflow:hidden;background:#000;margin:14px auto;border:4px solid #26384c;box-shadow:0 6px 18px #0004;touch-action:none}.screen canvas,.screen img{width:240px;height:240px;object-fit:contain;display:block}label{display:block;font-weight:700;margin:11px 0 5px}input,select,button{width:100%;padding:12px;font-size:16px;border:1px solid #687587;background:#fff}button{margin-top:12px;background:#17334f;color:#fff;font-weight:800;box-shadow:3px 3px #0003}.danger{background:#8c3340}.muted{font-size:13px;color:#5b6878}.warn{background:#fff0da;border:1px solid #d2a057;padding:10px}.bar{height:8px;background:#cbd2db;margin-top:12px}.bar i{display:block;height:100%;width:0;background:#1d8f63}.row{display:flex;gap:8px}.row>*{flex:1}.hidden{display:none}.status{min-height:22px;margin-top:9px;font-size:14px;color:#314157}</style></head><body>
<div class="tag">STORM EYE · ORNAMENT MODE</div><h1>桌面摆件</h1>
<p class="lead">热点会持续开启。连接 RadarSetup 后可随时回到本页更换画面。</p>
<section class="card"><b>设备地址</b><p><code>http://192.168.4.1</code></p><div id="current" class="muted">正在读取媒体状态…</div></section>
<section class="card"><div class="tag">MEDIA</div><h2>更换画面</h2>
<label>选择静态图片或 GIF</label><input id="file" type="file" accept="image/jpeg,image/png,image/webp,image/gif">
<div id="staticOpts"><label>静态图片适配</label><select id="fit"><option value="cover">填满裁切</option><option value="contain">完整显示</option></select><p class="muted">填满模式可在圆形预览中拖动图片位置。</p></div>
<div class="screen" id="screen"><canvas id="preview" width="240" height="240"></canvas><img id="gifPreview" class="hidden"></div>
<div class="row"><button type="button" id="pause">暂停/继续</button><button type="button" id="delete">删除媒体</button></div>
<button type="button" id="upload">上传并应用</button><div class="bar"><i id="progress"></i></div><div class="status" id="message"></div></section>
<section class="card"><div class="tag">MODE</div><h2>返回雷达</h2><p class="warn">切换后会删除摆件媒体并重新构建全部雷达缩放缓存。</p><button type="button" class="danger" id="radar">切换到雷达模式</button></section>
)HTML";

static const char kPageScript[] PROGMEM = R"HTML(
<script>
const $=id=>document.getElementById(id),file=$('file'),canvas=$('preview'),ctx=canvas.getContext('2d',{willReadFrequently:true}),gif=$('gifPreview');let source=null,blob=null,kind='',ox=0,oy=0,drag=false,lastX=0,lastY=0,url='',sending=false;
function msg(s,bad=false){$('message').textContent=s;$('message').style.color=bad?'#a12636':'#314157'}
async function status(){try{const s=await fetch('/media/status',{cache:'no-store'}).then(r=>r.json());$('current').textContent=s.valid?`当前：${s.type==='gif'?'GIF':'静态图片'} · ${s.width}×${s.height} · ${(s.size/1024).toFixed(1)} KiB${s.paused?' · 已暂停':''}`:`尚未上传媒体 · 可用 ${(s.free/1024).toFixed(0)} KiB`;}catch(e){}}
function draw(){if(!source)return;ctx.fillStyle='#000';ctx.fillRect(0,0,240,240);const sw=source.naturalWidth||source.width,sh=source.naturalHeight||source.height,fit=$('fit').value;let scale=fit==='cover'?Math.max(240/sw,240/sh):Math.min(240/sw,240/sh),w=sw*scale,h=sh*scale,x=(240-w)/2+ox,y=(240-h)/2+oy;if(fit==='contain'){ox=oy=0;x=(240-w)/2;y=(240-h)/2}else{const mx=Math.max(0,(w-240)/2),my=Math.max(0,(h-240)/2);ox=Math.max(-mx,Math.min(mx,ox));oy=Math.max(-my,Math.min(my,oy));x=(240-w)/2+ox;y=(240-h)/2+oy}ctx.drawImage(source,x,y,w,h);}
function rgbBlob(){const p=ctx.getImageData(0,0,240,240).data,o=new Uint8Array(115200);for(let i=0,j=0;i<p.length;i+=4){const v=((p[i]>>3)<<11)|((p[i+1]>>2)<<5)|(p[i+2]>>3);o[j++]=v&255;o[j++]=v>>8}return new Blob([o],{type:'application/octet-stream'})}
file.onchange=()=>{blob=null;kind='';ox=oy=0;if(url)URL.revokeObjectURL(url);const f=file.files[0];if(!f)return;url=URL.createObjectURL(f);if(f.type==='image/gif'||f.name.toLowerCase().endsWith('.gif')){if(f.size>1400000){msg('GIF 不能超过 1.4 MB',true);return}kind='gif';blob=f;gif.src=url;gif.classList.remove('hidden');canvas.classList.add('hidden');$('staticOpts').classList.add('hidden');gif.onload=()=>{if(gif.naturalWidth>240||gif.naturalHeight>240){blob=null;msg('GIF 尺寸不能超过 240×240',true)}else msg(`GIF 已就绪：${gif.naturalWidth}×${gif.naturalHeight}`)}}else{if(f.size>20*1024*1024){msg('静态原图不能超过 20 MB',true);return}const im=new Image;im.onload=()=>{if(im.naturalWidth*im.naturalHeight>32000000){msg('图片像素超过 3200 万',true);return}source=im;kind='image';gif.classList.add('hidden');canvas.classList.remove('hidden');$('staticOpts').classList.remove('hidden');draw();msg('拖动预览可调整裁切位置')};im.onerror=()=>msg('浏览器无法读取该图片',true);im.src=url}};
$('fit').onchange=draw;$('screen').onpointerdown=e=>{if(kind!=='image')return;drag=true;lastX=e.clientX;lastY=e.clientY;$('screen').setPointerCapture(e.pointerId)};$('screen').onpointermove=e=>{if(!drag)return;ox+=e.clientX-lastX;oy+=e.clientY-lastY;lastX=e.clientX;lastY=e.clientY;draw()};$('screen').onpointerup=()=>drag=false;
async function post(path,data){const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)});const t=await r.text();if(!r.ok)throw Error(t);return t}
$('upload').onclick=async()=>{if(sending)return;try{if(kind==='image'){draw();blob=rgbBlob()}if(!blob)throw Error('请先选择有效文件');sending=true;$('upload').disabled=true;$('progress').style.width='0';msg('正在准备空间…');await post('/media/prepare',{type:kind,size:blob.size});const fd=new FormData;fd.append('media',blob,'media.bin');const x=new XMLHttpRequest;x.open('POST','/media/upload');x.upload.onprogress=e=>{if(e.lengthComputable)$('progress').style.width=(e.loaded/e.total*100)+'%'};x.onload=()=>{sending=false;$('upload').disabled=false;if(x.status===200){msg('已应用新画面');status()}else msg(x.responseText||'上传失败',true)};x.onerror=()=>{sending=false;$('upload').disabled=false;msg('连接中断，请重新上传',true)};x.send(fd)}catch(e){sending=false;$('upload').disabled=false;msg(e.message,true)}};
$('pause').onclick=async()=>{try{await post('/media/pause',{});status()}catch(e){msg(e.message,true)}};$('delete').onclick=async()=>{if(!confirm('删除当前媒体？'))return;try{await post('/media/delete',{});status();msg('媒体已删除')}catch(e){msg(e.message,true)}};$('radar').onclick=async()=>{if(!confirm('删除媒体并切换到雷达模式？'))return;try{await post('/mode/radar',{});msg('设备正在重启并重建雷达缓存…')}catch(e){msg(e.message,true)}};const ub=$('update');if(ub)ub.onclick=async()=>{if(!$('updateAck').checked){msg('请先确认更新会删除媒体',true);return}try{await post('/update/confirm',{ack:'1',nonce:ub.dataset.nonce,version_code:ub.dataset.version});msg('设备将关闭热点并开始更新…')}catch(e){msg(e.message,true)}};status();setInterval(status,5000);
</script></body></html>)HTML";

static void handleRoot() {
  if (!s_server) return;
  Serial.printf("ornament root opened heap=%u stations=%d\n",
                ESP.getFreeHeap(), WiFi.softAPgetStationNum());
  noCache();
  s_server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_server->send(200, "text/html; charset=utf-8", "");
  s_server->sendContent_P(kPageHead);
  UpdatePortalInfo update{};
  updateManagerGetPortalInfo(&update);
  if (update.available && update.haveManifest) {
    String section;
    section.reserve(560);
    section += F("<section class=\"card\"><div class=\"tag\">UPDATE</div><h2>固件更新</h2><p>发现版本 <b>");
    section += update.manifest.version;
    section += F("</b></p><p class=\"warn\">更新会删除当前图片或 GIF，完成后需要重新上传。</p><label><input id=\"updateAck\" type=\"checkbox\" style=\"width:auto\"> 我已确认媒体会被删除</label><button type=\"button\" class=\"danger\" id=\"update\" data-nonce=\"");
    section += update.nonce;
    section += F("\" data-version=\"");
    section += update.manifest.versionCode;
    section += F("\">安装更新</button></section>");
    s_server->sendContent(section);
  }
  s_server->sendContent_P(kPageScript);
  s_server->sendContent("");
}

static void handlePrepare() {
  if (!s_server) return;
  const String typeArg = s_server->arg("type");
  const size_t size = (size_t)strtoul(s_server->arg("size").c_str(), nullptr, 10);
  if (typeArg != "gif" && typeArg != "image") {
    sendJson(400, "{\"error\":\"不支持的媒体类型\"}");
    return;
  }
  const OrnamentMediaType type =
      typeArg == "gif" ? OrnamentMediaType::Gif : OrnamentMediaType::Rgb565;
  char error[128]{};
  s_busy = true;
  const bool ok = ornamentMediaPrepareUpload(type, size, error, sizeof(error));
  s_busy = false;
  if (!ok) {
    sendJson(400, String("{\"error\":\"") + escapedJson(error) + "\"}");
    return;
  }
  sendJson(200, "{\"ok\":true}");
}

static void handleUploadComplete() {
  if (!s_server) return;
  s_busy = false;
  if (s_uploadOk) {
    sendJson(200, "{\"ok\":true}");
  } else {
    sendJson(400, String("{\"error\":\"") + escapedJson(s_uploadError) +
                      "\"}");
  }
}

static void handleUploadData() {
  if (!s_server) return;
  HTTPUpload& upload = s_server->upload();
  if (upload.status == UPLOAD_FILE_START) {
    s_busy = true;
    s_uploadError[0] = '\0';
    s_uploadOk = ornamentMediaUploadBegin(s_uploadError, sizeof(s_uploadError));
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (s_uploadOk) {
      s_uploadOk = ornamentMediaUploadWrite(upload.buf, upload.currentSize,
                                             s_uploadError,
                                             sizeof(s_uploadError));
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (s_uploadOk) {
      s_uploadOk =
          ornamentMediaUploadFinish(s_uploadError, sizeof(s_uploadError));
    } else {
      ornamentMediaUploadAbort();
      if (ornamentMediaValid()) ornamentMediaBegin(nullptr);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    ornamentMediaUploadAbort();
    if (ornamentMediaValid()) ornamentMediaBegin(nullptr);
    s_uploadOk = false;
    snprintf(s_uploadError, sizeof(s_uploadError), "上传已中断");
  }
}

static void handlePause() {
  const bool paused = ornamentMediaTogglePause();
  sendJson(200, paused ? "{\"paused\":true}" : "{\"paused\":false}");
}

static void handleDelete() {
  ornamentMediaClearAll();
  ornamentMediaBegin(nullptr);
  sendJson(200, "{\"ok\":true}");
}

static void handleRadar() {
  if (!appConfigSetDisplayMode(DISPLAY_MODE_RADAR, true)) {
    sendJson(500, "{\"error\":\"NVS 保存失败\"}");
    return;
  }
  ornamentMediaClearAll();
  sendJson(200, "{\"ok\":true,\"restarting\":true}");
  s_action = OrnamentPortalAction::RestartRadar;
  s_actionAt = millis() + 1200;
}

static void handleUpdateConfirm() {
  if (!s_server) return;
  const bool acknowledged = s_server->arg("ack") == "1";
  const uint32_t nonce = strtoul(s_server->arg("nonce").c_str(), nullptr, 10);
  const uint32_t version =
      strtoul(s_server->arg("version_code").c_str(), nullptr, 10);
  if (!acknowledged || !updateManagerRequestFromPortal(version, nonce)) {
    sendJson(409, "{\"error\":\"更新信息无效或未确认\"}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"restarting\":true}");
  s_action = OrnamentPortalAction::UpdateRequested;
  s_actionAt = millis() + 1200;
}

static void resetWifi() {
  WiFi.scanDelete();
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  const wifi_mode_t mode = WiFi.getMode();
  if ((mode & WIFI_MODE_AP) != 0) {
    WiFi.softAPdisconnect(true);
    delay(120);
  }
  if ((mode & WIFI_MODE_STA) != 0) {
    WiFi.disconnect(true, true);
    delay(120);
  }
  if (mode != WIFI_MODE_NULL) {
    WiFi.mode(WIFI_OFF);
    delay(350);
  }
}

static int selectApChannel() {
  uint32_t scores[3]{};
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  delay(100);
  const int found = WiFi.scanNetworks(false, true);
  for (int i = 0; i < found; ++i) {
    const int channel = WiFi.channel(i);
    const int32_t rssi = WiFi.RSSI(i);
    const uint32_t strength =
        rssi <= -95 ? 1U : (uint32_t)(rssi >= -30 ? 65 : rssi + 95);
    for (size_t c = 0; c < sizeof(kApChannels) / sizeof(kApChannels[0]); ++c) {
      const int distance = abs(channel - kApChannels[c]);
      if (distance < 5) scores[c] += strength * (uint32_t)(5 - distance);
    }
  }
  WiFi.scanDelete();
  size_t best = 0;
  for (size_t c = 1; c < sizeof(kApChannels) / sizeof(kApChannels[0]); ++c) {
    if (scores[c] < scores[best]) best = c;
  }
  Serial.printf("ornament channel scan found=%d ch1=%lu ch6=%lu ch11=%lu select=%d\n",
                found, (unsigned long)scores[0], (unsigned long)scores[1],
                (unsigned long)scores[2], kApChannels[best]);
  return kApChannels[best];
}

static void apWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    const auto& station = info.wifi_ap_staconnected;
    Serial.printf("ornament AP client connected %02x:%02x:%02x:%02x:%02x:%02x aid=%u\n",
                  station.mac[0], station.mac[1], station.mac[2], station.mac[3],
                  station.mac[4], station.mac[5], (unsigned)station.aid);
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    const auto& station = info.wifi_ap_stadisconnected;
    Serial.printf("ornament AP client disconnected %02x:%02x:%02x:%02x:%02x:%02x aid=%u reason=%u (%s)\n",
                  station.mac[0], station.mac[1], station.mac[2], station.mac[3],
                  station.mac[4], station.mac[5], (unsigned)station.aid,
                  (unsigned)station.reason,
                  WiFi.disconnectReasonName((wifi_err_reason_t)station.reason));
  }
}

}  // namespace

bool ornamentPortalBegin(LGFX*, AppConfig* cfg) {
  if (s_server) return true;
  s_cfg = cfg;
  s_action = OrnamentPortalAction::None;
  s_actionAt = 0;
  resetWifi();
  const int apChannel = selectApChannel();
  // 扫描只短暂使用 STA；彻底关闭射频后再以纯 AP 模式启动，避免 AP/STA
  // 状态竞态进入摆件运行期。
  resetWifi();
  WiFi.onEvent(apWifiEvent);
  WiFi.mode(WIFI_AP);
  delay(50);
  const IPAddress ip(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  const IPAddress leaseStart(192, 168, 4, 20);
  const bool configured = WiFi.softAPConfig(ip, ip, subnet, leaseStart);
  const bool started =
      WiFi.softAP(SOFTAP_SSID, SOFTAP_PASS, apChannel, 0, 4);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  esp_wifi_set_ps(WIFI_PS_NONE);
  delay(250);
  if (!configured || !started) {
    resetWifi();
    return false;
  }
  WebServer* server = new (std::nothrow) WebServer(80);
  if (!server) {
    resetWifi();
    return false;
  }
  s_server = server;
  server->on("/", HTTP_GET, handleRoot);
  server->on("/media/status", HTTP_GET, handleStatus);
  server->on("/media/prepare", HTTP_POST, handlePrepare);
  server->on("/media/upload", HTTP_POST, handleUploadComplete,
             handleUploadData);
  server->on("/media/pause", HTTP_POST, handlePause);
  server->on("/media/delete", HTTP_POST, handleDelete);
  server->on("/mode/radar", HTTP_POST, handleRadar);
  server->on("/update/confirm", HTTP_POST, handleUpdateConfirm);
  server->onNotFound([]() {
    if (!s_server) return;
    noCache();
    s_server->sendHeader("Location", "/", true);
    s_server->send(302, "text/plain", "");
  });
  server->begin();
  Serial.printf("ornament AP ready ssid=%s channel=%d ip=%s config=%d start=%d heap=%u\n",
                SOFTAP_SSID, apChannel, WiFi.softAPIP().toString().c_str(),
                (int)configured, (int)started, ESP.getFreeHeap());
  return true;
}

void ornamentPortalService() {
  if (s_server) s_server->handleClient();
}

void ornamentPortalEnd() {
  if (!s_server) return;
  WebServer* server = s_server;
  s_server = nullptr;
  server->close();
  server->stop();
  delete server;
  s_cfg = nullptr;
  resetWifi();
}

bool ornamentPortalBusy() { return s_busy; }

OrnamentPortalAction ornamentPortalTakeAction() {
  if (s_action == OrnamentPortalAction::None || s_actionAt == 0 ||
      (int32_t)(millis() - s_actionAt) < 0) {
    return OrnamentPortalAction::None;
  }
  const OrnamentPortalAction action = s_action;
  s_action = OrnamentPortalAction::None;
  s_actionAt = 0;
  return action;
}
