#include "alert_ring.h"

#include <math.h>

#include "config.h"
#include "frame_cache.h"

/** 一轮呼吸（淡入+淡出）合计 1s。 */
static constexpr uint32_t kFadeInMs = 500;
static constexpr uint32_t kFadeOutMs = 500;
/** 淡入→淡出完整脉冲次数，之后再淡入并常显。 */
static constexpr int kPulseCycles = 2;
static constexpr uint32_t kPulseMs = kFadeInMs + kFadeOutMs;
static constexpr uint32_t kAnimUntilHoldMs =
    (uint32_t)kPulseCycles * kPulseMs + kFadeInMs;
/** 呼吸帧间隔：过密的 writePixel 会拖慢主循环、拖住短按响应 */
static constexpr uint32_t kDrawMinMs = 80;

/** 环带约 π(Ro²−Ri²)≈2200px，留余量。 */
static constexpr int kRingCap = 2800;

static bool s_active = false;
static bool s_visible = false;
static uint16_t s_color = 0;
static uint32_t s_fadeStartMs = 0;
static float s_lastFade = -1.0f;
static uint32_t s_lastDrawMs = 0;

static int s_underZoom = -1;
static int s_underCount = 0;
static uint16_t s_underPix[kRingCap];
static uint8_t s_underX[kRingCap];
static uint8_t s_underY[kRingCap];
static bool s_captureActive = false;

static inline int expand5(int v) { return (v << 3) | (v >> 2); }
static inline int expand6(int v) { return (v << 2) | (v >> 4); }

static inline uint16_t pack565(int r8, int g8, int b8) {
  if (r8 < 0) {
    r8 = 0;
  } else if (r8 > 255) {
    r8 = 255;
  }
  if (g8 < 0) {
    g8 = 0;
  } else if (g8 > 255) {
    g8 = 255;
  }
  if (b8 < 0) {
    b8 = 0;
  } else if (b8 > 255) {
    b8 = 255;
  }
  return (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
}

/** 8-bit 域逐通道 alpha 混合，与 frame_cache.cpp 保持一致。
 *  旧的打包近似法 R 通道乘法溢出到 G 位，导致呼吸时颜色串色闪烁。 */
static inline uint16_t blend565(uint16_t src, uint16_t dst, uint8_t a) {
  if (a == 0) {
    return dst;
  }
  if (a >= 255) {
    return src;
  }
  const int inv = 255 - a;
  const int r = (expand5((src >> 11) & 0x1F) * a +
                 expand5((dst >> 11) & 0x1F) * inv + 127) /
                255;
  const int g = (expand6((src >> 5) & 0x3F) * a +
                 expand6((dst >> 5) & 0x3F) * inv + 127) /
                255;
  const int b =
      (expand5(src & 0x1F) * a + expand5(dst & 0x1F) * inv + 127) / 255;
  return pack565(r, g, b);
}

/**
 * 时间线：
 *   [脉冲×2] 0.5s 淡入 → 0.5s 淡出（一轮 1s），重复 2 次
 *   [收尾]   0.5s 淡入 → 保持不透明
 */
static float currentFade01() {
  if (!s_active) {
    return 0.0f;
  }
  const uint32_t elapsed = millis() - s_fadeStartMs;
  if (elapsed >= kAnimUntilHoldMs) {
    return 1.0f;
  }

  const uint32_t pulseSpan = (uint32_t)kPulseCycles * kPulseMs;
  if (elapsed >= pulseSpan) {
    const uint32_t t = elapsed - pulseSpan;
    return (float)t / (float)kFadeInMs;
  }

  const uint32_t inCycle = elapsed % kPulseMs;
  if (inCycle < kFadeInMs) {
    return (float)inCycle / (float)kFadeInMs;
  }
  const uint32_t outT = inCycle - kFadeInMs;
  return 1.0f - (float)outT / (float)kFadeOutMs;
}

static bool animationTimeDone() {
  if (!s_active) {
    return true;
  }
  return (millis() - s_fadeStartMs) >= kAnimUntilHoldMs;
}

static void paintFullRing(LGFX* lcd, uint16_t color) {
  if (!lcd || color == 0) {
    return;
  }
  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  const int rOuter = (LCD_WIDTH / 2) - 1;
  const int rInner = rOuter - 3;
  lcd->fillArc(cx, cy, rOuter, rInner, 0.0f, 360.0f, color);
}

static void clearUnderCache() {
  s_underZoom = -1;
  s_underCount = 0;
  s_captureActive = false;
}

void alertRingInvalidateUnderlay() { clearUnderCache(); }

void alertRingCaptureUnderlayBegin(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    clearUnderCache();
    return;
  }
  s_underZoom = zoom;
  s_underCount = 0;
  s_captureActive = true;
}

