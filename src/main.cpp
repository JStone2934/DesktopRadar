/**
 * ESP32-C3 桌面气象雷达 — SoftAP 配网 + RGB565 全档缓存
 */

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <math.h>
#include <stdio.h>

#include "LGFX_GC9A01.hpp"
#include "alert_ring.h"
#include "app_config.h"
#include "button.h"
#include "compose.h"
#include "config.h"
#include "config_portal.h"
#include "frame_cache.h"
#include "progress_ring.h"
#include "wifi_sta.h"
#include "zoom_ctrl.h"

static LGFX lcd;
static AppConfig s_cfg;

static int s_displayedZoom = -1;
static uint32_t s_lastRefresh = 0;
static uint32_t s_refreshFailAt = 0;  // 非 0：上次定时刷新失败，待短重试
static bool s_busyCompose = false;
static bool s_wifiOk = false;
static bool s_statusScreen = false;  // 首次 Fetching 黑底，圆环不 blit 地图
static int s_bakeZoom = -1;
static float s_bakeLocal = 0.0f;
// 全档静帧曾铺满一次后进度环保持满（本地积累不再单独「动画阶段」）
static bool s_staticFullPassDone = false;
// 用户交互后暂停后台缓存（预取/定时刷新）
static uint32_t s_cachePauseUntil = 0;
// 切缩放宽限：保留上一档动画最多 ANIM_ZOOM_GRACE_MS
static int s_animPrevZoom = -1;
static uint32_t s_animPrevLeftAt = 0;

static void noteUserInteraction() {
  const uint32_t until = millis() + CACHE_PAUSE_AFTER_USER_MS;
  // 连按切档时延长安静窗
  if (until > s_cachePauseUntil) {
    s_cachePauseUntil = until;
  }
}

static bool cachePaused() {
  return (int32_t)(millis() - s_cachePauseUntil) < 0;
}

static float globalCacheDone01() {
  // 动画阶段：满环保持消失（勿在此调用 frameCacheAnimHas 打盘）
  if (s_staticFullPassDone) {
    return 1.0f;
  }
  const int slots = frameCacheZoomSlots();
  if (slots <= 0) {
    return 1.0f;
  }
  float ready = (float)frameCacheCountReady();
  if (s_bakeZoom >= ZOOM_MIN && s_bakeZoom <= ZOOM_MAX &&
      !frameCacheHas(s_bakeZoom)) {
    ready += s_bakeLocal;
  }
  if (ready > (float)slots) {
    ready = (float)slots;
  }
  return ready / (float)slots;
}

static void refreshProgressRing() {
  if (!s_cfg.show_progress) {
    return;
  }
  const int under = s_statusScreen ? -1 : s_displayedZoom;
  progressRingUpdate(&lcd, globalCacheDone01(), under);
}

/** 进度环之下、预警环之上；预警可盖住进度环。 */
static void refreshEdgeRings() {
  refreshProgressRing();
  const int under = s_statusScreen ? -1 : s_displayedZoom;
  if (s_cfg.show_alert_ring) {
    alertRingRedraw(&lcd, under);
  }
}

static void applyAlertForDisplayedZoom() {
  const int under = s_statusScreen ? -1 : s_displayedZoom;
  if (!s_cfg.show_alert_ring || s_displayedZoom < ZOOM_MIN) {
    alertRingClear(&lcd, under);
    return;
  }
  bool hasCloud = false;
  uint16_t color = 0;
  if (!frameCacheReadAlert(s_displayedZoom, &hasCloud, &color) || !hasCloud) {
    alertRingClear(&lcd, under);
    return;
  }
  alertRingSet(color, true);
}

static void onComposeProgress(int zoom, float local01) {
  s_bakeZoom = zoom;
  s_bakeLocal = local01;
  // 全档已过：环已隐藏，勿每瓦片刷屏/打盘
  if (s_staticFullPassDone) {
    return;
  }
  static uint32_t s_lastRingMs = 0;
  const uint32_t now = millis();
  if (local01 < 0.999f && (now - s_lastRingMs) < 200) {
    return;
  }
  s_lastRingMs = now;
  refreshEdgeRings();
}

static void showStatus(const char* line1, const char* line2 = nullptr) {
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextDatum(MC_DATUM);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.setFont(&fonts::Font4);
  lcd.drawString(line1, LCD_WIDTH / 2, LCD_HEIGHT / 2 - (line2 ? 12 : 0));
  if (line2) {
    lcd.setFont(&fonts::Font2);
    lcd.drawString(line2, LCD_WIDTH / 2, LCD_HEIGHT / 2 + 16);
  }
}

