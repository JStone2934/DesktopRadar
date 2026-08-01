#include "progress_ring.h"

#include <math.h>

#include "config.h"
#include "frame_cache.h"

static bool s_ringVisible = false;
static float s_lastDone = -1.0f;
static uint32_t s_lastDrawMs = 0;

static void paintRemainingArc(LGFX* lcd, float remaining01) {
  if (!lcd || remaining01 <= 0.002f) {
    return;
  }
  if (remaining01 > 1.0f) {
    remaining01 = 1.0f;
  }
  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  // 贴圆屏外缘；厚度约 3px
  const int rOuter = (LCD_WIDTH / 2) - 1;
  const int rInner = rOuter - 3;
  const float sweep = remaining01 * 360.0f;
  // 自顶部顺时针：LovyanGFX 角度 0=右、逆时针；270 为顶
  lcd->fillArc(cx, cy, rOuter, rInner, 270.0f, 270.0f + sweep, TFT_WHITE);
}

void progressRingUpdate(LGFX* lcd, float done01, int underlayZoom) {
  if (!lcd) {
    return;
  }
  if (done01 < 0.0f) {
    done01 = 0.0f;
  }
  if (done01 > 1.0f) {
    done01 = 1.0f;
  }

  const float remaining = 1.0f - done01;
  const uint32_t now = millis();
  const bool completed = remaining <= 0.002f;
  const bool bigStep = (s_lastDone < 0.0f) ||
                       (fabsf(done01 - s_lastDone) >= 0.02f) || completed;
  // 预取时避免每瓦片全屏 blit；约 180ms 或跨 2% 再刷
  if (!completed && !bigStep && (now - s_lastDrawMs) < 180) {
    return;
  }

  const int cx = LCD_WIDTH / 2;
  const int cy = LCD_HEIGHT / 2;
  const int rOuter = (LCD_WIDTH / 2) - 1;
  const int rInner = rOuter - 3;

  if (completed) {
    if (s_ringVisible) {
      if (underlayZoom >= ZOOM_MIN && frameCacheHas(underlayZoom)) {
        frameCacheBlit(lcd, underlayZoom);
      } else {
        lcd->fillArc(cx, cy, rOuter, rInner, 0.0f, 360.0f, TFT_BLACK);
      }
      s_ringVisible = false;
    }
    s_lastDone = 1.0f;
    s_lastDrawMs = now;
    return;
  }

  if (underlayZoom >= ZOOM_MIN && frameCacheHas(underlayZoom)) {
    frameCacheBlit(lcd, underlayZoom);
  } else {
    // 状态黑屏：先擦环带再画剩余，避免变短时白痕残留
    lcd->fillArc(cx, cy, rOuter, rInner, 0.0f, 360.0f, TFT_BLACK);
  }
  paintRemainingArc(lcd, remaining);
  s_ringVisible = true;
  s_lastDone = done01;
  s_lastDrawMs = now;
}

void progressRingHide(LGFX* lcd, int underlayZoom) {
  s_lastDone = -1.0f;
  if (!s_ringVisible) {
    return;
  }
  s_ringVisible = false;
  if (lcd && underlayZoom >= ZOOM_MIN && frameCacheHas(underlayZoom)) {
    frameCacheBlit(lcd, underlayZoom);
  }
}