void alertRingCaptureUnderlayBand(int zoom, int bandY, int bandRows,
                                  const uint16_t* pixels) {
  if (!s_captureActive || zoom != s_underZoom || !pixels || bandRows <= 0) {
    return;
  }
  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  const int rOuter = (LCD_WIDTH / 2) - 1;
  const int rInner = rOuter - 3;
  const int rO2 = rOuter * rOuter;
  const int rI2 = rInner * rInner;
  for (int by = 0; by < bandRows; ++by) {
    const int y = bandY + by;
    if (y < 0 || y >= LCD_HEIGHT) {
      continue;
    }
    const int dy = y - cy;
    const int dy2 = dy * dy;
    if (dy2 > rO2) {
      continue;
    }
    const uint16_t* row = pixels + (size_t)by * LCD_WIDTH;
    for (int x = 0; x < LCD_WIDTH; ++x) {
      const int dx = x - cx;
      const int d2 = dx * dx + dy2;
      if (d2 > rO2 || d2 < rI2) {
        continue;
      }
      if (s_underCount >= kRingCap) {
        clearUnderCache();
        return;
      }
      s_underPix[s_underCount] = row[x];
      s_underX[s_underCount] = (uint8_t)x;
      s_underY[s_underCount] = (uint8_t)y;
      ++s_underCount;
    }
  }
}

void alertRingCaptureUnderlayEnd(int zoom, bool complete) {
  if (!s_captureActive) {
    return;
  }
  s_captureActive = false;
  if (!complete || zoom != s_underZoom || s_underCount <= 0) {
    clearUnderCache();
  }
}

static bool ensureUnderCache(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    clearUnderCache();
    return false;
  }
  if (s_underZoom == zoom && s_underCount > 0) {
    return true;
  }
  int n = 0;
  if (!frameCacheSampleAlertRing(zoom, s_underPix, s_underX, s_underY, kRingCap,
                                 &n) ||
      n <= 0) {
    clearUnderCache();
    return false;
  }
  s_underCount = n;
  s_underZoom = zoom;
  return true;
}

/** 用缓存底图像素按 alpha 画环；透明时露出地图。 */
static bool paintCachedFaded(LGFX* lcd, uint8_t alpha) {
  if (!lcd || s_underCount <= 0) {
    return false;
  }
  if (alpha >= 255) {
    paintFullRing(lcd, s_color);
    return true;
  }

  // 按行聚合连续段 pushImage（比逐点 writePixel 快）
  const bool prevSwap = lcd->getSwapBytes();
  lcd->setSwapBytes(true);
  uint16_t spanBuf[LCD_WIDTH];
  int i = 0;
  while (i < s_underCount) {
    const int y = s_underY[i];
    int j = i;
    while (j < s_underCount && s_underY[j] == y) {
      ++j;
    }
    int p = i;
    while (p < j) {
      const int x0 = s_underX[p];
      int q = p + 1;
      while (q < j && s_underX[q] == (uint8_t)(s_underX[q - 1] + 1)) {
        ++q;
      }
      const int len = q - p;
      for (int k = 0; k < len; ++k) {
        const int idx = p + k;
        spanBuf[k] = (alpha == 0)
                         ? s_underPix[idx]
                         : blend565(s_color, s_underPix[idx], alpha);
      }
      lcd->pushImage(x0, y, len, 1, spanBuf);
      p = q;
    }
    i = j;
  }
  lcd->setSwapBytes(prevSwap);
  return true;
}

