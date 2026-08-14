/**
 * ESP32-C3 桌面气象雷达 — SoftAP 配网 + RGB565 全档缓存
 * （静帧秒切 + 刷新期间保留旧帧可秒切）
 */

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
#include "wind_field.h"
#include "wind_particles.h"
#include "zoom_ctrl.h"

static LGFX lcd;
static AppConfig s_cfg;

static int s_displayedZoom = -1;
static uint32_t s_lastRefresh = 0;
static uint32_t s_refreshFailAt = 0;  // 非 0：上次定时刷新失败，待短重试
static bool s_busyCompose = false;
static bool s_wifiOk = false;
static uint32_t s_wifiRetryAt = 0;
static uint8_t s_wifiRetryStage = 0;
static bool s_statusScreen = false;  // 首次 Fetching 黑底，不 blit 地图
static int s_bakeZoom = -1;
static float s_bakeLocal = 0.0f;
// 全档静帧曾铺满一次后进度条保持满（隐藏）
static bool s_staticFullPassDone = false;
// 用户交互后暂停后台预取（不挡定时雷达刷新）
static uint32_t s_cachePauseUntil = 0;
// 每档最近一次成功造片时刻；仅风场模式用来做 5/15/45 分钟分级刷新。
static uint32_t s_zoomRefreshedAt[ZOOM_MAX - ZOOM_MIN + 1]{};
// S 键长按跳转前的临时视觉反馈（只画 LCD，不改缓存）。闪烁相位独立
// 于风场整屏刷新；整屏刷新只要求重画当前相位，不重置相位时钟。
static bool s_longCueActive = false;
static bool s_longCueShowCrosshair = true;
static bool s_longCueNeedsRedraw = false;
static bool s_longCueSuppressUntilRelease = false;
static uint32_t s_longCueNextToggleAt = 0;

static bool refreshIsDue();
static bool refreshApproaching();
static void updateLongPressCue();
static void pumpWindAnimation(bool busy);
static void pumpWindDuringBlock();
static void serviceWindField();
static uint32_t nonzeroMillis();

static constexpr uint32_t kWifiRetryBackoffMs[] = {
    60UL * 1000UL, 3UL * 60UL * 1000UL, 10UL * 60UL * 1000UL};

static void scheduleWifiRetry() {
  const uint8_t last =
      (uint8_t)(sizeof(kWifiRetryBackoffMs) / sizeof(kWifiRetryBackoffMs[0]) -
                1U);
  const uint8_t slot = s_wifiRetryStage > last ? last : s_wifiRetryStage;
  const uint32_t delayMs = kWifiRetryBackoffMs[slot];
  s_wifiRetryAt = nonzeroMillis() + delayMs;
  if (s_wifiRetryStage < last) {
    ++s_wifiRetryStage;
  }
  Serial.printf("WiFi retry scheduled in %lus (next backoff stage=%u)\n",
                (unsigned long)(delayMs / 1000UL),
                (unsigned)s_wifiRetryStage);
}

static void noteUserInteraction() {
  const uint32_t until = millis() + CACHE_PAUSE_AFTER_USER_MS;
  if (until > s_cachePauseUntil) {
    s_cachePauseUntil = until;
  }
}

static bool cachePaused() {
  return (int32_t)(millis() - s_cachePauseUntil) < 0;
}

static inline int zoomRefreshSlot(int zoom) { return zoom - ZOOM_MIN; }

static uint32_t nonzeroMillis() {
  const uint32_t now = millis();
  return now == 0 ? 1 : now;
}

static void noteZoomRefreshed(int zoom) {
  if (!zoomCanCompose(zoom)) {
    return;
  }
  s_zoomRefreshedAt[zoomRefreshSlot(zoom)] = nonzeroMillis();
}

static bool zoomRefreshDue(int zoom, uint32_t intervalMs) {
  if (!zoomCanCompose(zoom)) {
    return false;
  }
  const uint32_t at = s_zoomRefreshedAt[zoomRefreshSlot(zoom)];
  return at == 0 || (millis() - at) >= intervalMs;
}

