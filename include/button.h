#pragma once

#include <stdint.h>

enum class ButtonEvent : uint8_t {
  None = 0,
  ShortPress,  // 松手且按住 < BTN_HOLD_PLAY_MS
  MedPress,    // 保留；按住播放不走松手事件
  LongPress,   // 松手且按住 >= BTN_LONG_MS
};

void buttonBegin();

/** 非阻塞取事件；松手时根据按住时长产生一次。
 *  边沿由 GPIO 中断锁存，避免 http.GET 等长时间阻塞丢短按。
 *  按住播放区间（HOLD_PLAY..LONG）松手不产生事件（已由按住态消费）。
 */
ButtonEvent buttonPoll();

/** 当前是否按下（已武装且忽略窗口外）。 */
bool buttonIsDown();

/** 当前按住时长（ms）；未按下返回 0。 */
uint32_t buttonHeldMs();