static void clearAllFrameCaches(const char* reason) {
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; z++) {
    frameCacheRemove(z);
  }
  frameCacheAnimClearAll();
  s_displayedZoom = -1;
  s_staticFullPassDone = false;
  s_animPrevZoom = -1;
  s_animPrevLeftAt = 0;
  Serial.printf("frame caches cleared (%s)\n",
                reason && reason[0] ? reason : "all");
}

static void clearPeerFrameCaches(int keepZoom, const char* reason) {
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; z++) {
    if (z == keepZoom) {
      continue;
    }
    frameCacheRemove(z);
  }
  // 只保留 keepZoom 动画可能；其它 anim 清掉
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; z++) {
    if (z != keepZoom) {
      frameCacheAnimClear(z);
    }
  }
  Serial.printf("peer frame caches cleared keep=z%d (%s)\n", keepZoom,
                reason && reason[0] ? reason : "peers");
}

static void retainAnimZooms(int current) {
  // 只保留 current +（宽限内的）prev；第三档立刻清
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; z++) {
    if (z == ZOOM_SKIP) {
      continue;
    }
    if (z == current) {
      continue;
    }
    if (z == s_animPrevZoom) {
      continue;
    }
    if (frameCacheAnimStoredCount(z) > 0) {
      frameCacheAnimClear(z);
    }
  }
}

static void noteZoomLeft(int fromZoom, int toZoom) {
  if (fromZoom < ZOOM_MIN || fromZoom > ZOOM_MAX || fromZoom == toZoom) {
    return;
  }
  // 切回宽限中的 prev：取消宽限
  if (toZoom == s_animPrevZoom) {
    s_animPrevZoom = -1;
    s_animPrevLeftAt = 0;
    retainAnimZooms(toZoom);
    return;
  }
  // 已有另一档在宽限：立刻清掉更早的那档
  if (s_animPrevZoom >= ZOOM_MIN && s_animPrevZoom != fromZoom &&
      s_animPrevZoom != toZoom) {
    frameCacheAnimClear(s_animPrevZoom);
  }
  s_animPrevZoom = fromZoom;
  s_animPrevLeftAt = millis() == 0 ? 1 : millis();
  retainAnimZooms(toZoom);
}

static void pumpAnimZoomGrace() {
  if (s_animPrevZoom < ZOOM_MIN) {
    return;
  }
  if (zoomCurrent() == s_animPrevZoom) {
    s_animPrevZoom = -1;
    s_animPrevLeftAt = 0;
    return;
  }
  if ((millis() - s_animPrevLeftAt) >= ANIM_ZOOM_GRACE_MS) {
    Serial.printf("anim grace expire clear z%d\n", s_animPrevZoom);
    frameCacheAnimClear(s_animPrevZoom);
    s_animPrevZoom = -1;
    s_animPrevLeftAt = 0;
  }
}

static bool nearlySameLoc(float aLat, float aLon, float bLat, float bLon) {
  return fabsf(aLat - bLat) < 1e-5f && fabsf(aLon - bLon) < 1e-5f;
}

static bool buildAndCache(int zoom, bool pushToDisplay);
static void handlePendingZoom();
static void pumpPrefetch();
static void ensureZoomVisible(int zoom, bool userInitiated);

static bool showCached(int zoom) {
  const uint32_t t0 = millis();
  if (!frameCacheBlit(&lcd, zoom)) {
    return false;
  }
  const uint32_t dt = millis() - t0;
  if (dt >= 30) {
    Serial.printf("blit z%d %lums\n", zoom, (unsigned long)dt);
  }
  s_displayedZoom = zoom;
  s_statusScreen = false;
  applyAlertForDisplayedZoom();
  // 进度已满时勿再整屏 blit 擦环
  if (!s_staticFullPassDone) {
    refreshEdgeRings();
  } else if (s_cfg.show_alert_ring) {
    alertRingRedraw(&lcd, zoom);
  }
  return true;
}

/** 造片阻塞中短按：已缓存则立刻秒切。 */
static void onPendingZoomFeedback(int zoom) {
  noteUserInteraction();
  if (frameCacheHas(zoom)) {
    showCached(zoom);
    return;
  }
  if (!s_staticFullPassDone) {
    refreshEdgeRings();
  }
}

