#pragma once

#include <Arduino.h>

/** 使用 config.h 中的 WIFI_SSID/WIFI_PASS 连接 STA。成功返回 true。 */
bool wifiConnect();
