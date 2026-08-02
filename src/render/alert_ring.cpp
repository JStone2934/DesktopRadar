#include "alert_ring.h"

#include <math.h>

#include "config.h"
#include "frame_cache.h"

static constexpr uint32_t kFadeMs = 3000;
static constexpr uint32_t kDrawMinMs = 100;

static bool s_active = false;
static bool s_visible = false;
static uint16_t s_color = 0;
static uint32_t s_fadeStartMs = 0;
static float s_lastFade = -1.0f;
static uint32_t s_lastDrawMs = 0;

static inline uint16_t scaleColor565(uint16_t c, float fade01) {
  if (fade01 <= 0.0f) {
    return 0;
  }
  if (fade01 >= 1.0f) {
    return c;
  }
  const int r = (int)lroundf(((c >> 11) & 0x1F) * fade01);
  const int g = (int)lroundf(((c >> 5) & 0x3F) * fade01);
  const int b = (int)lroundf((c & 0x1F) * fade01);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static float currentFade01() {
  if (!s_active) {
    return 0.0f;
  }
  const uint32_t elapsed = millis() - s_fadeStartMs;
  if (elapsed >= kFadeMs) {
    return 1.0f;
  }
  return (float)elapsed / (float)kFadeMs;
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

static void eraseRingBand(LGFX* lcd, int underlayZoom) {
  if (!lcd) {
    return;
  }
  if (underlayZoom >= ZOOM_MIN && frameCacheBlitUnderlay(lcd, underlayZoom)) {
    return;
  }
  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  const int rOuter = (LCD_WIDTH / 2) - 1;
  const int rInner = rOuter - 3;
  lcd->fillArc(cx, cy, rOuter, rInner, 0.0f, 360.0f, TFT_BLACK);
}

/**
 * 仅绘制彩环（不 blit），以便叠在进度环之上。
 * 淡入只增不减，可用更亮颜色直接盖住上一帧。
 */
static void drawAtFade(LGFX* lcd, float fade01, bool force) {
  if (!lcd || !s_active) {
    return;
  }
  const uint32_t now = millis();
  const bool completed = fade01 >= 0.999f;
  const bool bigStep =
      force || (s_lastFade < 0.0f) ||
      (fabsf(fade01 - s_lastFade) >= 0.03f) || completed;
  if (!force && !completed && !bigStep && (now - s_lastDrawMs) < kDrawMinMs) {
    return;
  }
  if (!force && completed && s_visible && s_lastFade >= 0.999f) {
    return;
  }

  if (fade01 > 0.002f) {
    paintFullRing(lcd, scaleColor565(s_color, fade01));
    s_visible = true;
  } else {
    s_visible = false;
  }
  s_lastFade = fade01;
  s_lastDrawMs = now;
}

void alertRingSet(uint16_t color565, bool hasCloud) {
  if (!hasCloud || color565 == 0) {
    s_active = false;
    s_color = 0;
    s_lastFade = -1.0f;
    s_visible = false;
    return;
  }
  s_active = true;
  s_color = color565;
  s_fadeStartMs = millis() == 0 ? 1 : millis();
  s_lastFade = -1.0f;
  s_lastDrawMs = 0;
  s_visible = false;
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
}

void alertRingHide(LGFX* lcd, int underlayZoom) {
  alertRingClear(lcd, underlayZoom);
}

void alertRingTick(LGFX* lcd, int underlayZoom) {
  (void)underlayZoom;
  if (!s_active || !lcd) {
    return;
  }
  drawAtFade(lcd, currentFade01(), false);
}

void alertRingRedraw(LGFX* lcd, int underlayZoom) {
  (void)underlayZoom;
  if (!s_active || !lcd) {
    return;
  }
  drawAtFade(lcd, currentFade01(), true);
}

bool alertRingIsVisible() { return s_visible; }

bool alertRingNeedsTick() {
  if (!s_active) {
    return false;
  }
  return currentFade01() < 0.999f || s_lastFade < 0.0f;
}
