# ESP32-C3 + GC9A01 桌面气象雷达

端上气象雷达摆件固件（见 [`docs/plan.md`](docs/plan.md)）。

**当前进度：** 高德底图 + RainViewer 静帧；BOOT 短按 z3–12；**RGB565 成品全档缓存**（秒切）；空闲按距离铺满；z≥8 雷达双线性上采样。历史动画 / Web 配网尚未做。

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

## 编译与烧录

需安装 [PlatformIO](https://platformio.org/)。本机若无 `pio` 命令，可用 `py -m platformio`。

自定义分区（[`partitions.csv`](partitions.csv)）：app ≈1.5MB，LittleFS ≈**2.375MB**（容纳 10 档 ×112KB RGB565）。**改分区后建议全擦再烧：**

```bash
cd esp32c3
py -m platformio run -t erase
py -m platformio run -t upload
py -m platformio device monitor      # 串口 115200
```

ESP32-C3 Super Mini 若上传失败：按住 **BOOT**，点一下 **RST**，松开 **BOOT** 后再 `upload`。

## 配置

编辑 [`include/config.h`](include/config.h)：

| 宏 | 含义 |
|----|------|
| `WIFI_SSID` / `WIFI_PASS` | 硬编码 WiFi |
| `MAP_LAT` / `MAP_LON` | 地图中心（默认广州） |
| `MAP_ZOOM` | 默认档（7） |
| `ZOOM_MIN` / `ZOOM_MAX` | 3 / 12 |
| `RAINVIEWER_MAX_ZOOM` | 7；更高档双线性放大 z7 雷达瓦片 |
| `RADAR_REFRESH_MS` | 当前档重拉间隔（默认 15 分钟） |

## 操作

| 操作 | 行为 |
|------|------|
| BOOT **短按**（&lt;0.4s） | 循环 z3→…→z12；已缓存档 **Flash→SPI 秒切** |
| 未缓存 | 下载瓦片 → 烘焙 `/frames/zNN.rgb565`（可较慢） |
| 空闲 | 按距离预取全部 z3–12（首次约数分钟） |
| 约 15 分钟 | 重建当前档成品 |

## 缓存结构

- 成品：`/frames/zNN.rgb565`（115200 字节）+ `/frames/zNN.ready`
- 造片临时瓦片在烘焙后删除，以节省 LittleFS

## 性能

- 造片瓶颈仍是 HTTPS；烘焙阶段双线性 CPU 可接受
- 切档命中：行刷 RGB565，目标 &lt;50ms 量级（见串口 `blit zN Xms`）
