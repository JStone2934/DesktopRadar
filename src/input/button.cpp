#include "button.h"

#include <Arduino.h>

#include "config.h"

static volatile bool s_armed = true;
static volatile uint32_t s_downAt = 0;
static volatile uint8_t s_latched = 0;  // ButtonEvent
static constexpr uint32_t kDebounceMs = 30;

static void IRAM_ATTR bootIsr() {
  if (!s_armed) {
    return;
  }
  const uint32_t now = millis();
  if (digitalRead(PIN_BTN_BOOT) == LOW) {
    // 按下：只记首次边沿，忽略抖动
    if (s_downAt == 0) {
      s_downAt = now == 0 ? 1 : now;
    }
    return;
  }
  // 松开
  if (s_downAt == 0) {
    return;
  }
  const uint32_t held = now - s_downAt;
  s_downAt = 0;
  if (held < kDebounceMs) {
    return;
  }
  if (held < BTN_LONG_MS) {
    s_latched = static_cast<uint8_t>(ButtonEvent::ShortPress);
  } else {
    s_latched = static_cast<uint8_t>(ButtonEvent::LongPress);
  }
}

void buttonBegin() {
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  s_downAt = 0;
  s_latched = 0;
  s_armed = true;
  // 上电时 BOOT 可能仍被按住（烧录），等释放后再武装
  if (digitalRead(PIN_BTN_BOOT) == LOW) {
    s_armed = false;
  }
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_BOOT), bootIsr, CHANGE);
}

ButtonEvent buttonPoll() {
  if (!s_armed) {
    if (digitalRead(PIN_BTN_BOOT) != LOW) {
      s_armed = true;
      s_downAt = 0;
    }
    return ButtonEvent::None;
  }

  noInterrupts();
  const uint8_t ev = s_latched;
  s_latched = 0;
  interrupts();
  return static_cast<ButtonEvent>(ev);
}