static bool buildAndCache(int zoom, bool pushToDisplay) {
  if (!zoomCanCompose(zoom)) {
    return false;
  }
  s_busyCompose = true;
  composeClearAbort();
  s_bakeZoom = zoom;
  s_bakeLocal = 0.0f;

  if (pushToDisplay && s_displayedZoom < 0) {
    char line2[20];
    snprintf(line2, sizeof(line2), "zoom %d", zoom);
    s_statusScreen = true;
    showStatus("Fetching...", line2);
    refreshEdgeRings();
  } else if (pushToDisplay) {
    Serial.printf("rebuild z%d (keep display)\n", zoom);
  } else {
    Serial.printf("prefetch bake z%d (no display)\n", zoom);
  }

  const bool ok =
      composeRadarFrame(&lcd, s_cfg.lat, s_cfg.lon, zoom, pushToDisplay);
  s_busyCompose = false;
  s_bakeZoom = -1;
  s_bakeLocal = 0.0f;

  if (!ok) {
    if (composeAbortRequested()) {
      Serial.printf("build z%d aborted\n", zoom);
    } else {
      Serial.printf("build z%d fail\n", zoom);
    }
    frameCacheRestoreStale(zoom);
    s_statusScreen = false;
    refreshEdgeRings();
    return false;
  }

  if (pushToDisplay) {
    s_displayedZoom = zoom;
    s_statusScreen = false;
    applyAlertForDisplayedZoom();
  }
  // 当前档静帧成功 → 本地积累一帧（邻档预取不进动画队列）
  if (zoom == zoomCurrent() && frameCacheHas(zoom)) {
    uint32_t t = 0;
    if (frameCacheReadRadarTime(zoom, &t)) {
      frameCacheAnimAppendFromStatic(zoom, t);
    }
  }
  refreshEdgeRings();
  return true;
}

/** 按住播放：5fps 循环；松手或 ≥10s 退出。 */
static void playAnimLoop(int zoom) {
  const int n = frameCacheAnimCount(zoom);
  if (n < 2) {
    return;
  }

  progressRingHide(&lcd, s_displayedZoom);
  alertRingHide(&lcd, s_displayedZoom);
  Serial.printf("anim play z%d frames=%d @%dfps\n", zoom, n, ANIM_FPS);

  bool stopForSettings = false;
  while (buttonIsDown()) {
    if (buttonHeldMs() >= BTN_LONG_MS) {
      stopForSettings = true;
      break;
    }
    for (int i = 0; i < n; ++i) {
      if (!buttonIsDown() || buttonHeldMs() >= BTN_LONG_MS) {
        if (buttonHeldMs() >= BTN_LONG_MS) {
          stopForSettings = true;
        }
        break;
      }
      const uint32_t t0 = millis();
      if (!frameCacheAnimBlit(&lcd, zoom, i)) {
        Serial.printf("anim blit fail i=%d\n", i);
        break;
      }
      const uint32_t elapsed = millis() - t0;
      uint32_t remain = 0;
      if (elapsed < ANIM_FRAME_INTERVAL_MS) {
        remain = ANIM_FRAME_INTERVAL_MS - elapsed;
      }
      while (remain > 0) {
        if (!buttonIsDown() || buttonHeldMs() >= BTN_LONG_MS) {
          if (buttonHeldMs() >= BTN_LONG_MS) {
            stopForSettings = true;
          }
          remain = 0;
          break;
        }
        const uint32_t slice = remain > 20 ? 20 : remain;
        delay(slice);
        remain -= slice;
      }
      if (!buttonIsDown() || stopForSettings) {
        break;
      }
    }
    if (!buttonIsDown() || stopForSettings) {
      break;
    }
  }

  // 恢复最新静帧
  if (frameCacheHas(zoom)) {
    showCached(zoom);
  }
  Serial.printf("anim stop z%d settings=%d\n", zoom, (int)stopForSettings);

  // ≥10s：等松手以产生 LongPress（ISR 已在松手时锁存）
  if (stopForSettings) {
    while (buttonIsDown()) {
      delay(10);
    }
  }
}

static void pumpAnimOrPrefetch() {
  if (zoomHasPending()) {
    handlePendingZoom();
    return;
  }
  if (cachePaused()) {
    return;
  }
  if (s_busyCompose || WiFi.status() != WL_CONNECTED) {
    return;
  }

  const int z = zoomCurrent();
  if (frameCacheCountReady() >= frameCacheZoomSlots()) {
    s_staticFullPassDone = true;
  }

  // 本地积累：后台只补静帧；动画靠刷新/造片成功追加
  if (frameCacheCountReady() < frameCacheZoomSlots()) {
    const int readyBefore = frameCacheCountReady();
    pumpPrefetch();
    if (s_busyCompose) {
      return;
    }
    if (frameCacheCountReady() == readyBefore) {
      zoomPrefetchResetAround(z);
      pumpPrefetch();
    }
  }
}