static void eraseRingBand(LGFX* lcd, int underlayZoom) {
  if (!lcd) {
    return;
  }
  if (ensureUnderCache(underlayZoom) && paintCachedFaded(lcd, 0)) {
    return;
  }
  if (underlayZoom >= ZOOM_MIN &&
      frameCachePaintAlertRing(lcd, underlayZoom, 0, 0)) {
    return;
  }
  // 无底图时不涂黑，避免出现黑环
}

static void drawAtFade(LGFX* lcd, float fade01, bool force, int underlayZoom) {
  if (!lcd || !s_active) {
    return;
  }
  const uint32_t now = millis();
  const bool holdComplete = fade01 >= 0.999f && animationTimeDone();
  const bool bigStep =
      force || (s_lastFade < 0.0f) ||
      (fabsf(fade01 - s_lastFade) >= 0.03f) || holdComplete;
  if (!force && !holdComplete && !bigStep &&
      (now - s_lastDrawMs) < kDrawMinMs) {
    return;
  }
  if (!force && holdComplete && s_visible && s_lastFade >= 0.999f) {
    return;
  }

  uint8_t alpha = 0;
  if (fade01 >= 0.999f) {
    alpha = 255;
  } else if (fade01 > 0.002f) {
    alpha = (uint8_t)lroundf(fade01 * 255.0f);
    if (alpha < 1) {
      alpha = 1;
    }
  }

  bool painted = false;
  if (underlayZoom >= ZOOM_MIN && ensureUnderCache(underlayZoom)) {
    painted = paintCachedFaded(lcd, alpha);
  }
  if (!painted && underlayZoom >= ZOOM_MIN) {
    painted = frameCachePaintAlertRing(lcd, underlayZoom, s_color, alpha);
  }
  if (!painted) {
    // 无底图：只在接近实色时画，避免颜色压黑造成黑环
    if (alpha >= 250) {
      paintFullRing(lcd, s_color);
      painted = true;
    }
  }

  s_visible = painted && alpha > 0;
  s_lastFade = fade01;
  s_lastDrawMs = now;
}

void alertRingSet(uint16_t color565, bool hasCloud) {
  if (!hasCloud || color565 == 0) {
    s_active = false;
    s_color = 0;
    s_lastFade = -1.0f;
    s_visible = false;
    clearUnderCache();
    return;
  }
  s_active = true;
  s_color = color565;
  // 前移 80ms 使首帧 alpha≈41（而非 0），消除换档后环带空窗闪烁
  const uint32_t now = millis();
  s_fadeStartMs = (now > 80) ? (now - 80) : 1;
  s_lastFade = -1.0f;
  s_lastDrawMs = 0;
  s_visible = false;
  // 环带底色由整屏 blit 同步采集。若当前底色属于其它 zoom，
  // ensureUnderCache() 会按 zoom 校验并回退读取；这里不能清掉刚采集的数据。
}

void alertRingClear(LGFX* lcd, int underlayZoom) {
  const bool was = s_visible || s_active;
  s_active = false;
  s_color = 0;
  s_lastFade = -1.0f;
  if (was && lcd) {
    eraseRingBand(lcd, underlayZoom);
  }
  s_visible = false;
  clearUnderCache();
}

void alertRingHide(LGFX* lcd, int underlayZoom) {
  alertRingClear(lcd, underlayZoom);
}

void alertRingTick(LGFX* lcd, int underlayZoom) {
  if (!s_active || !lcd) {
    return;
  }
  drawAtFade(lcd, currentFade01(), false, underlayZoom);
}

void alertRingRedraw(LGFX* lcd, int underlayZoom) {
  if (!s_active || !lcd) {
    return;
  }
  drawAtFade(lcd, currentFade01(), true, underlayZoom);
}

bool alertRingIsVisible() { return s_visible; }

bool alertRingNeedsTick() {
  if (!s_active) {
    return false;
  }
  if (s_lastFade < 0.0f) {
    return true;
  }
  return !animationTimeDone() || s_lastFade < 0.999f;
}
