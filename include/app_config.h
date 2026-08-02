#pragma once

#include <stdint.h>

/** 应用层 WiFi 认证（勿与 esp_wifi WIFI_AUTH_* 混用）。 */
enum AppWifiMode : uint8_t {
  APP_WIFI_PSK = 0,
  APP_WIFI_PEAP = 1,
};

struct AppConfig {
  AppWifiMode wifi_mode;
  char ssid[33];
  char pass[65];      // PSK 密码；PEAP 用户密码
  char identity[64];  // PEAP Identity；PSK 可空
  float lat;
  float lon;
  bool show_progress;   // 屏缘进度环
  bool show_alert_ring;  // 中心天气预警环
};

/** 用 config.h 宏填充默认值（PSK + MAP_LAT/LON）。 */
void appConfigSetDefaults(AppConfig* cfg);

/**
 * 从 NVS 加载。若从未保存过则写入 defaults 并返回 false；
 * 已保存则填充 cfg 并返回 true。
 */
bool appConfigLoad(AppConfig* cfg);

/** 写入 NVS，标记已配置。成功返回 true。 */
bool appConfigSave(const AppConfig* cfg);

/** 是否曾通过 Web 保存过配置。 */
bool appConfigHasSaved();
