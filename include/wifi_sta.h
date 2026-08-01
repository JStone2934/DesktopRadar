#pragma once

#include "app_config.h"

/** 按 AppConfig 连接 STA（PSK 或 PEAP）。成功返回 true。
 *  若默认 SSID 的 NVS 密码错误，可能回退 config.h 并写回 cfg/NVS。
 */
bool wifiConnect(AppConfig* cfg);

/** 断开 STA / 企业认证状态，便于再进 SoftAP。 */
void wifiDisconnectClean();
