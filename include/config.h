#pragma once

// GC9A01 接线（ESP32-C3 GPIO）
//   0 -> RST
//   1 -> CS
//   2 -> DC
//   3 -> SDA (MOSI)
//   4 -> SCL (SCLK)
// VCC -> 3.3V, GND -> GND；无独立 BLK 时背光常亮

#define PIN_LCD_RST  0
#define PIN_LCD_CS   1
#define PIN_LCD_DC   2
#define PIN_LCD_MOSI 3
#define PIN_LCD_SCLK 4
#define PIN_LCD_BL   -1  // 无背光控制脚

#define LCD_WIDTH  240
#define LCD_HEIGHT 240

// 底栏日期条高度；进度条贴其顶边
#define OVERLAY_BAR_H 24
#define PROGRESS_BAR_THICK 2

// 杜邦线/飞线先用较低速率；稳定后再提到 40M
#define SPI_FREQ_WRITE 20000000

// ---- WiFi 默认（NVS 未保存时回退；Web 配网写入 Preferences）----
#define WIFI_SSID "GL-AXT1800-c11"
#define WIFI_PASS "T3Y8NHYDK9"
#define WIFI_CONNECT_TIMEOUT_MS 30000

// SoftAP 配置门户
#define SOFTAP_SSID "RadarSetup"
#define SOFTAP_PASS "radar1234"
#define CONFIG_PORTAL_URL "http://192.168.4.1"
#define CONFIG_PORTAL_TIMEOUT_MS 180000
#define CONFIG_PORTAL_RECONNECT_GRACE_MS 60000

// ---- 地图中心：广州市区 ----
#define MAP_LAT 23.1291f
#define MAP_LON 113.2644f
#define MAP_ZOOM 7  // 默认档；与 DesktopRadar 默认一致
// 雷达帧 Unix 时间 → 北京时间（UTC+8）显示
#define TIMEZONE_OFFSET_SEC (8 * 3600)

// 缩放档位（与 DesktopRadar ZOOM_MIN/MAX、步进 1 对齐）
#define ZOOM_MIN 3
#define ZOOM_MAX 12
// 无跳过档（按住动画关闭后恢复 z11）；置为 -1 使既有 ZOOM_SKIP 判断失效
#define ZOOM_SKIP (-1)
// RainViewer 免费档原生雷达瓦片上限；更高档对 z7 瓦片上采样绘制
#define RAINVIEWER_MAX_ZOOM 7

#define TILE_SIZE 256
#define VIEW_HALF (LCD_WIDTH / 2)

// ---- S 键（板载 GPIO9）----
#define PIN_BTN_BOOT 9
// S 键：18ms 以上即视为有效按压；未达到长按阈值的释放全部切缩放，
// 不再保留 1.5–2.0s 的无响应死区。按住 350ms 开始十字反馈，1.8s 回默认档。
#define BTN_MED_MS 1800
#define BTN_SHORT_MS BTN_MED_MS
#define BTN_LONG_FEEDBACK_MS 350
#define BTN_LONG_MS 1800

// 用户切换后暂停后台预取，避免下载/烘焙任务连续抢占按键响应。
#define CACHE_PAUSE_AFTER_USER_MS 10000UL
// 风场模式以全档缓存秒切为最高优先级；用户停止操作一分钟后才恢复静态后台刷新。
#define WIND_CACHE_PAUSE_AFTER_USER_MS 60000UL

// ---- 数据源 ----
#define RAINVIEWER_API "https://api.rainviewer.com/public/weather-maps.json"
#define AMAP_TILE_FMT \
  "https://webrd0%u.is.autonavi.com/appmaptile" \
  "?lang=zh_cn&size=1&scale=1&style=8&x=%d&y=%d&z=%d"
#define AMAP_REFERER "https://www.amap.com/"

// 底图压暗：result = tile * blend + backdrop * (1 - blend)
#define BASEMAP_BLEND 0.35f
#define BASEMAP_BACKDROP_R 15
#define BASEMAP_BACKDROP_G 20
#define BASEMAP_BACKDROP_B 30

#define HTTP_TIMEOUT_MS 15000
#define TILE_TIMEOUT_MS 10000
#define HTTP_MAX_JSON_BYTES (32 * 1024)
#define HTTP_MAX_TILE_BYTES (96 * 1024)

// ---- 当前 10m 风场粒子 ----
// Open-Meteo ECMWF：7x7 当前视口网格；显示层按屏幕位置双线性插值。
#define WIND_API "https://api.open-meteo.com/v1/ecmwf"
#define WIND_GRID_N 7
#define WIND_PARTICLE_COUNT 56
#define WIND_TRAIL_POINTS 4
#define WIND_FRAME_MS 167UL       // 正常约 6 FPS
#define WIND_BUSY_FRAME_MS 500UL  // HTTPS / PNG / 造片期间约 2 FPS
#define WIND_FIELD_REFRESH_MS (60UL * 60UL * 1000UL)
#define WIND_FIELD_RETRY_MS (30UL * 1000UL)
#define WIND_FIELD_SLOW_RETRY_MS (10UL * 60UL * 1000UL)
#define WIND_FIELD_FAST_RETRY_LIMIT 3
#define WIND_FIELD_SETTLE_MS 2500UL
#define WIND_HTTP_TIMEOUT_MS 20000UL
#define WIND_MAX_JSON_BYTES (28UL * 1024UL)

// 风场模式静态图分级刷新：全档成品一直保留，过时档在后台重建。
#define WIND_ADJACENT_REFRESH_MS (15UL * 60UL * 1000UL)
#define WIND_FAR_REFRESH_MS (45UL * 60UL * 1000UL)

#define FRAME_RGB565_BYTES (LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t))
#define FRAME_ROW_BYTES (LCD_WIDTH * sizeof(uint16_t))

// RainViewer past 帧缓存上限与时间窗（rainviewer.cpp 仍使用）
#define ANIM_MAX_FRAMES 10
#define ANIM_WINDOW_HOURS 6.0f

// 提升以作废旧缓存；11=拒绝底图缺瓦仍 commit 的残缺成品（黑角）
#define FRAME_CACHE_GEN 11

// 雷达静帧刷新间隔（对齐 DesktopRadar 默认 300s）
#define RADAR_REFRESH_MS (5UL * 60UL * 1000UL)
// 定时刷新失败/中止后重试间隔
#define RADAR_REFRESH_RETRY_MS (60UL * 1000UL)
