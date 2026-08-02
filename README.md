# ESP32-C3 + GC9A01 桌面气象雷达

端上气象雷达摆件固件（见 [`docs/plan.md`](docs/plan.md)）。

**当前进度：** SoftAP / Web 配网（PSK + PEAP）已落地；高德底图 + RainViewer 静帧；BOOT 短按 z3–12；**RGB565 成品全档缓存**（秒切）；空闲按距离铺满；z≥8 雷达双线性上采样。HUD：中心白十字 + 红点、底栏北京时间 `WWW HH:MM`。历史动画尚未做。

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
| BOOT（短按切缩放；启动门户内短按跳过；≥10s 再进设置） | 9（板载，上拉） |

## 配网（上电 / RST 先进门户）

每次上电或按 **RST** 后，设备开 SoftAP **`Radar-Setup`**（开放），屏显步骤说明与缩小的 URL 二维码（约 60s 倒计时；**打开设置页后取消超时**，等点「保存并继续」）。

1. 手机「设置 → WLAN」手动连 `Radar-Setup`（不要指望扫码连热点）
2. 浏览器打开 `http://192.168.4.1`，或扫屏上 URL 码
3. 配置 **PSK** 或 **PEAP**（SSID + Identity + Password，不校验 CA）与经纬度 →「保存并继续」
4. 或：门户内 **短按 BOOT** / **超时** → 跳过，用 NVS（无则 `config.h` 默认）连网

运行中 **超长按 BOOT（≥10s）** 可再次进入同一门户。改经纬度会清空旧 zoom 成品缓存并重造。

## 编译与烧录

需安装 [PlatformIO](https://platformio.org/)。本机若无 `pio` 命令，可用 `py -m platformio`。

自定义分区（[`partitions.csv`](partitions.csv)）：app ≈1.5MB，LittleFS ≈**2.375MB**（约 10 档 ×115KB RGB565 + 造片余量）。**改分区后建议全擦再烧：**

```bash
cd esp32c3
py -m platformio run -t erase
py -m platformio run -t upload
py -m platformio device monitor      # 串口 115200
```

ESP32-C3 Super Mini 若上传失败：按住 **BOOT**，点一下 **RST**，松开 **BOOT** 后再 `upload`。

## 配置

运行时参数写入 NVS（`Preferences` namespace `radar`）。编译默认仍在 [`include/config.h`](include/config.h)：

| 宏 | 含义 |
|----|------|
| `WIFI_SSID` / `WIFI_PASS` | NVS 未保存时的默认 PSK |
| `MAP_LAT` / `MAP_LON` | NVS 未保存时的默认地图中心（广州） |
| `SOFTAP_SSID` / `CONFIG_PORTAL_*` | 热点名、管理 URL、门户超时 |
| `MAP_ZOOM` | 默认档（7） |
| `ZOOM_MIN` / `ZOOM_MAX` | 3 / 12 |
| `RAINVIEWER_MAX_ZOOM` | 7；更高档双线性放大 z7 雷达瓦片 |
| `TIMEZONE_OFFSET_SEC` | 雷达帧时间 → 底栏显示（默认 UTC+8 北京时间） |
| `RADAR_REFRESH_MS` | 全档重拉间隔（默认 5 分钟；失败后 `RADAR_REFRESH_RETRY_MS` 60s 重试） |
| `FRAME_CACHE_GEN` | 升高后启动清旧成品并重烘焙 |

## 显示（对齐 DesktopRadar 静帧 HUD）

| 元素 | 说明 |
|------|------|
| 白十字 | 中心臂长 ±8px，烘焙进成品帧 |
| 红点 | `(255,60,60)` 实心，半径约 3 |
| 底栏 | 黑底白字：`WED 14:35`（RainViewer 帧 Unix 时间 + 北京时区）；无时间则 `--- --:--` |
| 切档提示 | 顶部短暂 `zN`（不进成品帧） |
| 底图压暗 | 8-bit 域 `tile×0.35 + (15,20,30)×0.65`，减轻 RGB565 偏绿 |

HUD 与雷达一并写入 `/frames/zNN.rgb565`，秒切时直接刷缓存，不重画字。

## 操作

| 操作 | 行为 |
|------|------|
| 上电 / **RST** | 进入 SoftAP 配置门户（约 60s） |
| 门户内 BOOT **短按** | 跳过配置，用 NVS / 默认连网 |
| 运行中 BOOT **短按** | 循环 z3→…→z12；已缓存档 **Flash→SPI 秒切** |
| 运行中 BOOT **超长按**（≥10s） | 再次进入 SoftAP 配置 |
| 未缓存 | 下载瓦片 → 烘焙成品（可较慢） |
| 空闲 | 按距离预取全部 z3–12（首次约数分钟） |
| 约 5 分钟 | 作废其它档成品，先重建当前档，再后台预取其余档；失败约 60s 重试 |

## 缓存结构

- 成品：`/frames/zNN.rgb565`（115200 字节）+ `/frames/zNN.ready`
- 造片：**逐张** PNG→raw→贴图→立刻删除临时文件，避免 LittleFS 峰值撑满
- 启动 / 造片前：`frameCacheScrubOrphans()` 清失败残留的 png/raw/alpha，保留已 commit 成品
- 串口心跳含 `fs=used/total`；命中秒切见 `blit zN Xms`

若出现 `No more free space` 或秒切退化成反复 `Fetching...`，多为临时文件占满；新固件会自动回收，必要时 `erase` 后重烧。

## 性能

- 造片瓶颈仍是 HTTPS；烘焙阶段双线性 CPU 可接受
- 切档命中：行刷 RGB565，目标 &lt;50ms 量级（见串口 `blit zN Xms`）
