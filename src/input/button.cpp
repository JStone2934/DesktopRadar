#include "button.h"

#include <Arduino.h>
#include <driver/gpio.h>

#include "config.h"

static volatile bool s_armed = true;
static volatile uint32_t s_downAt = 0;
static volatile uint8_t s_latched = 0;  // ButtonEvent
static volatile bool s_longFired = false;
static volatile bool s_priorityRequested = false;
static uint32_t s_ignoreUntil = 0;
static constexpr uint32_t kDebounceMs = 18;

static inline bool IRAM_ATTR buttonLevelDown() {
  return gpio_get_level(static_cast<gpio_num_t>(PIN_BTN_BOOT)) == 0;
}

static void IRAM_ATTR bootIsr() {
  if (!s_armed) {
    return;
  }
  const uint32_t now = millis();
  if (buttonLevelDown()) {
    s_priorityRequested = true;
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
    s_priorityRequested = false;
    return;
  }
  if (!longFired && held < BTN_SHORT_MS) {
    s_latched = static_cast<uint8_t>(ButtonEvent::ShortPress);
  }
}

void buttonBegin() {
  // Arduino-ESP32 3.x 引入了统一引脚管理层；板载 BOOT/S 键改用 IDF 5
  // 原生 GPIO 配置和读取，避免 WiFi/USB 初始化后兼容层偶发丢失输入状态。
  gpio_config_t io{};
  io.pin_bit_mask = 1ULL << PIN_BTN_BOOT;
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  const esp_err_t configErr = gpio_config(&io);
  s_downAt = 0;
  s_latched = 0;
  s_priorityRequested = false;
  s_armed = true;
  // 上电时 S 键可能仍被按住（烧录），等释放后再武装
  if (buttonLevelDown()) {
    s_armed = false;
  }
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_BOOT), bootIsr, CHANGE);
  // USB 复位/上电边沿噪声：短暂忽略，避免门户被“假短按”跳过
  s_ignoreUntil = millis() + 800;
  s_latched = 0;
  s_downAt = 0;
  Serial.printf("button init: gpio=%d config=%s level=%d armed=%d\n",
                PIN_BTN_BOOT, esp_err_to_name(configErr),
                (int)gpio_get_level(static_cast<gpio_num_t>(PIN_BTN_BOOT)),
                (int)s_armed);
}

void buttonService() {
  if (!s_armed) {
    if (!buttonLevelDown()) {
      s_armed = true;
      s_downAt = 0;
      s_longFired = false;
      s_priorityRequested = false;
    }
    return;
  }

  if (millis() < s_ignoreUntil) {
    noInterrupts();
    s_latched = 0;
    s_downAt = 0;
    s_longFired = false;
    s_priorityRequested = false;
    interrupts();
    return;
  }

  const uint32_t now = millis();
  const bool physicalDown = buttonLevelDown();

  // GPIO9 的 CHANGE 中断负责在 HTTP 等阻塞阶段锁存短按；轮询时再用
  // 实际电平对齐一次，避免复位、WiFi 模式切换或边沿抖动造成漏按。
  // 门户约每 5ms 调用本函数，因此即使中断漏掉也能可靠捕获按下/松开。
  noInterrupts();
  if (physicalDown && s_downAt == 0) {
    s_priorityRequested = true;
    s_downAt = now == 0 ? 1 : now;
    s_longFired = false;
  } else if (!physicalDown && s_downAt != 0) {
    const uint32_t held = now - s_downAt;
    const bool longWasFired = s_longFired;
    s_downAt = 0;
    s_longFired = false;
    if (held >= kDebounceMs && !longWasFired && held < BTN_SHORT_MS) {
      s_latched = static_cast<uint8_t>(ButtonEvent::ShortPress);
    } else if (held < kDebounceMs) {
      s_priorityRequested = false;
    }
  }
  if (s_downAt != 0 && !s_longFired &&
      (now - s_downAt) >= BTN_LONG_MS) {
    s_longFired = true;
    s_latched = static_cast<uint8_t>(ButtonEvent::LongPress);
  }
  interrupts();
}

ButtonEvent buttonPoll() {
  buttonService();

  noInterrupts();
  const uint8_t ev = s_latched;
  s_latched = 0;
  if (ev != static_cast<uint8_t>(ButtonEvent::None)) {
    s_priorityRequested = false;
  }
  interrupts();
  return static_cast<ButtonEvent>(ev);
}

bool buttonPriorityRequested() {
  if (!s_armed || millis() < s_ignoreUntil) {
    return false;
  }
  return s_priorityRequested;
}

bool buttonIsDown() {
  if (!s_armed || millis() < s_ignoreUntil) {
    return false;
  }
  return buttonLevelDown();
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
