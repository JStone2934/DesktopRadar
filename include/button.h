#pragma once

#include <stdint.h>

enum class ButtonEvent : uint8_t {
  None = 0,
  ShortPress,
  MedPress,   // 骨架：本轮不处理
  LongPress,  // 骨架：本轮不处理
};

void buttonBegin();

/** 非阻塞取事件；松手时根据按住时长产生一次。
 *  边沿由 GPIO 中断锁存，避免 http.GET 等长时间阻塞丢短按。
 */
ButtonEvent buttonPoll();
