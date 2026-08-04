/**
 * ESP32-C3 桌面气象雷达 — SoftAP 配网 + RGB565 全档缓存
 * （静帧秒切 + 刷新期间保留旧帧可秒切）
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
#include "rainviewer.h"
#include "wifi_sta.h"
#include "zoom_ctrl.h"

static LGFX lcd;
static AppConfig s_cfg;

static int s_displayedZoom = -1;
static uint32_t s_lastRefresh = 0;
static uint32_t s_refreshFailAt = 0;  // 非 0：上次定时刷新失败，待短重试
static bool s_busyCompose = false;
static bool s_wifiOk = false;
static bool s_statusScreen = false;  // 首次 Fetching 黑底，不 blit 地图
static int s_bakeZoom = -1;
static float s_bakeLocal = 0.0f;
// 全档静帧曾铺满一次后进度条保持满（隐藏）
static bool s_staticFullPassDone = false;
// 用户交互后暂停后台预取（不挡定时雷达刷新）
static uint32_t s_cachePauseUntil = 0;

static bool refreshIsDue();
static bool refreshApproaching();

static void noteUserInteraction() {
  const uint32_t until = millis() + CACHE_PAUSE_AFTER_USER_MS;
  if (until > s_cachePauseUntil) {
    s_cachePauseUntil = until;
  }
}

static bool cachePaused() {
  return (int32_t)(millis() - s_cachePauseUntil) < 0;
}

static float globalCacheDone01() {
  if (s_staticFullPassDone) {
    return 1.0f;
  }
  const int slots = frameCacheZoomSlots();
  if (slots <= 0) {
    return 1.0f;
  }
  float fresh = (float)frameCacheCountFresh();
  if (s_bakeZoom >= ZOOM_MIN && s_bakeZoom <= ZOOM_MAX &&
      !frameCacheIsFresh(s_bakeZoom)) {
    fresh += s_bakeLocal;
  }
  if (fresh > (float)slots) {
    fresh = (float)slots;
  }
  return fresh / (float)slots;
}

static void refreshProgressRing() {
  if (!s_cfg.show_progress) {
    return;
  }
  const int under = s_statusScreen ? -1 : s_displayedZoom;
  progressRingUpdate(&lcd, globalCacheDone01(), under);
  // 预警环与进度条分区绘制，互不强制重绘
}

/**
 * 按指定缩放档的预警数据重启呼吸环。
 * underlay 用当前屏上档位，与底图做透明混合。
 */
static void applyAlertForZoom(int alertZoom) {
  const int under = s_statusScreen ? -1 : s_displayedZoom;
  if (!s_cfg.show_alert_ring || alertZoom < ZOOM_MIN) {
    alertRingClear(&lcd, under);
    return;
  }
  bool hasCloud = false;
  uint16_t color = 0;
  if (!frameCacheReadAlert(alertZoom, &hasCloud, &color) || !hasCloud) {
    alertRingClear(&lcd, under);
    return;
  }
  // 只置状态；秒切路径里紧接着 Tick 画首帧（见 showCached）
  alertRingSet(color, true);
  (void)under;
}

static void applyAlertForDisplayedZoom() { applyAlertForZoom(s_displayedZoom); }

/**
 * composeRadarFrame 在 pushImage 上屏后立即回调：同步 s_displayedZoom 与预警环，
 * 使后续 reportComposeProgress → pumpAlertRingDuringCompose 使用正确档位底图。
 */
static void onComposeDisplay(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return;
  }
  s_displayedZoom = zoom;
  frameCacheSetProtectedZoom(zoom);
  s_statusScreen = false;
  zoomNoteDisplayed(zoom);
  applyAlertForZoom(zoom);
}

/** 造片/补满阻塞期间也推进预警呼吸，避免冻帧后跳变。 */
static void pumpAlertRingDuringCompose() {
  if (!s_cfg.show_alert_ring || !alertRingNeedsTick()) {
    return;
  }
  static uint32_t s_last = 0;
  const uint32_t now = millis();
  if ((now - s_last) < 100) {
    return;
  }
  s_last = now;
  const int under = s_statusScreen ? -1 : s_displayedZoom;
  alertRingTick(&lcd, under);
}

