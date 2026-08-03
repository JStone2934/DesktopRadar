#include "progress_ring.h"

#include <Arduino.h>
#include <math.h>

#include "config.h"

static bool s_barVisible = false;
static float s_lastDone = -1.0f;
static uint32_t s_lastDrawMs = 0;

static inline int barY() { return LCD_HEIGHT - OVERLAY_BAR_H; }

static void eraseBar(LGFX* lcd) {
  if (!lcd) {
    return;
  }
  lcd->fillRect(0, barY(), LCD_WIDTH, PROGRESS_BAR_THICK, TFT_BLACK);
}

static void paintBar(LGFX* lcd, float done01) {
  if (!lcd || done01 <= 0.002f) {
    return;
  }
  if (done01 > 1.0f) {
    done01 = 1.0f;
  }
  const int cx = LCD_WIDTH / 2;
  int half = (int)lroundf(done01 * (float)(LCD_WIDTH / 2));
  if (half < 1) {
    half = 1;
  }
  if (half > LCD_WIDTH / 2) {
    half = LCD_WIDTH / 2;
  }
  // 先整条顶边涂黑再画白段，避免变短时白痕残留
  eraseBar(lcd);
  lcd->fillRect(cx - half, barY(), half * 2, PROGRESS_BAR_THICK, TFT_WHITE);
}

void progressRingUpdate(LGFX* lcd, float done01, int underlayZoom) {
  (void)underlayZoom;
  if (!lcd) {
    return;
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
    return;
  }

  if (completed) {
    if (s_barVisible) {
      eraseBar(lcd);
      s_barVisible = false;
    }
    s_lastDone = 1.0f;
    s_lastDrawMs = now;
    return;
  }

  paintBar(lcd, done01);
  s_barVisible = true;
  s_lastDone = done01;
  s_lastDrawMs = now;
}

void progressRingHide(LGFX* lcd, int underlayZoom) {
  (void)underlayZoom;
  s_lastDone = -1.0f;
  if (!s_barVisible) {
    return;
  }
  s_barVisible = false;
  eraseBar(lcd);
}
