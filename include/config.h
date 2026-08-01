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

// 杜邦线/飞线先用较低速率；稳定后再提到 40M
#define SPI_FREQ_WRITE 20000000

// ---- WiFi（本阶段硬编码；Web 配网后置）----
#define WIFI_SSID "GL-AXT1800-c11"
#define WIFI_PASS "T3Y8NHYDK9"
#define WIFI_CONNECT_TIMEOUT_MS 30000

// ---- 地图中心：广州市区 ----
#define MAP_LAT 23.1291f
#define MAP_LON 113.2644f
#define MAP_ZOOM 7

#define TILE_SIZE 256
#define VIEW_HALF (LCD_WIDTH / 2)

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
#define HTTP_MAX_TILE_BYTES (40 * 1024)

// 雷达静帧刷新间隔
#define RADAR_REFRESH_MS (15UL * 60UL * 1000UL)