static void onComposeProgress(int zoom, float local01) {
  s_bakeZoom = zoom;
  s_bakeLocal = local01;
  // 邻档预取中若定时刷新已到期：立刻中止，把主循环还给当前档刷新
  if (zoom != zoomCurrent() && refreshIsDue()) {
    Serial.printf("abort prefetch z%d — refresh due\n", zoom);
    composeRequestAbort();
  }
  pumpAlertRingDuringCompose();
  if (s_staticFullPassDone) {
    return;
  }
  static uint32_t s_lastRingMs = 0;
  const uint32_t now = millis();
  if (local01 < 0.999f && (now - s_lastRingMs) < 200) {
    return;
  }
  s_lastRingMs = now;
  refreshProgressRing();
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
  frameCacheSetProtectedZoom(-1);
  s_displayedZoom = -1;
  zoomNoteDisplayed(-1);
  s_staticFullPassDone = false;
  Serial.printf("frame caches cleared (%s)\n",
                reason && reason[0] ? reason : "all");
}

static bool nearlySameLoc(float aLat, float aLon, float bLat, float bLon) {
  return fabsf(aLat - bLat) < 1e-5f && fabsf(aLon - bLon) < 1e-5f;
}

static bool refreshIsDue() {
  if (s_refreshFailAt != 0) {
    return (millis() - s_refreshFailAt) >= RADAR_REFRESH_RETRY_MS;
  }
  return (millis() - s_lastRefresh) >= RADAR_REFRESH_MS;
}

static bool refreshApproaching() {
  const uint32_t cushion = 15UL * 1000UL;
  if (s_refreshFailAt != 0) {
    return (millis() - s_refreshFailAt + cushion) >= RADAR_REFRESH_RETRY_MS;
  }
  return (millis() - s_lastRefresh + cushion) >= RADAR_REFRESH_MS;
}

static size_t fsFreeBytes() {
  const size_t total = LittleFS.totalBytes();
  const size_t used = LittleFS.usedBytes();
  return total > used ? (total - used) : 0;
}

static bool buildAndCache(int zoom, bool pushToDisplay);
static void handlePendingZoom();
static void pumpPrefetch();
static void ensureZoomVisible(int zoom, bool userInitiated);
static bool showCached(int zoom);

static bool showCached(int zoom) {
  const uint32_t t0 = millis();
  if (!frameCacheBlit(&lcd, zoom)) {
    Serial.printf("showCached z%d blit fail\n", zoom);
    return false;
  }
  const uint32_t dt = millis() - t0;
  if (dt >= 30) {
    Serial.printf("blit z%d %lums\n", zoom, (unsigned long)dt);
  }
  const bool switched = (s_displayedZoom != zoom);
  s_displayedZoom = zoom;
  frameCacheSetProtectedZoom(zoom);
  s_statusScreen = false;
  zoomNoteDisplayed(zoom);
  // 仅真正换档时重启呼吸；立刻画首帧（采样新图层），秒切本身仍只做一次 blit
  if (switched) {
    applyAlertForDisplayedZoom();
    if (s_cfg.show_alert_ring) {
      alertRingRedraw(&lcd, zoom);
    }
  }
  if (!s_staticFullPassDone) {
    refreshProgressRing();
  }
  // 整屏 blit 会擦掉屏缘预警；同档再显时补一帧
  if (s_cfg.show_alert_ring && !switched) {
    alertRingRedraw(&lcd, zoom);
  }
  return true;
}

/** 造片阻塞中短按：已缓存则立刻秒切；未缓存不碰预警环（避免地图未变却像切了档）。 */
static void onPendingZoomFeedback(int zoom) {
  noteUserInteraction();
  if (!frameCacheHas(zoom)) {
    Serial.printf("pending z%d not cached — wait for compose\n", zoom);
    return;
  }
  if (!showCached(zoom)) {
    return;
  }
  zoomSetCurrent(zoom);
  // 已上屏：清掉 pending，避免 abort 返回后再 ensure 同一档
  if (zoomClearPendingIf(zoom)) {
    Serial.printf("pending z%d applied in feedback\n", zoom);
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
    refreshProgressRing();
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
    refreshProgressRing();
    return false;
  }

  // display-only（无雷达 commit）不算造片成功，避免刷新时钟被空转推进
  if (!frameCacheHas(zoom)) {
    Serial.printf("build z%d not cached (display-only)\n", zoom);
    frameCacheRestoreStale(zoom);
    if (pushToDisplay && frameCacheHas(zoom)) {
      s_displayedZoom = zoom;
      s_statusScreen = false;
      showCached(zoom);
    }
    refreshProgressRing();
    return false;
  }

  if (pushToDisplay) {
    // s_displayedZoom / statusScreen / 预警环已由 onComposeDisplay 回调同步
  }
  refreshProgressRing();
  return true;
}