static void ensureZoomVisible(int zoom, bool userInitiated) {
  if (userInitiated) {
    noteUserInteraction();
    // 打断后台造片，优先响应用户
    if (s_busyCompose) {
      composeRequestAbort();
    }
    zoomPrefetchClear();
  }

  const int prev = zoomCurrent();
  zoomSetCurrent(zoom);
  if (prev != zoom) {
    noteZoomLeft(prev, zoom);
  } else {
    retainAnimZooms(zoom);
  }

  if (frameCacheHas(zoom)) {
    showCached(zoom);
    if (!userInitiated) {
      zoomPrefetchResetAround(zoom);
    }
    return;
  }

  // 用户要看的未缓存档：立即造（仍优先于邻档预取）
  if (userInitiated && s_busyCompose) {
    composeRequestAbort();
  }
  if (!userInitiated) {
    zoomPrefetchClear();
  }

  if (!buildAndCache(zoom, true)) {
    if (!zoomHasPending() && s_displayedZoom < 0) {
      showStatus("Compose fail", "see serial");
    }
    return;
  }
  if (!userInitiated) {
    zoomPrefetchResetAround(zoom);
  }
}

static void handlePendingZoom() {
  for (;;) {
    const int pending = zoomTakePending();
    if (pending < 0) {
      break;
    }
    Serial.printf("handle pending z%d\n", pending);
    ensureZoomVisible(pending, true);
  }
}

static void handleShortPress() {
  const int next = zoomCycleNext();
  Serial.printf("short press -> z%d\n", next);
  ensureZoomVisible(next, true);
  handlePendingZoom();
}

static void pumpPrefetch() {
  if (zoomHasPending()) {
    handlePendingZoom();
    return;
  }
  if (s_busyCompose || WiFi.status() != WL_CONNECTED) {
    return;
  }
  int z = 0;
  if (!zoomPrefetchPop(&z)) {
    return;
  }
  if (frameCacheHas(z)) {
    return;
  }
  buildAndCache(z, false);
  handlePendingZoom();
}

static bool tryWifiAndRadar() {
  Serial.printf("apply cfg: mode=%u ssid=%s lat=%.5f lon=%.5f\n",
                (unsigned)s_cfg.wifi_mode, s_cfg.ssid, s_cfg.lat, s_cfg.lon);
  showStatus("Connecting...", s_cfg.ssid);
  s_wifiOk = wifiConnect(&s_cfg);
  if (!s_wifiOk) {
    showStatus("WiFi fail", s_cfg.ssid);
    Serial.println("hold BOOT 10s to re-open setup");
    return false;
  }

  char ipBuf[24];
  snprintf(ipBuf, sizeof(ipBuf), "%s", WiFi.localIP().toString().c_str());
  showStatus("WiFi OK", ipBuf);
  delay(600);

  ensureZoomVisible(zoomCurrent(), true);
  if (!frameCacheHas(zoomCurrent())) {
    Serial.printf("compose miss z%d — will retry in loop\n", zoomCurrent());
  }
  s_lastRefresh = millis();
  s_refreshFailAt = 0;
  return true;
}

/** 跑门户；按结果更新 s_cfg，必要时清缓存并重连。 */
static void runPortalAndApply() {
  composeRequestAbort();
  zoomPrefetchClear();
  s_busyCompose = false;
  progressRingHide(&lcd, s_displayedZoom);
  alertRingHide(&lcd, s_displayedZoom);

  const float oldLat = s_cfg.lat;
  const float oldLon = s_cfg.lon;

  const PortalResult pr =
      configPortalRun(&lcd, CONFIG_PORTAL_TIMEOUT_MS, &s_cfg);

  Serial.printf("portal result=%u ssid=%s\n", (unsigned)pr, s_cfg.ssid);

  if (pr == PortalResult::Saved) {
    if (!nearlySameLoc(oldLat, oldLon, s_cfg.lat, s_cfg.lon)) {
      clearAllFrameCaches("location change");
    }
  }

  if (!s_cfg.show_progress) {
    progressRingHide(&lcd, s_displayedZoom);
  }
  if (!s_cfg.show_alert_ring) {
    alertRingHide(&lcd, s_displayedZoom);
  }

  tryWifiAndRadar();
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("ESP32-C3 Radar SoftAP config + RGB565 zoom");

  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(255);

  buttonBegin();
  zoomSetCurrent(MAP_ZOOM);
  zoomSetPendingFeedback(onPendingZoomFeedback);
  composeSetProgressFn(onComposeProgress);

  showStatus("LittleFS...", "");
  if (!frameCacheBegin()) {
    showStatus("FS fail", "no cache");
  }

  // 上电 / RST：先进配置门户
  runPortalAndApply();
}

