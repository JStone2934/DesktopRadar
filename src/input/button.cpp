#include "button.h"

#include <Arduino.h>

#include "config.h"

static volatile bool s_armed = true;
static volatile uint32_t s_downAt = 0;
static volatile uint8_t s_latched = 0;  // ButtonEvent
static volatile bool s_longFired = false;
static uint32_t s_ignoreUntil = 0;
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
      s_longFired = false;
    }
    return;
  }
  // 松开
  if (s_downAt == 0) {
    return;
  }
  const uint32_t held = now - s_downAt;
  const bool longFired = s_longFired;
  s_downAt = 0;
  s_longFired = false;
  if (held < kDebounceMs) {
    return;
  }
  if (!longFired && held < BTN_SHORT_MS) {
    s_latched = static_cast<uint8_t>(ButtonEvent::ShortPress);
  }
}

void buttonBegin() {
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  s_downAt = 0;
  s_latched = 0;
  s_armed = true;
  // 上电时 S 键可能仍被按住（烧录），等释放后再武装
  if (digitalRead(PIN_BTN_BOOT) == LOW) {
    s_armed = false;
  }
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_BOOT), bootIsr, CHANGE);
  // USB 复位/上电边沿噪声：短暂忽略，避免门户被“假短按”跳过
  s_ignoreUntil = millis() + 800;
  s_latched = 0;
  s_downAt = 0;
}

ButtonEvent buttonPoll() {
  if (!s_armed) {
    if (digitalRead(PIN_BTN_BOOT) != LOW) {
      s_armed = true;
      s_downAt = 0;
      s_longFired = false;
    }
    return ButtonEvent::None;
  }

  if (millis() < s_ignoreUntil) {
    noInterrupts();
    s_latched = 0;
    s_downAt = 0;
    s_longFired = false;
    interrupts();
    return ButtonEvent::None;
  }

  const uint32_t now = millis();
  noInterrupts();
  const uint32_t downAt = s_downAt;
  const bool longFired = s_longFired;
  if (downAt != 0 && !longFired && (now - downAt) >= BTN_LONG_MS) {
    s_longFired = true;
    interrupts();
    return ButtonEvent::LongPress;
  }
  interrupts();

  noInterrupts();
  const uint8_t ev = s_latched;
  s_latched = 0;
  interrupts();
  return static_cast<ButtonEvent>(ev);
}

bool buttonIsDown() {
  if (!s_armed || millis() < s_ignoreUntil) {
    return false;
  }
  return digitalRead(PIN_BTN_BOOT) == LOW;
}

uint32_t buttonHeldMs() {
  if (!buttonIsDown()) {
    return 0;
  }
  noInterrupts();
  const uint32_t downAt = s_downAt;
  interrupts();
  if (downAt == 0) {
    // 中断尚未记录按下时刻：用当前作近似
    return 1;
  }
  return millis() - downAt;
}