static void pumpBackgroundPrefetch() {
  if (zoomHasPending()) {
    handlePendingZoom();
    return;
  }
  if (cachePaused()) {
    return;
  }
  if (refreshIsDue() || refreshApproaching()) {
    return;
  }
  if (s_busyCompose || WiFi.status() != WL_CONNECTED) {
    return;
  }

  const int slots = frameCacheZoomSlots();
  const int freshBefore = frameCacheCountFresh();
  if (freshBefore >= slots) {
    s_staticFullPassDone = true;
    return;
  }

  pumpPrefetch();
  if (s_busyCompose) {
    return;
  }

  // 无进展：队列空或造片失败 → 重建预取队列
  if (frameCacheCountFresh() == freshBefore) {
    zoomPrefetchResetAround(zoomCurrent());
    static uint32_t s_lastPrefetchStallLog = 0;
    if (millis() - s_lastPrefetchStallLog > 10000) {
      s_lastPrefetchStallLog = millis();
      Serial.printf("prefetch stall fresh=%d/%d free=%u — requeue\n",
                    freshBefore, slots, (unsigned)fsFreeBytes());
    }
  }
}

static void ensureZoomVisible(int zoom, bool userInitiated) {
  if (userInitiated) {
    if (s_busyCompose) {
      composeRequestAbort();
    }
    zoomPrefetchClear();
  }

  zoomSetCurrent(zoom);

  // 已在屏上：不再 blit/呼吸，避免「跳了一下还是同一档」
  if (s_displayedZoom == zoom && frameCacheHas(zoom)) {
    zoomPrefetchResetAround(zoom);
    return;
  }

  if (frameCacheHas(zoom)) {
    // 已缓存秒切：不暂停预取，让后台继续铺其它档
    if (!showCached(zoom)) {
      if (s_displayedZoom >= ZOOM_MIN && s_displayedZoom <= ZOOM_MAX) {
        zoomSetCurrent(s_displayedZoom);
      }
      return;
    }
    zoomPrefetchResetAround(zoom);
    return;
  }

  // 未缓存：造片会占满主循环，短暂暂停预取
  if (userInitiated) {
    noteUserInteraction();
  }

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
    if (s_displayedZoom >= ZOOM_MIN && s_displayedZoom <= ZOOM_MAX) {
      Serial.printf("compose fail z%d — resync to display z%d\n", zoom,
                    s_displayedZoom);
      zoomSetCurrent(s_displayedZoom);
    }
    zoomPrefetchResetAround(zoomCurrent());
    return;
  }
  zoomPrefetchResetAround(zoom);
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
  // 以屏上实际档为基准循环，防止逻辑档与显示档脱节后出现跳档/乱序
  if (s_displayedZoom >= ZOOM_MIN && s_displayedZoom <= ZOOM_MAX &&
      s_displayedZoom != zoomCurrent()) {
    Serial.printf("short press resync logic=z%d display=z%d\n", zoomCurrent(),
                  s_displayedZoom);
    zoomSetCurrent(s_displayedZoom);
  }
  const int next = zoomCycleNext();
  Serial.printf("short press -> z%d (display was z%d)\n", next,
                s_displayedZoom);
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
  if (frameCacheIsFresh(z)) {
    return;
  }
  if (buildAndCache(z, false)) {
    zoomPrefetchNoteOk(z);
  } else if (!composeAbortRequested()) {
    zoomPrefetchNoteFail(z);
  }
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
  } else {
    // 首档就绪后立刻铺满邻档预取（不必等 5 分钟刷新）
    zoomPrefetchResetAround(zoomCurrent());
  }
  s_lastRefresh = millis();
  s_refreshFailAt = 0;
  return true;
}

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
    if (s_cfg.show_alert_ring) {
      alertRingRedraw(&lcd, s_displayedZoom);
    }
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
  composeSetDisplayFn(onComposeDisplay);

  showStatus("LittleFS...", "");
  if (!frameCacheBegin()) {
    showStatus("FS fail", "no cache");
  }

  runPortalAndApply();
}