static uint32_t zoomRefreshAgeMs(int zoom) {
  if (!zoomCanCompose(zoom)) {
    return 0;
  }
  const uint32_t at = s_zoomRefreshedAt[zoomRefreshSlot(zoom)];
  return at == 0 ? 0 : millis() - at;
}

static void seedZoomRefreshTimesFromCache() {
  const uint32_t now = nonzeroMillis();
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; ++z) {
    if (zoomCanCompose(z) && frameCacheHas(z) &&
        s_zoomRefreshedAt[zoomRefreshSlot(z)] == 0) {
      s_zoomRefreshedAt[zoomRefreshSlot(z)] = now;
    }
  }
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
  updateLongPressCue();
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
  memset(s_zoomRefreshedAt, 0, sizeof(s_zoomRefreshedAt));
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
  if (s_cfg.show_wind_particles) {
    return zoomRefreshDue(zoomCurrent(), RADAR_REFRESH_MS);
  }
  return (millis() - s_lastRefresh) >= RADAR_REFRESH_MS;
}

static bool refreshApproaching() {
  const uint32_t cushion = 15UL * 1000UL;
  if (s_refreshFailAt != 0) {
    return (millis() - s_refreshFailAt + cushion) >= RADAR_REFRESH_RETRY_MS;
  }
  if (s_cfg.show_wind_particles) {
    const int zoom = zoomCurrent();
    if (!zoomCanCompose(zoom) ||
        s_zoomRefreshedAt[zoomRefreshSlot(zoom)] == 0) {
      return true;
    }
    return zoomRefreshAgeMs(zoom) + cushion >= RADAR_REFRESH_MS;
  }
  return (millis() - s_lastRefresh + cushion) >= RADAR_REFRESH_MS;
}

static size_t fsFreeBytes() {
  const size_t total = LittleFS.totalBytes();
  const size_t used = LittleFS.usedBytes();
  return total > used ? (total - used) : 0;
}

/**
 * 风场模式只把到期档标为 stale，绝不删除 ready 成品：旧图仍可秒切，
 * 后台队列按当前、±1、±2… 的顺序逐档替换。
 */
static void scheduleWindStaticRefreshes() {
  if (!s_cfg.show_wind_particles) {
    return;
  }
  static uint32_t lastScanAt = 0;
  const uint32_t now = millis();
  if (lastScanAt != 0 && (now - lastScanAt) < 2000UL) {
    return;
  }
  lastScanAt = now;

  const int center = zoomCurrent();
  int staleCount = 0;
  for (int z = ZOOM_MIN; z <= ZOOM_MAX; ++z) {
    if (!zoomCanCompose(z) || z == center || !frameCacheHas(z) ||
        !frameCacheIsFresh(z)) {
      continue;
    }
    const uint32_t interval =
        abs(z - center) <= 1 ? WIND_ADJACENT_REFRESH_MS
                             : WIND_FAR_REFRESH_MS;
    if (!zoomRefreshDue(z, interval)) {
      continue;
    }
    frameCacheMarkFresh(z, false);
    ++staleCount;
  }

  if (staleCount > 0) {
    s_staticFullPassDone = false;
    zoomPrefetchResetAround(center);
    refreshProgressRing();
    Serial.printf("wind static tier: stale=%d center=z%d fresh=%d/%d\n",
                  staleCount, center, frameCacheCountFresh(),
                  frameCacheZoomSlots());
  }
}

static void forceWifiReconnectAfterFetchFail(const char* reason) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  Serial.printf("WiFi recovery: disconnect after %s\n",
                reason && reason[0] ? reason : "fetch failure");
  WiFi.disconnect(false, false);
}

static bool buildAndCache(int zoom, bool pushToDisplay);
static void handlePendingZoom();
static void pumpPrefetch();
static void ensureZoomVisible(int zoom, bool userInitiated);
static bool showCached(int zoom);

static bool restoreCrosshairCueUnderlay(int zoom) {
  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  // 十字半径为 8px；高亮闪烁会加粗到 3px。多留 2px 避免边缘残留。
  const int pad = 12;
  return frameCacheRestoreRect(&lcd, zoom, cx - pad, cy - pad, pad * 2 + 1,
                               pad * 2 + 1);
}

