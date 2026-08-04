#include "progress_ring.h"

#include <Arduino.h>
#include <math.h>

#include "config.h"

static bool s_barVisible = false;
static float s_lastDone = -1.0f;
static uint32_t s_lastDrawMs = 0;
static int s_lastHalf = 0;

static inline int barY() { return LCD_HEIGHT - OVERLAY_BAR_H; }

/** 进度条半宽上限：避免画到屏缘预警环带（底栏顶边与环相交处）。 */
static int maxSafeHalf() {
  const int cy = LCD_HEIGHT / 2;
  // 用进度条底边（y=barY+THICK-1）计算，此处环内半径最小，确保不覆盖环带像素
  const int dy = barY() + PROGRESS_BAR_THICK - 1 - cy;
  const int rInner = (LCD_WIDTH / 2) - 1 - 3;
  const int dy2 = dy * dy;
  const int rI2 = rInner * rInner;
  if (dy2 >= rI2) {
    return 0;
  }
  int m = (int)floorf(sqrtf((float)(rI2 - dy2))) - 2;
  if (m < 1) {
    m = 1;
  }
  const int hard = LCD_WIDTH / 2 - 4;
  return m < hard ? m : hard;
}

static void eraseHalf(LGFX* lcd, int half) {
  if (!lcd || half <= 0) {
    return;
  }
  const int cx = LCD_WIDTH / 2;
  if (half > LCD_WIDTH / 2) {
    half = LCD_WIDTH / 2;
  }
  lcd->fillRect(cx - half, barY(), half * 2, PROGRESS_BAR_THICK, TFT_BLACK);
}

static void paintBar(LGFX* lcd, float done01) {
  if (!lcd || done01 <= 0.002f) {
    return;
  }
  if (done01 > 1.0f) {
    done01 = 1.0f;
  }
  const int cx = LCD_WIDTH / 2;
  const int safe = maxSafeHalf();
  int half = (int)lroundf(done01 * (float)safe);
  if (half < 1) {
    half = 1;
  }
  if (half > safe) {
    half = safe;
  }
  // 只擦旧白段再画，绝不全宽涂黑，避免伤预警环
  if (s_lastHalf > half) {
    eraseHalf(lcd, s_lastHalf);
  }
  lcd->fillRect(cx - half, barY(), half * 2, PROGRESS_BAR_THICK, TFT_WHITE);
  s_lastHalf = half;
}

bool progressRingUpdate(LGFX* lcd, float done01, int underlayZoom) {
  (void)underlayZoom;
  if (!lcd) {
    return false;
  }
  if (done01 < 0.0f) {
    done01 = 0.0f;
  }
  if (done01 > 1.0f) {
    done01 = 1.0f;
  }

  const uint32_t now = millis();
  const bool completed = done01 >= 0.998f;
  const bool bigStep = (s_lastDone < 0.0f) ||
                       (fabsf(done01 - s_lastDone) >= 0.02f) || completed;
  if (!completed && !bigStep && (now - s_lastDrawMs) < 180) {
    return false;
  }

  if (completed) {
    bool painted = false;
    if (s_barVisible) {
      eraseHalf(lcd, s_lastHalf > 0 ? s_lastHalf : maxSafeHalf());
      s_barVisible = false;
      s_lastHalf = 0;
      painted = true;
    }
    s_lastDone = 1.0f;
    s_lastDrawMs = now;
    return painted;
  }

  paintBar(lcd, done01);
  s_barVisible = true;
  s_lastDone = done01;
  s_lastDrawMs = now;
  return true;
}

bool progressRingHide(LGFX* lcd, int underlayZoom) {
  (void)underlayZoom;
  s_lastDone = -1.0f;
  if (!s_barVisible) {
    s_lastHalf = 0;
    return false;
  }
  s_barVisible = false;
  eraseHalf(lcd, s_lastHalf > 0 ? s_lastHalf : maxSafeHalf());
  s_lastHalf = 0;
  return true;
}
