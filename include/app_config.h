#pragma once

#include <stdint.h>

/** 应用层 WiFi 认证（勿与 esp_wifi WIFI_AUTH_* 混用）。 */
enum AppWifiMode : uint8_t {
  APP_WIFI_PSK = 0,
  APP_WIFI_PEAP = 1,
  APP_WIFI_OPEN = 2,
};

struct AppConfig {
  AppWifiMode wifi_mode;
  char ssid[33];
  char pass[65];            // PSK 密码；PEAP 用户密码；开放网络为空
  char identity[64];        // PEAP/MSCHAPv2 内层用户名；其它模式可空
  char outer_identity[64];  // PEAP 外层 identity；空时使用 identity
  float lat;
  float lon;
  bool show_progress;   // 底栏横向进度条
  bool show_alert_ring;  // 中心天气预警环
  int default_zoom;      // 启动默认缩放档；按住 S 键 2 秒跳回此档
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

/** 非安全用途的短签名，仅用于串口比对密码是否被表单/NVS 改写。 */
uint16_t appConfigSecretSig(const char* s);
