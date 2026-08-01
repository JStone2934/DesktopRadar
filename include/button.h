#pragma once

#include <stdint.h>

enum class ButtonEvent : uint8_t {
  None = 0,
  ShortPress,
  MedPress,   // 骨架：本轮不处理
  LongPress,  // 骨架：本轮不处理
};

void buttonBegin();

/** 非阻塞轮询；在松手时根据按住时长产生一次事件。 */
ButtonEvent buttonPoll();
