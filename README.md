# ESP32-C3 + GC9A01 桌面气象雷达

端上气象雷达摆件固件（见 [`docs/plan.md`](docs/plan.md)）。

**当前进度（阶段 B 核心）：** 已能联网拉取高德底图 + RainViewer 最新雷达，合成静帧上屏；约每 15 分钟整帧重拉。缩放 / 历史动画 / Web 配网尚未做。

## 硬件接线

| 屏幕引脚 | ESP32-C3 GPIO |
|---------|---------------|
| RST | 0 |
| CS | 1 |
| DC | 2 |
| SDA (MOSI) | 3 |
| SCL (SCLK) | 4 |
| VCC | 3.3V |
| GND | GND |

无独立 BLK 时背光常亮。

## 编译与烧录

需安装 [PlatformIO](https://platformio.org/)。本机若无 `pio` 命令，可用 `py -m platformio`。

```bash
cd esp32c3
py -m platformio run                 # 编译
py -m platformio run -t upload       # 烧录（USB 自动识别）
py -m platformio device monitor      # 串口 115200
```

ESP32-C3 Super Mini 若上传失败：按住 **BOOT**，点一下 **RST**，松开 **BOOT** 后再 `upload`。

## 配置

编辑 [`include/config.h`](include/config.h)：

| 宏 | 含义 |
|----|------|
| `WIFI_SSID` / `WIFI_PASS` | 硬编码 WiFi（Web 配网后置） |
| `MAP_LAT` / `MAP_LON` | 地图中心（默认广州） |
| `MAP_ZOOM` | 固定 7（RainViewer 免费档上限） |
| `RADAR_REFRESH_MS` | 静帧重拉间隔（默认 15 分钟） |

## 当前行为

1. 连接 `WIFI_SSID`
2. 拉取 RainViewer `weather-maps.json` + 视口内高德 / 雷达 PNG 瓦片（通常 2×2）
3. LovyanGFX 解码后直绘 GC9A01；中心十字准星
4. 串口输出堆内存与瓦片进度；整帧约 **20–25 s**（瓶颈在 HTTPS）

若颜色颠倒，在 `include/LGFX_GC9A01.hpp` 中切换 `cfg.invert`。

## 性能与后续动画

实测瓶颈：**HTTPS/TLS ≫ PNG 解码 ≫ SPI**。播放循环内现下现解无法达到 5–10 fps；后续动画必须 **预渲 RGB565 帧到 Flash，本地 Flash→SPI 轮播**（详见 `docs/plan.md` §4.5）。
