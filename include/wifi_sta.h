#pragma once

#include "app_config.h"

/** 按 AppConfig 连接 STA（PSK 或 PEAP）。成功返回 true。 */
bool wifiConnect(const AppConfig& cfg);

/** 断开 STA / 企业认证状态，便于再进 SoftAP。 */
void wifiDisconnectClean();