void loop() {
  static uint32_t lastBeat = 0;

  const ButtonEvent ev = buttonPoll();
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
    }
    delay(2000);
    return;
  }

  const bool due = refreshIsDue();
  if (!s_busyCompose && due) {
    const int z = zoomCurrent();
    if (zoomCanCompose(z)) {
      Serial.printf("scheduled refresh focus z%d (%s) free=%u\n", z,
                    s_refreshFailAt != 0 ? "retry" : "due",
                    (unsigned)fsFreeBytes());
      zoomPrefetchClear();
      // 全量同一雷达时刻：标记全部过时（保留旧成品供刷新期间秒切）
      rainviewerInvalidatePin();
      s_staticFullPassDone = false;
      frameCacheMarkAllStaleExcept(z);
      frameCacheSetProtectedZoom(s_displayedZoom >= 0 ? s_displayedZoom : z);
      const bool ok = buildAndCache(z, true);
      if (ok) {
        s_lastRefresh = millis();
        s_refreshFailAt = 0;
        uint32_t t = 0;
        frameCacheReadRadarTime(z, &t);
        showCached(z);
        Serial.printf(
            "refresh ok z%d radar_t=%lu fresh=%d/%d free=%u — prefetch others\n", z,
            (unsigned long)t, frameCacheCountFresh(), frameCacheZoomSlots(),
            (unsigned)fsFreeBytes());
        handlePendingZoom();
        zoomPrefetchResetAround(zoomCurrent());
      } else {
        s_refreshFailAt = millis() == 0 ? 1 : millis();
        zoomPrefetchClear();
        Serial.printf("refresh fail, retry in %lus free=%u\n",
                      (unsigned long)(RADAR_REFRESH_RETRY_MS / 1000UL),
                      (unsigned)fsFreeBytes());
        handlePendingZoom();
      }
    }
  }

  if (!s_busyCompose && !frameCacheHas(zoomCurrent()) &&
      zoomCanCompose(zoomCurrent())) {
    static uint32_t lastUncachedRetry = 0;
    if (millis() - lastUncachedRetry >= 15000) {
      lastUncachedRetry = millis();
      Serial.printf("retry uncached z%d\n", zoomCurrent());
      ensureZoomVisible(zoomCurrent(), true);
    }
  }

  // 预警呼吸优先于预取/past 补满，避免秒切后迟迟不闪、环动画卡顿
  const bool alertBreathing =
      s_cfg.show_alert_ring && alertRingNeedsTick();
  if (alertBreathing) {
    const int under = s_statusScreen ? -1 : s_displayedZoom;
    alertRingTick(&lcd, under);
  }

  // 呼吸未结束时不启动阻塞式预取/补满（否则会冻住数秒再跳变）
  if (!s_busyCompose && !alertBreathing) {
    pumpBackgroundPrefetch();
  }

  if (millis() - lastBeat >= 5000) {
    lastBeat = millis();
    uint32_t refreshAge = 0;
    if (s_refreshFailAt != 0) {
      refreshAge = (millis() - s_refreshFailAt) / 1000UL;
    } else {
      refreshAge = (millis() - s_lastRefresh) / 1000UL;
    }
    Serial.printf(
        "[%lu] heap=%u max=%u wifi=%d z=%d cached=%d fresh=%d/%d "
        "fs=%u/%u refreshAge=%lus pause=%d failRetry=%d due=%d\n",
        millis() / 1000, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), WiFi.RSSI(),
        zoomCurrent(), (int)frameCacheHas(zoomCurrent()),
        frameCacheCountFresh(), frameCacheZoomSlots(),
        (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes(),
        (unsigned long)refreshAge, (int)cachePaused(),
        (int)(s_refreshFailAt != 0), (int)due);
  }

  delay(10);
}
