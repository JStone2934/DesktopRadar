#pragma once

#include "LGFX_GC9A01.hpp"

/**
 * 设置屏：步骤说明 + 缩小的 URL 二维码（整屏只应调用一次，避免堵 WebServer）。
 * remainSec >= 0：显示剩余秒数；remainSec < 0：等待保存（无倒计时）。
 */
void setupScreenDraw(LGFX* lcd, int remainSec);

/** 仅刷新底部倒计时/等待文案，不重画二维码。 */
void setupScreenUpdateStatus(LGFX* lcd, int remainSec);
