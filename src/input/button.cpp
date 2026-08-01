#include "button.h"

#include <Arduino.h>

#include "config.h"

static bool s_down = false;
static uint32_t s_downAt = 0;
static bool s_armed = true;

void buttonBegin() {
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  s_down = false;
  s_downAt = 0;
  s_armed = true;
  // 上电时 BOOT 可能仍被按住（烧录），等释放后再武装
  if (digitalRead(PIN_BTN_BOOT) == LOW) {
    s_armed = false;
  }
}

ButtonEvent buttonPoll() {
  const bool pressed = digitalRead(PIN_BTN_BOOT) == LOW;
  const uint32_t now = millis();

  if (!s_armed) {
    if (!pressed) {
      s_armed = true;
    }
    return ButtonEvent::None;
  }

  if (pressed && !s_down) {
    s_down = true;
    s_downAt = now;
    return ButtonEvent::None;
  }

  if (!pressed && s_down) {
    s_down = false;
    const uint32_t held = now - s_downAt;
    // 小于长按阈值一律当短按切档（避免 0.4–0.8s 死区无响应）
    if (held < BTN_LONG_MS) {
      if (held >= BTN_MED_MS) {
        // 仍回报 ShortPress，保证切档；Med 骨架暂不单独占用
        return ButtonEvent::ShortPress;
      }
      return ButtonEvent::ShortPress;
    }
    return ButtonEvent::LongPress;
  }

  return ButtonEvent::None;
}
