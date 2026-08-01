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
    if (held < BTN_SHORT_MS) {
      return ButtonEvent::ShortPress;
    }
    if (held >= BTN_LONG_MS) {
      return ButtonEvent::LongPress;
    }
    if (held >= BTN_MED_MS) {
      return ButtonEvent::MedPress;
    }
    // 0.4s～0.8s：本轮忽略
    return ButtonEvent::None;
  }

  return ButtonEvent::None;
}
