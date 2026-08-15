#pragma once

#include "LGFX_GC9A01.hpp"

/**
 * 设置屏：手机连接热点并在浏览器输入网址的步骤说明。
 * remainSec >= 0：显示剩余秒数；remainSec < 0：等待保存（无倒计时）。
 */
void setupScreenDraw(LGFX* lcd, int remainSec);

/** 仅刷新底部倒计时/等待文案。 */
void setupScreenUpdateStatus(LGFX* lcd, int remainSec);

/** 保存成功后立即替换引导页，明确告知用户设备已进入联网阶段。 */
void setupScreenShowSaved(LGFX* lcd, const char* ssid);