void loop() {
  static uint32_t lastBeat = 0;

  const ButtonEvent ev = buttonPoll();
  if (ev == ButtonEvent::LongPress) {
    Serial.println("long press -> config portal");
    showStatus("Setup...", "hold release ok");
    delay(200);
    runPortalAndApply();
    return;
  }
  if (ev == ButtonEvent::ShortPress) {
    if (s_wifiOk) {
      handleShortPress();
    }
  }

  if (!s_wifiOk) {
    delay(50);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnect(&s_cfg)) {
      ensureZoomVisible(zoomCurrent(), true);
      // 不重置 s_lastRefresh：避免闪断推迟整轮雷达刷新
    }
    delay(2000);
    return;
  }

  // 按住播放（本地积累满 ≥2 帧才可播；不再紧急 past 烘焙）
  if (!s_busyCompose && buttonIsDown()) {
    const uint32_t held = buttonHeldMs();
    const int z = zoomCurrent();
    if (held >= BTN_HOLD_PLAY_MS && frameCacheAnimHas(z)) {
      playAnimLoop(z);
      return;
    }
    if (held >= BTN_HOLD_PLAY_MS && frameCacheHas(z) && !frameCacheAnimHas(z)) {
      static uint32_t s_lastAnimMissLog = 0;
      if (millis() - s_lastAnimMissLog > 2000) {
        s_lastAnimMissLog = millis();
        Serial.printf("anim hold miss z%d stored=%d (need>=2)\n", z,
                      frameCacheAnimStoredCount(z));
      }
    }
  }

  // 当前档无成品时定期重试（避免首次造片失败后干等定时刷新）
  if (!s_busyCompose && !frameCacheHas(zoomCurrent()) &&
      zoomCanCompose(zoomCurrent())) {
    static uint32_t lastUncachedRetry = 0;
    if (millis() - lastUncachedRetry >= 15000) {
      lastUncachedRetry = millis();
      Serial.printf("retry uncached z%d\n", zoomCurrent());
      ensureZoomVisible(zoomCurrent(), true);
    }
  }

  // 到点或失败短重试：只更当前静帧并追加到本地动画队列
  bool shouldRefresh = false;
  if (s_refreshFailAt != 0) {
    shouldRefresh =
        (millis() - s_refreshFailAt >= RADAR_REFRESH_RETRY_MS);
  } else {
    shouldRefresh = (millis() - s_lastRefresh >= RADAR_REFRESH_MS);
  }
  if (!s_busyCompose && shouldRefresh && !cachePaused()) {
    const int z = zoomCurrent();
    if (zoomCanCompose(z)) {
      Serial.printf("scheduled refresh focus z%d (%s) [accumulate]\n", z,
                    s_refreshFailAt != 0 ? "retry" : "due");
      zoomPrefetchClear();
      const bool ok = buildAndCache(z, true);
      if (ok || frameCacheHas(z)) {
        s_lastRefresh = millis();
        s_refreshFailAt = 0;
      } else {
        s_refreshFailAt = millis() == 0 ? 1 : millis();
        Serial.printf("refresh fail, retry in %lus\n",
                      (unsigned long)(RADAR_REFRESH_RETRY_MS / 1000UL));
      }
      handlePendingZoom();
      zoomPrefetchResetAround(zoomCurrent());
    }
  }

  pumpAnimZoomGrace();
  pumpAnimOrPrefetch();

  // 预警环淡入：只叠彩环，不每圈强刷进度环
  if (s_cfg.show_alert_ring && alertRingNeedsTick()) {
    const int under = s_statusScreen ? -1 : s_displayedZoom;
    alertRingTick(&lcd, under);
  }

  if (millis() - lastBeat >= 5000) {
    lastBeat = millis();
    Serial.printf(
        "[%lu] heap=%u max=%u wifi=%d z=%d cached=%d anim=%d ring=%d/%d "
        "fs=%u/%u\n",
        millis() / 1000, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.RSSI(),
        zoomCurrent(), (int)frameCacheHas(zoomCurrent()),
        frameCacheAnimCount(zoomCurrent()), frameCacheCountReady(),
        frameCacheZoomSlots(), (unsigned)LittleFS.usedBytes(),
        (unsigned)LittleFS.totalBytes());
  }

  delay(10);
}
