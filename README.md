# ESP32-C3 + GC9A01 桌面气象雷达

端上气象雷达摆件固件（见 [`docs/plan.md`](docs/plan.md)）。

**当前进度：** 高德底图 + RainViewer 最新雷达静帧；**BOOT 短按**在 z3–12 循环切档；z3–7 瓦片 PNG 写入 LittleFS；空闲时按距离排队铺满 z3–7，命中本地重绘免 HTTPS。z≥8 上采样后置。

## 硬件接线

| 屏幕引脚 | ESP32-C3 GPIO |
|----------|---------------|
| RST | 0 |
| CS | 1 |
| DC | 2 |
| SDA (MOSI) | 3 |
| SCL (SCLK) | 4 |
| VCC | 3.3V |
| GND | GND |

| 按键 | GPIO |
|------|------|
| BOOT（短按切缩放） | 9（板载，上拉） |

无独立 BLK 时背光常亮。

## 编译与烧录

需安装 [PlatformIO](https://platformio.org/)。本机若无 `pio` 命令，可用 `py -m platformio`。

```bash
cd esp32c3
py -m platformio run                 # 编译
py -m platformio run -t upload       # 烧录（USB 自动识别）
py -m platformio device monitor      # 串口 115200
```

首次使用 LittleFS：固件会在挂载失败时自动 format。也可手动：

```bash
py -m platformio run -t uploadfs
```

ESP32-C3 Super Mini 若上传失败：按住 **BOOT**，点一下 **RST**，松开 **BOOT** 后再 `upload`。

## 配置

编辑 [`include/config.h`](include/config.h)：

| 宏 | 含义 |
|----|------|
| `WIFI_SSID` / `WIFI_PASS` | 硬编码 WiFi（Web 配网后置） |
| `MAP_LAT` / `MAP_LON` | 地图中心（默认广州） |
| `MAP_ZOOM` | 默认档（7） |
| `ZOOM_MIN` / `ZOOM_MAX` | 3 / 12（与 DesktopRadar 一致） |
| `RAINVIEWER_MAX_ZOOM` | 7；更高档造片本轮不做 |
| `RADAR_REFRESH_MS` | 当前档重拉间隔（默认 15 分钟） |
| `BTN_SHORT_MS` | 短按阈值（默认 400ms） |

## 操作

| 操作 | 行为 |
|------|------|
| BOOT **短按**（&lt;0.4s） | 循环 z3→…→z12→z3；屏上短暂显示 `zN` |
| 切到已缓存档 | 从 `/frames/zNN/` 瓦片 PNG 本地重绘（免 HTTPS） |
| 切到未缓存且 z≤7 | 显示 Fetching，造片并缓存瓦片 PNG |
| 切到 z≥8 | 保持上一帧，提示 `zN soon` |
| 空闲 | 按距离排队预取全部 z3–7（当前档优先，再 ±1、±2…） |
| 约 15 分钟 | 重建并覆盖**当前**档缓存 |

中长按动画：手势已预留，行为后置。

**规划中的配网（尚未实现，见 `docs/plan.md` §6）：**

- 上电 / 按 **RST** → 先开 SoftAP 配置门户
- 屏上：**文字**热点名 + Web 管理 URL + **仅网址二维码**（不显示 WiFi 入网码）
- 启动窗口内短按 **BOOT**（或超时）→ 跳过，用 NVS / 默认配置继续
- 须先在手机 WiFi 列表手动连热点，再扫码/手打打开管理页；扫 URL 码不会自动连热点

## 当前行为（启动）

1. 挂载 LittleFS，连接 `WIFI_SSID`
2. 显示默认 zoom（缓存命中则直出，否则造片）
3. 空闲按距离排队预取 z3–7；串口输出堆与进度日志

若颜色颠倒，在 `include/LGFX_GC9A01.hpp` 中切换 `cfg.invert`。

## 性能与后续

造片瓶颈仍是 **HTTPS**（约 20–25 s/档）。已缓存档短按不走网络。历史动画仍须预渲多帧后本地轮播（见 `docs/plan.md` §4.5）。