static bool paintLongPressCue(bool visible) {
  const int under = s_statusScreen ? -1 : s_displayedZoom;
  if (under < ZOOM_MIN || under > ZOOM_MAX) {
    return false;
  }
  return visible ? restoreCrosshairCueUnderlay(under)
                 : frameCacheHideCrosshair(&lcd, under);
}

static void clearLongPressCue() {
  if (!s_longCueActive) {
    return;
  }
  const int under = s_statusScreen ? -1 : s_displayedZoom;
  if (under >= ZOOM_MIN && under <= ZOOM_MAX) {
    restoreCrosshairCueUnderlay(under);
  }
  s_longCueActive = false;
  s_longCueShowCrosshair = true;
  s_longCueNeedsRedraw = false;
  s_longCueNextToggleAt = 0;
}

static void updateLongPressCue() {
  if (!s_wifiOk) {
    clearLongPressCue();
    s_longCueSuppressUntilRelease = false;
    return;
  }
  if (!buttonIsDown()) {
    clearLongPressCue();
    s_longCueSuppressUntilRelease = false;
    return;
  }
  if (s_longCueSuppressUntilRelease) {
    return;
  }
  if (!s_cfg.show_crosshair) {
    clearLongPressCue();
    return;
  }
  const uint32_t held = buttonHeldMs();
  if (held < BTN_LONG_FEEDBACK_MS) {
    clearLongPressCue();
    return;
  }
  if (held >= BTN_LONG_MS) {
    // 无论长按事件是在主循环还是阻塞下载路径中被消费，都不能把最后的
    // “隐藏”相位留在屏上；等松手后再解除抑制。
    clearLongPressCue();
    s_longCueSuppressUntilRelease = true;
    return;
  }

  const uint32_t now = millis();
  if (!s_longCueActive) {
    s_longCueActive = true;
    s_longCueShowCrosshair = false;  // 首相位隐藏，立即产生可见反馈
    s_longCueNeedsRedraw = true;
    s_longCueNextToggleAt = now + 160UL;
  } else {
    // 用独立截止时间推进相位。即使一次绘屏阻塞跨过多个周期，也按跨过
    // 的周期数保持正确奇偶相位，而不是从按住时长重新初始化。
    while ((int32_t)(now - s_longCueNextToggleAt) >= 0) {
      s_longCueShowCrosshair = !s_longCueShowCrosshair;
      s_longCueNeedsRedraw = true;
      s_longCueNextToggleAt += 160UL;
    }
  }

  if (!s_longCueNeedsRedraw) {
    return;
  }
  if (paintLongPressCue(s_longCueShowCrosshair)) {
    s_longCueNeedsRedraw = false;
  }
}

/**
 * 风场每帧会从 Flash 恢复干净成品，因此随后补画动态 UI。
 * 阻塞 HTTP/PNG 路径传 busy=true，自动降至约 2 FPS。
 */
static void pumpWindAnimation(bool busy) {
  static bool pumping = false;
  if (pumping || !s_cfg.show_wind_particles || !s_wifiOk || s_statusScreen ||
      s_displayedZoom < ZOOM_MIN || s_displayedZoom > ZOOM_MAX) {
    return;
  }
  pumping = true;
  if (windParticlesTick(&lcd, s_displayedZoom, busy)) {
    progressRingRedraw(&lcd);
    if (s_cfg.show_alert_ring) {
      alertRingRedraw(&lcd, s_displayedZoom);
    }
    // 整屏恢复会把十字恢复成显示状态。若状态机当前要求隐藏，仅重画
    // 这一相位；绝不重置闪烁时钟，避免与约 6 FPS 风场刷新发生拍频。
    s_longCueNeedsRedraw =
        s_longCueActive && !s_longCueShowCrosshair;
    updateLongPressCue();
  }
  pumping = false;
}

static void pumpWindDuringBlock() {
  // HTTPS/解码阻塞期间不依赖风场恰好出帧：持续采样按键并独立推进
  // 长按十字反馈，避免慢请求窗口内看起来“按住没有反应”。
  buttonService();
  pumpWindAnimation(true);
  updateLongPressCue();
}

