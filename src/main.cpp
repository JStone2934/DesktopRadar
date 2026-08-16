/**
 * ESP32-C3 桌面气象雷达 — SoftAP 配网 + RGB565 全档缓存
 * （静帧秒切 + 刷新期间保留旧帧可秒切）
 */

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
#include "ornament_media.h"
#include "ornament_portal.h"
#include "radar_font.h"
#include "rainviewer.h"
#include "update_manager.h"
#include "wifi_sta.h"
#include "wind_field.h"
#include "wind_particles.h"
#include "zoom_ctrl.h"

static LGFX lcd;
static AppConfig s_cfg;
static bool s_ornamentMode = false;

static int s_displayedZoom = -1;
static uint32_t s_lastRefresh = 0;
static uint32_t s_refreshFailAt = 0;  // 非 0：上次定时刷新失败，待短重试
static volatile bool s_busyCompose = false;
static bool s_wifiOk = false;
static uint32_t s_wifiRetryAt = 0;
static uint8_t s_wifiRetryStage = 0;
static bool s_statusScreen = false;  // 首次 Fetching 黑底，不 blit 地图
// 缺少任一缩放档时，保持黑底初始化画面，直到 ready=全档。
static bool s_initialCacheScreen = false;
static uint32_t s_initialPulseDrawAt = 0;
static int s_initialPulseLevel = -1;
static volatile int s_bakeZoom = -1;
static volatile float s_bakeLocal = 0.0f;

enum class RadarWorkKind : uint8_t {
  None = 0,
  Scheduled,
  Prefetch,
};

// 雷达下载/解码只在这个低优先级任务运行。UI 主任务永不等待它完成；
// S 键只设置 abort 并立即读取已有成品缓存。
static TaskHandle_t s_radarWorkerTask = nullptr;
static volatile bool s_radarWorkActive = false;
static volatile bool s_radarWorkDone = false;
static volatile int s_radarWorkZoom = -1;
static volatile RadarWorkKind s_radarWorkKind = RadarWorkKind::None;
static volatile ComposeResult s_radarWorkResult = ComposeResult::Failed;
static volatile bool s_radarUiPreempted = false;
// 全档静帧曾铺满一次后进度条保持满（隐藏）
static bool s_staticFullPassDone = false;
// 用户交互后暂停所有后台雷达造片（缓存显示本身不受影响）
static uint32_t s_cachePauseUntil = 0;
static uint32_t s_lastUserInteractionAt = 0;
// 连续缩放时不逐档读取风场缓存；静止片刻后只加载最终停留档。
static uint32_t s_windSelectNotBefore = 0;
// 风场模式后台静态造片的最早启动时刻；避免多个档位无缝连跑。
static uint32_t s_windBackgroundComposeAt = 0;
// 风场网络更新和雷达全量合成之间的隔离窗口，禁止两项重活首尾相接。
static uint32_t s_heavyWorkNotBefore = 0;
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
static bool serviceWindField(bool allowNetwork = true);
static uint32_t nonzeroMillis();
static void initialCacheScreenTick(bool force = false);
static void finishInitialCacheScreenIfReady();
static void serviceRadarWorkCompletion();
static void serviceRadarWorkUi();

static constexpr uint32_t kWifiRetryBackoffMs[] = {
    60UL * 1000UL, 3UL * 60UL * 1000UL, 10UL * 60UL * 1000UL};
// 人工连续点按常见间隔约 0.3--0.6 秒；800ms 可确保整轮只装最终档。
static constexpr uint32_t kWindSelectSettleAfterZoomMs = 800UL;
static constexpr uint32_t kHeavyWorkSeparationMs = 30UL * 1000UL;

static bool heavyWorkCooling() {
  return s_heavyWorkNotBefore != 0 &&
         (int32_t)(millis() - s_heavyWorkNotBefore) < 0;
}

static void separateNextHeavyWork() {
  s_heavyWorkNotBefore = millis() + kHeavyWorkSeparationMs;
  if (s_heavyWorkNotBefore == 0) {
    s_heavyWorkNotBefore = 1;
  }
}

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
  const uint32_t now = millis();
  s_lastUserInteractionAt = now == 0 ? 1 : now;
  const uint32_t pauseMs = s_cfg.show_wind_particles
                               ? WIND_CACHE_PAUSE_AFTER_USER_MS
                               : CACHE_PAUSE_AFTER_USER_MS;
  const uint32_t until = now + pauseMs;
  if (until > s_cachePauseUntil) {
    s_cachePauseUntil = until;
  }
  if (s_cfg.show_wind_particles) {
    s_windSelectNotBefore = now + kWindSelectSettleAfterZoomMs;
    if (s_windSelectNotBefore == 0) {
      s_windSelectNotBefore = 1;
    }
  }
}