static void serviceWindField() {
  if (!s_cfg.show_wind_particles || s_displayedZoom < ZOOM_MIN ||
      s_displayedZoom > ZOOM_MAX) {
    return;
  }
  windFieldSelect(s_cfg.lat, s_cfg.lon, s_displayedZoom);
  if (windFieldService()) {
    handlePendingZoom();
  }
}

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
  const bool wasFresh = frameCacheIsFresh(zoom);
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
    if (wasFresh && frameCacheHas(zoom)) {
      frameCacheMarkFresh(zoom, true);
    }
    s_statusScreen = false;
    refreshProgressRing();
    return false;
  }

  // display-only（无雷达 commit）不算造片成功，避免刷新时钟被空转推进
  if (!frameCacheHas(zoom)) {
    Serial.printf("build z%d not cached (display-only)\n", zoom);
    frameCacheRestoreStale(zoom);
    if (wasFresh && frameCacheHas(zoom)) {
      frameCacheMarkFresh(zoom, true);
    }
    if (pushToDisplay && frameCacheHas(zoom)) {
      s_displayedZoom = zoom;
      s_statusScreen = false;
      showCached(zoom);
    }
    refreshProgressRing();
    return false;
  }

  noteZoomRefreshed(zoom);

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
  if (windFieldBlocksPrefetch()) {
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
    noteUserInteraction();
    if (s_busyCompose) {
      composeRequestAbort();
    }
    zoomPrefetchClear();
  }

  zoomSetCurrent(zoom);

  // 已在屏上：不再 blit/呼吸，避免「跳了一下还是同一档」
  if (s_displayedZoom == zoom && frameCacheHas(zoom)) {
    if (!userInitiated) {
      zoomPrefetchResetAround(zoom);
    }
    return;
  }

  if (frameCacheHas(zoom)) {
    // 已缓存秒切：用户触发时先让后台预取让路；后台触发时才重建队列。
    if (!showCached(zoom)) {
      if (s_displayedZoom >= ZOOM_MIN && s_displayedZoom <= ZOOM_MAX) {
        zoomSetCurrent(s_displayedZoom);
      }
      return;
    }
    if (!userInitiated) {
      zoomPrefetchResetAround(zoom);
    }
    return;
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

static void handleLongPress() {
  const int target = zoomDefault();
  Serial.printf("long press -> default z%d (display was z%d)\n", target,
                s_displayedZoom);
  s_longCueSuppressUntilRelease = true;
  clearLongPressCue();
  ensureZoomVisible(target, true);
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
  Serial.printf("apply cfg: mode=%u ssid=%s lat=%.5f lon=%.5f defZoom=%d\n",
                (unsigned)s_cfg.wifi_mode, s_cfg.ssid, s_cfg.lat, s_cfg.lon,
                s_cfg.default_zoom);
  showStatus("Connecting...", s_cfg.ssid);
  s_wifiOk = wifiConnect(&s_cfg);
  if (!s_wifiOk) {
    showStatus("WiFi fail", s_cfg.ssid);
    scheduleWifiRetry();
    return false;
  }

  s_wifiRetryAt = 0;
  s_wifiRetryStage = 0;

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
  const bool oldCrosshair = s_cfg.show_crosshair;

  const PortalResult pr =
      configPortalRun(&lcd, CONFIG_PORTAL_TIMEOUT_MS, &s_cfg);

  Serial.printf("portal result=%u ssid=%s\n", (unsigned)pr, s_cfg.ssid);

  if (pr == PortalResult::Saved) {
    if (!nearlySameLoc(oldLat, oldLon, s_cfg.lat, s_cfg.lon)) {
      clearAllFrameCaches("location change");
    } else if (oldCrosshair != s_cfg.show_crosshair) {
      clearAllFrameCaches("crosshair setting change");
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

  windFieldSetEnabled(s_cfg.show_wind_particles);
  windParticlesReset();
  // 两种模式都保留全档静帧秒切；差异只在刷新调度。
  zoomSetPrefetchRadius(ZOOM_MAX - ZOOM_MIN);

  zoomSetDefault(s_cfg.default_zoom);
  composeSetCrosshairVisible(s_cfg.show_crosshair);
  zoomSetCurrent(zoomDefault());
  s_wifiRetryAt = 0;
  s_wifiRetryStage = 0;
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
  zoomSetDefault(MAP_ZOOM);
  zoomSetCurrent(MAP_ZOOM);
  zoomSetPendingFeedback(onPendingZoomFeedback);
  inputSetBlockingUiService(pumpWindDuringBlock);
  composeSetProgressFn(onComposeProgress);
  composeSetDisplayFn(onComposeDisplay);
  composeSetCrosshairVisible(s_cfg.show_crosshair);

  showStatus("LittleFS...", "");
  if (!frameCacheBegin()) {
    showStatus("FS fail", "no cache");
  }
  seedZoomRefreshTimesFromCache();
  windFieldBegin();
  windParticlesBegin();

  runPortalAndApply();
}

void loop() {
  static uint32_t lastBeat = 0;

  const ButtonEvent ev = buttonPoll();
  if (ev == ButtonEvent::ShortPress) {
    if (s_wifiOk) {
      handleShortPress();
    }
  } else if (ev == ButtonEvent::LongPress) {
    if (s_wifiOk) {
      handleLongPress();
    }
  }

  updateLongPressCue();

  if (!s_wifiOk) {
    if (s_wifiRetryAt != 0 &&
        (int32_t)(millis() - s_wifiRetryAt) >= 0) {
      Serial.println("WiFi background retry starting");
      s_wifiRetryAt = 0;
      tryWifiAndRadar();
    }
    delay(50);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi link lost; switching to background retry");
    s_wifiOk = false;
    s_wifiRetryStage = 0;
    s_wifiRetryAt = nonzeroMillis();
    delay(50);
    return;
  }

  // 风场首拉优先于邻档预取；已有旧场时请求期间动画继续播放。
  serviceWindField();
  pumpWindAnimation(false);
  scheduleWindStaticRefreshes();

  const bool due = refreshIsDue();
  if (!s_busyCompose && due) {
    const int z = zoomCurrent();
    if (zoomCanCompose(z)) {
      Serial.printf("scheduled refresh focus z%d (%s) free=%u\n", z,
                    s_refreshFailAt != 0 ? "retry" : "due",
                    (unsigned)fsFreeBytes());
      zoomPrefetchClear();
      // 先刷新当前档；成功后再按当前模式决定哪些邻档进入后台队列。
      rainviewerInvalidatePin();
      frameCacheSetProtectedZoom(s_displayedZoom >= 0 ? s_displayedZoom : z);
      const bool ok = buildAndCache(z, true);
      if (ok) {
        s_lastRefresh = millis();
        s_refreshFailAt = 0;
        uint32_t t = 0;
        frameCacheReadRadarTime(z, &t);
        showCached(z);
        if (s_cfg.show_wind_particles) {
          // 风场模式：其余档由 15/45 分钟分级计时器负责，旧帧保持 ready。
          s_staticFullPassDone =
              frameCacheCountFresh() >= frameCacheZoomSlots();
        } else {
          // 原模式：当前档成功后，其余全部进入同一轮更新。
          s_staticFullPassDone = false;
          frameCacheMarkAllStaleExcept(z);
        }
        refreshProgressRing();
        Serial.printf("refresh ok z%d radar_t=%lu mode=%s fresh=%d/%d "
                      "free=%u — prefetch others\n",
                      z, (unsigned long)t,
                      s_cfg.show_wind_particles ? "wind-tiered" : "full-pass",
                      frameCacheCountFresh(), frameCacheZoomSlots(),
                      (unsigned)fsFreeBytes());
        handlePendingZoom();
        zoomPrefetchResetAround(zoomCurrent());
      } else {
        s_refreshFailAt = millis() == 0 ? 1 : millis();
        zoomPrefetchClear();
        Serial.printf("refresh fail, retry in %lus free=%u\n",
                      (unsigned long)(RADAR_REFRESH_RETRY_MS / 1000UL),
                      (unsigned)fsFreeBytes());
        forceWifiReconnectAfterFetchFail("scheduled refresh failure");
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
    } else if (s_cfg.show_wind_particles) {
      refreshAge = zoomRefreshAgeMs(zoomCurrent()) / 1000UL;
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