static bool cachePaused() {
  if (s_initialCacheScreen) {
    return false;
  }
  // 松开后才产生短按事件；把实际按下电平也视为暂停，堵住“刚按下但
  // 后台任务抢先一步启动”的偶发竞态。
  return buttonIsDown() ||
         (int32_t)(millis() - s_cachePauseUntil) < 0;
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

/** 初始化只关心“可秒切”的 ready 档位，不把旧图时效纳入首次进度。 */
static float initialCacheDone01() {
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

static void drawInitialCacheStaticLabels() {
  lcd.setTextDatum(MC_DATUM);
  lcd.setFont(&radar_fonts::cn12);
  lcd.setTextSize(1.25f);
  lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  lcd.drawString("雷达数据加载中...", LCD_WIDTH / 2, 137);
}

static void initialCacheScreenTick(bool force) {
  if (!s_initialCacheScreen) {
    return;
  }
  const uint32_t now = millis();
  if (!force && (now - s_initialPulseDrawAt) < 33UL) {
    return;
  }

  // 2.2 秒一轮的平滑呼吸。再做一层时间域低通，模拟屏幕余辉，
  // 避免亮度量化在相邻帧间产生明显台阶。
  constexpr float kTwoPi = 6.28318530718f;
  const float phase = (float)(now % 2200UL) / 2200.0f;
  const float breath = 0.5f - 0.5f * cosf(kTwoPi * phase);
  const int targetLevel = 72 + (int)lroundf(183.0f * breath);
  int level = targetLevel;
  if (!force && s_initialPulseLevel >= 0) {
    const int delta = targetLevel - s_initialPulseLevel;
    level = s_initialPulseLevel + (int)lroundf((float)delta * 0.38f);
    if (level == s_initialPulseLevel && delta != 0) {
      level += delta > 0 ? 1 : -1;
    }
  }

  // 静态中文只在进页面时绘制一次；动画帧仅覆盖标题自身的字形框。
  // 不再先清空 184x76 的整块区域，消除黑底擦除造成的整体闪烁。
  lcd.setTextDatum(MC_DATUM);
  lcd.setTextSize(1.0f);
  lcd.setFont(&fonts::Font4);
  const uint16_t titleColor = lcd.color565(level, level, level);
  lcd.setTextColor(titleColor, TFT_BLACK);
  lcd.drawString("Storm Eye", LCD_WIDTH / 2, 101);

  progressRingUpdate(&lcd, initialCacheDone01(), -1);
  s_initialPulseDrawAt = now;
  s_initialPulseLevel = level;
}

static void beginInitialCacheScreen() {
  if (s_initialCacheScreen) {
    initialCacheScreenTick(true);
    return;
  }
  progressRingHide(&lcd, -1);
  alertRingHide(&lcd, s_displayedZoom);
  s_initialCacheScreen = true;
  inputSetLocked(true);
  s_statusScreen = true;
  s_displayedZoom = -1;
  frameCacheSetProtectedZoom(-1);
  zoomNoteDisplayed(-1);
  s_initialPulseDrawAt = 0;
  s_initialPulseLevel = -1;
  lcd.fillScreen(TFT_BLACK);
  drawInitialCacheStaticLabels();
  initialCacheScreenTick(true);
  Serial.printf("initial cache screen: ready=%d/%d\n", frameCacheCountReady(),
                frameCacheZoomSlots());
}

static void refreshProgressRing() {
  if (s_initialCacheScreen) {
    initialCacheScreenTick(false);
    return;
  }
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
  windParticlesNotifyBaseRedrawn();
  // compose 直接 push 分段，没有经过 frameCacheBlit 的同步环带采集。
  alertRingInvalidateUnderlay();
  applyAlertForZoom(zoom);
}

/** 造片/补满阻塞期间也推进预警呼吸，避免冻帧后跳变。 */
static void pumpAlertRingDuringCompose() {
  if (s_initialCacheScreen || !s_cfg.show_alert_ring ||
      !alertRingNeedsTick()) {
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

/** 首次初始化先补齐所有 ready 成品；完成前不进入按时效分级刷新。 */
static bool initialCacheBootstrapActive() {
  return s_initialCacheScreen &&
         frameCacheCountReady() < frameCacheZoomSlots();
}

static void onComposeProgress(int zoom, float local01) {
  s_bakeZoom = zoom;
  s_bakeLocal = local01;
  // 此回调来自低优先级造片任务，只发布进度，绝不在后台任务碰 LCD。
  // 抢占判断、进度条、预警环与按键全部由 UI 主循环推进。
}

static void serviceRadarWorkUi() {
  if (!s_radarWorkActive) {
    return;
  }
  updateLongPressCue();
  const int zoom = s_bakeZoom;
  // 全档已经 ready 后，当前档定时刷新才可抢占邻档刷新。初次补齐阶段
  // 必须让缺失档完成提交，否则五分钟定时器会让全档缓存长期补不满。
  if (s_radarWorkKind == RadarWorkKind::Prefetch &&
      zoom != zoomCurrent() && refreshIsDue() &&
      !initialCacheBootstrapActive()) {
    if (!composeAbortRequested()) {
      Serial.printf("abort prefetch z%d — refresh due\n", zoom);
      composeRequestAbort();
    }
  }
  pumpAlertRingDuringCompose();
  if (s_staticFullPassDone) {
    return;
  }
  static uint32_t s_lastRingMs = 0;
  const uint32_t now = millis();
  const float local01 = s_bakeLocal;
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
 * 风场模式只允许同时存在一个待更新静态档。切档保护期内完全不改变
 * fresh 状态；否则用户快速浏览会因“相对中心距离”变化把全档依次标旧，
 * 在保护期结束后制造一整串后台合成。
 */
static void scheduleWindStaticRefreshes() {
  if (!s_cfg.show_wind_particles || s_radarWorkActive || cachePaused() ||
      heavyWorkCooling()) {
    return;
  }
  // 当前档五分钟刷新到期或即将到期时，不再同时制造一个梯队 stale 档。
  // 当前档完成后再评估梯队，避免同一轮叠加两项后台工作。
  if (refreshIsDue() || refreshApproaching()) {
    return;
  }
  static uint32_t lastScanAt = 0;
  const uint32_t now = millis();
  if (lastScanAt != 0 && (now - lastScanAt) < 2000UL) {
    return;
  }
  lastScanAt = now;

  if ((int32_t)(now - s_windBackgroundComposeAt) < 0) {
    return;
  }

  // 已有一个 stale 档就等待它完成；不继续累积后台债务。ready 成品始终
  // 保留，所以即使 stale 也仍可用于秒切。
  const int slots = frameCacheZoomSlots();
  if (frameCacheCountReady() < slots || frameCacheCountFresh() < slots) {
    return;
  }

  const int center = zoomCurrent();
  int candidate = -1;
  // 以当前档为中心逐层找一个最值得刷新的档；每档仍只按自己的成功刷新
  // 时间判断到期，切换中心不会改变或清空其它档的状态。
  for (int delta = 1; delta <= ZOOM_MAX - ZOOM_MIN && candidate < 0;
       ++delta) {
    const int choices[2] = {center - delta, center + delta};
    for (int i = 0; i < 2; ++i) {
      const int z = choices[i];
      if (!zoomCanCompose(z) || !frameCacheHas(z)) {
        continue;
      }
      const uint32_t interval = delta == 1 ? WIND_ADJACENT_REFRESH_MS
                                            : WIND_FAR_REFRESH_MS;
      if (zoomRefreshDue(z, interval)) {
        candidate = z;
        break;
      }
    }
  }

  if (candidate >= 0) {
    frameCacheMarkFresh(candidate, false);
    s_staticFullPassDone = false;
    zoomPrefetchClear();
    // 队列控制器按当前、±1、±2顺序重建，但只有 candidate 为 stale，
    // 因此实际只会弹出这一档。
    zoomPrefetchResetAround(center);
    refreshProgressRing();
    Serial.printf("wind static tier: queue z%d center=z%d fresh=%d/%d\n",
                  candidate, center, frameCacheCountFresh(), slots);
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

static ComposeResult buildAndCacheBackground(int zoom);
static bool startRadarWork(int zoom, RadarWorkKind kind);
static bool startRadarWorker();
static void handlePendingZoom();
static bool pumpPrefetch();
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

/** 风场使用局部持久渐隐轨迹；阻塞路径传 busy=true，自动降至约 2 FPS。 */
static void pumpWindAnimation(bool busy) {
  static bool pumping = false;
  if (pumping || s_initialCacheScreen || !s_cfg.show_wind_particles ||
      s_statusScreen || s_displayedZoom < ZOOM_MIN ||
      s_displayedZoom > ZOOM_MAX) {
    return;
  }
  pumping = true;
  if (windParticlesTick(&lcd, s_displayedZoom, busy)) {
    progressRingRedraw(&lcd);
    if (s_cfg.show_alert_ring) {
      alertRingRedraw(&lcd, s_displayedZoom);
    }
    // 若长按状态机当前要求隐藏，继续维持该相位；不重置闪烁时钟。
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
  initialCacheScreenTick(false);
  // 不等松手判定短按/长按：物理按下先抢占耗时合成。松手后的事件
  // 仍按原状态机消费，因此不会改变短按/长按语义。
  if (s_busyCompose && !s_initialCacheScreen && buttonIsDown()) {
    composeRequestAbort();
  }
  // 一旦有按键抢占，当前调用只负责尽快退栈；不再补画一帧 30--40ms 的
  // 风场，避免秒切已经完成后还被同轮 SPI 工作拖住。
  if (composeAbortRequested() || zoomHasPending()) {
    updateLongPressCue();
    return;
  }
  pumpWindAnimation(true);
  updateLongPressCue();
}

static bool serviceWindField(bool allowNetwork) {
  if (s_initialCacheScreen || !s_cfg.show_wind_particles ||
      s_radarWorkActive ||
      s_displayedZoom < ZOOM_MIN || s_displayedZoom > ZOOM_MAX) {
    return false;
  }
  // 连续按 S 时只做静态缓存 blit；等按键停止片刻后，再为最终档读取一次
  // 风场缓存。否则每档约几十毫秒的 LittleFS 读取会叠在下一次按键前面。
  if (buttonIsDown() || zoomHasPending()) {
    return false;
  }
  if (s_windSelectNotBefore != 0 &&
      (int32_t)(millis() - s_windSelectNotBefore) < 0) {
    return false;
  }
  s_windSelectNotBefore = 0;
  // 装入最终档磁盘旧场后即可恢复粒子；静默期只拦截后面的 HTTPS 更新。
  // 当前档完全没有风场时绕过 60 秒静默期。
  windFieldSelect(s_cfg.lat, s_cfg.lon, s_displayedZoom);
  if (!allowNetwork) {
    return false;
  }
  if (heavyWorkCooling()) {
    return false;
  }
  if (cachePaused() && windFieldReadyFor(s_displayedZoom)) {
    return false;
  }
  if (windFieldService()) {
    separateNextHeavyWork();
    handlePendingZoom();
    return true;
  }
  return false;
}

static bool showCached(int zoom) {
  if (s_initialCacheScreen) {
    return false;
  }
  const uint32_t t0 = millis();
  if (!frameCacheBlit(&lcd, zoom)) {
    Serial.printf("showCached z%d blit fail\n", zoom);
    return false;
  }
  windParticlesNotifyBaseRedrawn();
  const uint32_t blitMs = millis() - t0;
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
  const uint32_t totalMs = millis() - t0;
  Serial.printf("showCached z%d total=%lums blit=%lums overlay=%lums\n", zoom,
                (unsigned long)totalMs, (unsigned long)blitMs,
                (unsigned long)(totalMs - blitMs));
  return true;
}

static void finishInitialCacheScreenIfReady() {
  if (!s_initialCacheScreen ||
      frameCacheCountReady() < frameCacheZoomSlots()) {
    return;
  }

  s_initialCacheScreen = false;
  inputSetLocked(false);
  s_staticFullPassDone = true;
  s_statusScreen = false;
  s_initialPulseLevel = -1;
  progressRingHide(&lcd, -1);

  const int target = zoomDefault();
  zoomSetCurrent(target);
  if (!showCached(target)) {
    showStatus("Cache ready", "display fail");
    Serial.printf("initial cache complete but z%d blit failed\n", target);
    return;
  }
  zoomPrefetchResetAround(target);
  Serial.printf("initial cache complete: ready=%d/%d display=z%d\n",
                frameCacheCountReady(), frameCacheZoomSlots(), target);
}

/** 造片阻塞中短按：已缓存则立刻秒切；未缓存不碰预警环（避免地图未变却像切了档）。 */
static void onPendingZoomFeedback(int zoom) {
  if (s_initialCacheScreen) {
    zoomClearPendingIf(zoom);
    zoomSetCurrent(zoomDefault());
    composeClearAbort();
    Serial.println("input ignored during initial full-cache build");
    return;
  }
  noteUserInteraction();
  if (!frameCacheHas(zoom)) {
    // S 键路径绝不等待网络造片。当前后台任务立即让路；缺失档留给之后的
    // 低优先级预取，屏幕和逻辑档保持在最后一个真实可见缓存。
    if (s_radarWorkActive) {
      s_radarUiPreempted = true;
      composeRequestAbort();
    }
    zoomClearPendingIf(zoom);
    if (s_displayedZoom >= ZOOM_MIN && s_displayedZoom <= ZOOM_MAX) {
      zoomSetCurrent(s_displayedZoom);
    }
    Serial.printf("pending z%d unavailable — keep visible z%d\n", zoom,
                  s_displayedZoom);
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

static ComposeResult buildAndCacheBackground(int zoom) {
  if (!zoomCanCompose(zoom)) {
    return ComposeResult::Failed;
  }
  // 静态造片完全依赖在线瓦片。离线时立即失败，避免一次按键被每张瓦片的
  // DNS/TLS 超时拖住几十秒；旧的 ready 帧始终留在屏上。
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("build z%d skipped: WiFi offline (status=%d)\n", zoom,
                  (int)WiFi.status());
    return ComposeResult::Failed;
  }
  const bool wasFresh = frameCacheIsFresh(zoom);
  Serial.printf("background bake z%d (display isolated)\n", zoom);

  const ComposeResult result =
      composeRadarFrame(&lcd, s_cfg.lat, s_cfg.lon, zoom, false);

  if (result == ComposeResult::Failed) {
    if (composeAbortRequested()) {
      Serial.printf("build z%d aborted\n", zoom);
    } else {
      Serial.printf("build z%d fail\n", zoom);
    }
    frameCacheRestoreStale(zoom);
    if (wasFresh && frameCacheHas(zoom)) {
      frameCacheMarkFresh(zoom, true);
    }
    return ComposeResult::Failed;
  }

  if (result == ComposeResult::Unchanged) {
    frameCacheMarkFresh(zoom, true);
    noteZoomRefreshed(zoom);
    Serial.printf("build z%d unchanged; mark fresh without blit\n", zoom);
    return ComposeResult::Unchanged;
  }

  // 无雷达 commit 不算造片成功，避免刷新时钟被空转推进。
  if (!frameCacheHas(zoom)) {
    Serial.printf("build z%d not cached\n", zoom);
    frameCacheRestoreStale(zoom);
    if (wasFresh && frameCacheHas(zoom)) {
      frameCacheMarkFresh(zoom, true);
    }
    return ComposeResult::Failed;
  }

  noteZoomRefreshed(zoom);
  return ComposeResult::Updated;
}

static void radarWorkerMain(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const int zoom = s_radarWorkZoom;
    const ComposeResult result = buildAndCacheBackground(zoom);
    s_radarWorkResult = result;
    s_busyCompose = false;
    s_radarWorkDone = true;
  }
}

static bool startRadarWorker() {
  if (s_radarWorkerTask) {
    return true;
  }
  const BaseType_t created =
      xTaskCreate(radarWorkerMain, "radar-bake", 7168, nullptr,
                  tskIDLE_PRIORITY, &s_radarWorkerTask);
  if (created != pdPASS) {
    s_radarWorkerTask = nullptr;
    Serial.println("radar worker alloc fail; background refresh disabled");
    return false;
  }
  Serial.println("radar worker ready: priority=idle stack=7168");
  return true;
}

static void stopRadarWorker() {
  if (!s_radarWorkerTask) return;
  if (s_radarWorkActive) {
    composeRequestAbort();
    const uint32_t deadline = millis() + 3000UL;
    while (s_radarWorkActive && (int32_t)(millis() - deadline) < 0) {
      delay(10);
    }
  }
  vTaskDelete(s_radarWorkerTask);
  s_radarWorkerTask = nullptr;
  s_radarWorkActive = false;
  s_radarWorkDone = false;
  Serial.println("radar worker stopped for ornament mode");
}

static bool startRadarWork(int zoom, RadarWorkKind kind) {
  if (!s_radarWorkerTask || s_radarWorkActive || !zoomCanCompose(zoom) ||
      WiFi.status() != WL_CONNECTED) {
    return false;
  }
  composeClearAbort();
  s_bakeZoom = zoom;
  s_bakeLocal = 0.0f;
  s_radarWorkZoom = zoom;
  s_radarWorkKind = kind;
  s_radarWorkResult = ComposeResult::Failed;
  s_radarUiPreempted = false;
  s_radarWorkDone = false;
  s_busyCompose = true;
  s_radarWorkActive = true;
  xTaskNotifyGive(s_radarWorkerTask);
  Serial.printf("radar work start kind=%u z%d\n", (unsigned)kind, zoom);
  return true;
}

static void pumpBackgroundPrefetch() {
  if (updateManagerCheckPending()) {
    return;
  }
  if (zoomHasPending()) {
    handlePendingZoom();
    return;
  }
  if (cachePaused()) {
    return;
  }
  if (windFieldBlocksPrefetch() || heavyWorkCooling()) {
    return;
  }
  const bool bootstrap = initialCacheBootstrapActive();
  // 补齐全档是风场模式的第一阶段；当前档的定时刷新只能在 ready=全档
  // 后抢占。普通模式及梯队刷新阶段维持原来的时序。
  if (!bootstrap && (refreshIsDue() || refreshApproaching())) {
    return;
  }
  if (s_radarWorkActive || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (s_cfg.show_wind_particles && !bootstrap &&
      (int32_t)(millis() - s_windBackgroundComposeAt) < 0) {
    return;
  }

  const int slots = frameCacheZoomSlots();
  const int freshBefore = frameCacheCountFresh();
  if (freshBefore >= slots) {
    s_staticFullPassDone = true;
    finishInitialCacheScreenIfReady();
    return;
  }

  if (pumpPrefetch()) {
    return;
  }
  finishInitialCacheScreenIfReady();
  if (bootstrap && !s_initialCacheScreen) {
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
    if (s_radarWorkActive) {
      s_radarUiPreempted = true;
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

  // S 键只切已有缓存，永远不在交互路径启动网络任务。缺失档标为待补，
  // 恢复屏上逻辑档；后台调度器会在交互保护期结束后再尝试生成。
  frameCacheMarkFresh(zoom, false);
  Serial.printf("z%d unavailable (%s) — keep display z%d and defer bake\n",
                zoom, WiFi.status() == WL_CONNECTED ? "online" : "offline",
                s_displayedZoom);
  if (s_displayedZoom >= ZOOM_MIN && s_displayedZoom <= ZOOM_MAX) {
    zoomSetCurrent(s_displayedZoom);
  }
  zoomPrefetchResetAround(zoomCurrent());
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

static bool pumpPrefetch() {
  if (zoomHasPending()) {
    handlePendingZoom();
    return false;
  }
  if (s_radarWorkActive || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  int z = 0;
  const int repair = frameCacheTakeRepairZoom();
  if (repair >= 0) {
    Serial.printf("priority cache repair z%d\n", repair);
    return startRadarWork(repair, RadarWorkKind::Prefetch);
  }
  if (!zoomPrefetchPop(&z)) {
    return false;
  }
  if (frameCacheIsFresh(z)) {
    return false;
  }
  return startRadarWork(z, RadarWorkKind::Prefetch);
}

static void serviceRadarWorkCompletion() {
  if (!s_radarWorkActive || !s_radarWorkDone) {
    return;
  }

  const int zoom = s_radarWorkZoom;
  const RadarWorkKind kind = s_radarWorkKind;
  const ComposeResult result = s_radarWorkResult;
  s_radarWorkDone = false;
  s_radarWorkActive = false;
  s_radarWorkKind = RadarWorkKind::None;
  s_radarWorkZoom = -1;
  s_bakeZoom = -1;
  s_bakeLocal = 0.0f;

  const bool aborted = composeAbortRequested();
  const bool uiInterrupted =
      s_radarUiPreempted || buttonIsDown() || buttonPriorityRequested() ||
      zoomHasPending() ||
      (kind == RadarWorkKind::Scheduled &&
       (zoomCurrent() != zoom ||
        (s_displayedZoom >= ZOOM_MIN && s_displayedZoom != zoom)));
  s_radarUiPreempted = false;

  if (kind == RadarWorkKind::Prefetch) {
    if (result != ComposeResult::Failed) {
      zoomPrefetchNoteOk(zoom);
    } else if (!aborted) {
      zoomPrefetchNoteFail(zoom);
    }

    if (uiInterrupted && !s_initialCacheScreen) {
      noteUserInteraction();
      Serial.printf("background prefetch z%d preempted by UI\n", zoom);
    } else if (s_cfg.show_wind_particles && !s_initialCacheScreen) {
      s_windBackgroundComposeAt = millis() + WIND_BACKGROUND_COMPOSE_GAP_MS;
      Serial.printf("wind static cooldown %lus; interaction remains primary\n",
                    (unsigned long)(WIND_BACKGROUND_COMPOSE_GAP_MS / 1000UL));
    }

    if (frameCacheCountFresh() >= frameCacheZoomSlots()) {
      s_staticFullPassDone = true;
    }
    finishInitialCacheScreenIfReady();
    refreshProgressRing();
    if (!s_initialCacheScreen && uiInterrupted) {
      zoomPrefetchClear();
    } else if (result == ComposeResult::Failed) {
      zoomPrefetchResetAround(zoomCurrent());
    }
  } else if (kind == RadarWorkKind::Scheduled) {
    separateNextHeavyWork();
    if (result != ComposeResult::Failed) {
      s_lastRefresh = millis();
      s_refreshFailAt = 0;
      uint32_t radarTime = 0;
      frameCacheReadRadarTime(zoom, &radarTime);
      if (result == ComposeResult::Updated && zoomCurrent() == zoom &&
          s_displayedZoom == zoom) {
        showCached(zoom);
      }
      if (s_cfg.show_wind_particles) {
        s_windBackgroundComposeAt =
            millis() + WIND_BACKGROUND_COMPOSE_GAP_MS;
        s_staticFullPassDone =
            frameCacheCountFresh() >= frameCacheZoomSlots();
      } else {
        s_staticFullPassDone = false;
        frameCacheMarkAllStaleExcept(zoom);
      }
      refreshProgressRing();
      Serial.printf("refresh ok z%d radar_t=%lu mode=%s fresh=%d/%d free=%u\n",
                    zoom, (unsigned long)radarTime,
                    s_cfg.show_wind_particles ? "wind-tiered" : "full-pass",
                    frameCacheCountFresh(), frameCacheZoomSlots(),
                    (unsigned)fsFreeBytes());
      zoomPrefetchResetAround(zoomCurrent());
    } else if (uiInterrupted) {
      // 所有模式都把 S 抢占视为正常调度，不断 Wi-Fi、不进入失败重试。
      s_refreshFailAt = 0;
      zoomPrefetchClear();
      noteUserInteraction();
      Serial.printf("scheduled refresh z%d preempted by UI; keep WiFi/cache\n",
                    zoom);
      zoomPrefetchResetAround(zoomCurrent());
    } else {
      s_refreshFailAt = nonzeroMillis();
      zoomPrefetchClear();
      Serial.printf("refresh fail, retry in %lus free=%u\n",
                    (unsigned long)(RADAR_REFRESH_RETRY_MS / 1000UL),
                    (unsigned)fsFreeBytes());
      forceWifiReconnectAfterFetchFail("scheduled refresh failure");
    }
  }

  handlePendingZoom();
}

static bool tryWifiAndRadar() {
  Serial.printf("apply cfg: mode=%u ssid=%s lat=%.5f lon=%.5f defZoom=%d\n",
                (unsigned)s_cfg.wifi_mode, s_cfg.ssid, s_cfg.lat, s_cfg.lon,
                s_cfg.default_zoom);
  const bool recoveringInitialCache = s_initialCacheScreen;
  const bool hasVisibleCacheNow =
      !s_statusScreen && s_displayedZoom >= ZOOM_MIN &&
      s_displayedZoom <= ZOOM_MAX && frameCacheHas(s_displayedZoom);
  if (!recoveringInitialCache && !hasVisibleCacheNow) {
    showStatus("Connecting...", s_cfg.ssid);
  }
  s_wifiOk = wifiConnect(&s_cfg);
  if (!s_wifiOk) {
    if (recoveringInitialCache) {
      initialCacheScreenTick(true);
    } else if (!(s_displayedZoom >= ZOOM_MIN &&
                 s_displayedZoom <= ZOOM_MAX &&
                 frameCacheHas(s_displayedZoom) && !s_statusScreen)) {
      showStatus("WiFi fail", s_cfg.ssid);
    }
    scheduleWifiRetry();
    return false;
  }

  s_wifiRetryAt = 0;
  s_wifiRetryStage = 0;

  const bool visibleAfterConnect =
      !s_statusScreen && s_displayedZoom >= ZOOM_MIN &&
      s_displayedZoom <= ZOOM_MAX && frameCacheHas(s_displayedZoom);
  if (!recoveringInitialCache && !visibleAfterConnect) {
    char ipBuf[24];
    snprintf(ipBuf, sizeof(ipBuf), "%s", WiFi.localIP().toString().c_str());
    showStatus("WiFi OK", ipBuf);
    delay(600);
  }

  if (frameCacheCountReady() < frameCacheZoomSlots()) {
    zoomSetCurrent(zoomDefault());
    beginInitialCacheScreen();
    s_cachePauseUntil = 0;
    zoomPrefetchResetAround(zoomCurrent());
  } else if (s_initialCacheScreen) {
    finishInitialCacheScreenIfReady();
  } else {
    ensureZoomVisible(zoomCurrent(), true);
    if (!frameCacheHas(zoomCurrent())) {
      Serial.printf("compose miss z%d — will retry in loop\n", zoomCurrent());
    } else {
      zoomPrefetchResetAround(zoomCurrent());
    }
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

  if (pr == PortalResult::UpdateRequested) {
    if (!updateManagerExecuteConfirmed(&lcd, &s_cfg)) {
      showStatus("Update cancelled", "See Web details");
      delay(1200);
      if (updateManagerConsumeFilesystemReset()) {
        frameCacheBegin();
        seedZoomRefreshTimesFromCache();
      }
    }
  }

  if (pr == PortalResult::Saved) {
    if (s_cfg.display_mode == DISPLAY_MODE_ORNAMENT) {
      showStatus("Storm Eye", "Starting ornament mode");
      clearAllFrameCaches("enter ornament mode");
      windFieldClearCache();
      appConfigSetDisplayMode(DISPLAY_MODE_ORNAMENT, true);
      delay(700);
      ESP.restart();
      return;
    }
    if (!nearlySameLoc(oldLat, oldLon, s_cfg.lat, s_cfg.lon)) {
      clearAllFrameCaches("location change");
    } else if (oldCrosshair != s_cfg.show_crosshair) {
      clearAllFrameCaches("crosshair setting change");
    }
  }

  // 用户在摆件上传区准备了媒体、但最终保留雷达模式或按 S 跳过时，
  // 立即释放媒体空间，避免挤占全档雷达缓存。
  if (s_cfg.display_mode == DISPLAY_MODE_RADAR) ornamentMediaClearAll();

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
  windParticlesSetStyle(s_cfg.wind_particle_style);
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

static bool ornamentBootGuide() {
  const PortalResult result =
      configPortalRun(&lcd, CONFIG_PORTAL_TIMEOUT_MS, &s_cfg);
  Serial.printf("ornament boot guide result=%u display=%u\n",
                (unsigned)result, (unsigned)s_cfg.display_mode);

  if (result == PortalResult::UpdateRequested) {
    if (!updateManagerExecuteConfirmed(&lcd, &s_cfg)) {
      showStatus("Update cancelled", "Returning to ornament");
      delay(900);
      if (updateManagerConsumeFilesystemReset()) frameCacheBegin();
    }
  }

  if (result == PortalResult::Saved &&
      s_cfg.display_mode == DISPLAY_MODE_RADAR) {
    ornamentMediaClearAll();
    appConfigSetDisplayMode(DISPLAY_MODE_RADAR, true);
    showStatus("Storm Eye", "Starting radar mode");
    delay(700);
    ESP.restart();
    return false;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  // USB CDC 主机存在但没有程序持续读取时，Arduino-ESP32 默认一次写入
  // 最多等待约 20 x 100ms。状态/风场日志因此可能周期性冻结 UI 数秒。
  // 日志只用于诊断：发送队列满就丢弃，绝不能反向阻塞 S 键和屏幕。
  Serial.setTxTimeoutMs(0);
  delay(400);
  Serial.println();
  Serial.println("ESP32-C3 Radar SoftAP config + RGB565 zoom");

  lcd.init();
  lcd.setRotation(0);
  lcd.setBrightness(255);

  buttonBegin();
  inputBindUiTaskToCurrent();
  zoomSetDefault(MAP_ZOOM);
  zoomSetCurrent(MAP_ZOOM);
  zoomSetPendingFeedback(onPendingZoomFeedback);
  inputSetBlockingUiService(pumpWindDuringBlock);
  composeSetProgressFn(onComposeProgress);
  composeSetDisplayFn(onComposeDisplay);
  composeSetCrosshairVisible(s_cfg.show_crosshair);

  showStatus("Storm Eye", "");
  updateManagerBegin();
  const bool filesystemReady = frameCacheBegin();
  if (!filesystemReady) {
    showStatus("FS fail", "no cache");
  }
  const bool workerReady = startRadarWorker();
  if (!updateManagerConfirmFirstBoot(&lcd, filesystemReady, workerReady)) {
    return;
  }
  if (!appConfigLoad(&s_cfg)) {
    appConfigSetDefaults(&s_cfg);
  }
  updateManagerHandleBootResume(&lcd, &s_cfg);
  const bool directBoot = appConfigConsumeDirectBootOnce();
  const esp_reset_reason_t resetReason = esp_reset_reason();
  // ESP.restart() 用于媒体播放器重启、模式切换和升级恢复，这些路径应直启；
  // RST/重新上电等其它复位则进入一次配置引导。
  const bool physicalBoot = resetReason != ESP_RST_SW;
  Serial.printf("boot reset reason=%u direct=%d physical=%d\n",
                (unsigned)resetReason, (int)directBoot, (int)physicalBoot);

  if (s_cfg.display_mode == DISPLAY_MODE_ORNAMENT) {
    s_ornamentMode = true;
    stopRadarWorker();
    frameCacheClearAll();
    windFieldClearCache();
    if (!directBoot && physicalBoot && !ornamentBootGuide()) return;
    ornamentMediaBegin(&lcd);
    if (!ornamentPortalBegin(&lcd, &s_cfg)) {
      showStatus("Hotspot fail", "Restart device");
    }
    Serial.printf("ornament runtime ready media=%d heap=%u\n",
                  (int)ornamentMediaValid(), ESP.getFreeHeap());
    return;
  }

  ornamentMediaClearAll();
  seedZoomRefreshTimesFromCache();
  windFieldBegin();
  windParticlesSetStyle(s_cfg.wind_particle_style);
  windParticlesBegin();

  if (directBoot) {
    Serial.println("direct boot into radar mode");
    windFieldSetEnabled(s_cfg.show_wind_particles);
    windParticlesSetStyle(s_cfg.wind_particle_style);
    zoomSetPrefetchRadius(ZOOM_MAX - ZOOM_MIN);
    zoomSetDefault(s_cfg.default_zoom);
    composeSetCrosshairVisible(s_cfg.show_crosshair);
    zoomSetCurrent(zoomDefault());
    tryWifiAndRadar();
  } else {
    runPortalAndApply();
  }
}

void loop() {
  static uint32_t lastBeat = 0;

  if (s_ornamentMode) {
    const ButtonEvent mediaEvent = buttonPoll();
    if (mediaEvent == ButtonEvent::ShortPress) {
      const bool paused = ornamentMediaTogglePause();
      Serial.printf("ornament GIF paused=%d\n", (int)paused);
    } else if (mediaEvent == ButtonEvent::LongPress) {
      ornamentMediaStop();
      ornamentPortalEnd();
      delay(80);
      ESP.restart();
      return;
    }
    ornamentPortalService();
    if (!ornamentPortalBusy()) {
      ornamentMediaService();
    }
    const OrnamentPortalAction action = ornamentPortalTakeAction();
    if (action == OrnamentPortalAction::RestartRadar) {
      ornamentMediaStop();
      ornamentPortalEnd();
      delay(80);
      ESP.restart();
      return;
    }
    if (action == OrnamentPortalAction::UpdateRequested) {
      ornamentMediaStop();
      ornamentPortalEnd();
      if (!updateManagerExecuteConfirmed(&lcd, &s_cfg)) {
        delay(600);
        ESP.restart();
      }
      return;
    }
    if (millis() - lastBeat >= 5000) {
      lastBeat = millis();
      Serial.printf("[%lu] ornament heap=%u max=%u stations=%d media=%d busy=%d\n",
                    millis() / 1000UL, ESP.getFreeHeap(),
                    ESP.getMaxAllocHeap(), WiFi.softAPgetStationNum(),
                    (int)ornamentMediaValid(), (int)ornamentPortalBusy());
    }
    delay(2);
    return;
  }

  initialCacheScreenTick(false);
  const ButtonEvent ev = buttonPoll();
  if (!s_initialCacheScreen && ev == ButtonEvent::ShortPress) {
    handleShortPress();
  } else if (!s_initialCacheScreen && ev == ButtonEvent::LongPress) {
    handleLongPress();
  }

  serviceRadarWorkCompletion();
  serviceRadarWorkUi();

  // 阻塞路径为抢占而锁存的 abort 只服务当前任务；任务已经回到主循环、
  // pending 也已消费后立即释放，不能误伤后续风场 HTTP 请求。
  if (!s_radarWorkActive && !zoomHasPending() && !buttonIsDown()) {
    composeClearAbort();
  }

  updateLongPressCue();

  if (!s_wifiOk) {
    if (s_radarWorkActive) {
      composeRequestAbort();
      delay(10);
      return;
    }
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
    if (s_radarWorkActive) {
      composeRequestAbort();
    }
    delay(50);
    return;
  }

  const uint32_t updateIdleAge =
      s_lastUserInteractionAt == 0 ? millis()
                                   : millis() - s_lastUserInteractionAt;
  const bool updateCheckSafeToStart =
      !s_radarWorkActive && !buttonIsDown() && !zoomHasPending();
  const bool updateUserIdle = updateIdleAge >= 10000UL;
  updateManagerServiceDailyCheck(updateCheckSafeToStart, updateUserIdle);
  if (updateManagerBusy()) {
    pumpWindAnimation(false);
    delay(10);
    return;
  }
  const bool updateCheckPending = updateManagerCheckPending();

  // 风场首拉优先于邻档预取；已有旧场时请求期间动画继续播放。
  const bool windWorked = serviceWindField(!updateCheckPending);
  pumpWindAnimation(false);
  if (!windWorked && !updateCheckPending) {
    scheduleWindStaticRefreshes();
  }

  const bool due = refreshIsDue();
  // 首次全档未完成时禁止定时刷新抢占补档；其余时候所有模式都遵守
  // 用户交互保护，避免松手后的同一轮马上启动后台重活。
  const bool deferScheduledRefresh =
      initialCacheBootstrapActive() || cachePaused();
  if (!updateCheckPending && !s_radarWorkActive && !windWorked &&
      !heavyWorkCooling() && due && !deferScheduledRefresh) {
    const int z = zoomCurrent();
    if (zoomCanCompose(z)) {
      Serial.printf("scheduled refresh focus z%d (%s) free=%u\n", z,
                    s_refreshFailAt != 0 ? "retry" : "due",
                    (unsigned)fsFreeBytes());
      zoomPrefetchClear();
      // 先刷新当前档；成功后再按当前模式决定哪些邻档进入后台队列。
      rainviewerInvalidatePin();
      frameCacheSetProtectedZoom(s_displayedZoom >= 0 ? s_displayedZoom : z);
      // 所有模式都只后台写缓存，绝不在刷新中触碰当前屏幕。
      startRadarWork(z, RadarWorkKind::Scheduled);
    }
  }

  if (!s_radarWorkActive && !frameCacheHas(zoomCurrent()) &&
      zoomCanCompose(zoomCurrent())) {
    static uint32_t lastUncachedRetry = 0;
    if (millis() - lastUncachedRetry >= 15000) {
      lastUncachedRetry = millis();
      Serial.printf("retry uncached z%d\n", zoomCurrent());
      frameCacheMarkFresh(zoomCurrent(), false);
      zoomPrefetchResetAround(zoomCurrent());
    }
  }

  // 预警呼吸优先于预取/past 补满，避免秒切后迟迟不闪、环动画卡顿
  const bool alertBreathing =
      !s_initialCacheScreen && s_cfg.show_alert_ring && alertRingNeedsTick();
  if (alertBreathing) {
    const int under = s_statusScreen ? -1 : s_displayedZoom;
    alertRingTick(&lcd, under);
  }

  // 呼吸未结束时不启动阻塞式预取/补满（否则会冻住数秒再跳变）
  if (!s_radarWorkActive && !alertBreathing) {
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
