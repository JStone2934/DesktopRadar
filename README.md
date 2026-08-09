# ESP32-C3 + GC9A01 桌面气象雷达

ESP32-C3 Super Mini 驱动 GC9A01 240x240 圆屏的桌面气象雷达固件。设备通过 SoftAP 网页配网，联网后下载高德底图和 RainViewer 雷达图，端上合成 RGB565 成品帧并缓存到 LittleFS，已缓存缩放档可以秒切。

当前能力：

- SoftAP / Web 配网：普通密码 WiFi、PEAP/MSCHAPv2 企业 WiFi、开放网络 / MAC 白名单。
- PEAP 连接会扫描同名 SSID，锁定最强 WPA2 Enterprise BSSID 和信道，避免 ESP32-C3 自动选到弱 AP。
- 上电或按下 R键后默认先进入 3 分钟配置门户；短按 S键或超时后继续使用已保存配置。
- 高德底图 + RainViewer 静帧，HUD 包含中心白十字、红点、底栏北京时间。
- z3-z12 全档 RGB565 成品缓存；已缓存档位 Flash -> SPI 秒切。
- 后台空闲预取相邻缩放档；进度条单调递增，不回撤。

更完整的路线说明见 [docs/plan.md](docs/plan.md)。

## 硬件

| 屏幕引脚 | ESP32-C3 GPIO |
|----------|---------------|
| RST | 0 |
| CS | 1 |
| DC | 2 |
| SDA / MOSI | 3 |
| SCL / SCLK | 4 |
| VCC | 3.3V |
| GND | GND |

| 按键 | GPIO |
|------|------|
| S键 | 9（板载，上拉） |

备注：

- 若屏幕无独立 BLK 引脚，背光常亮即可。
- SPI 写入频率默认 20 MHz，飞线较长时更稳；稳定后可在 [include/config.h](include/config.h) 中调高。

## 快速开始

推荐使用仓库内的 Conda 环境：

```bash
conda env create -f environment.yml
conda activate radar-esp32c3
pio run
pio run -t upload
```

如果同时接了多个串口设备，指定端口：

```bash
pio run -t upload --upload-port /dev/cu.usbmodem101
pio device monitor --port /dev/cu.usbmodem101 --baud 115200
```

ESP32-C3 Super Mini 上传失败时，可按住 S键，点一下 R键，松开 S键后再次烧录。

## 配网流程

每次上电或按 R键后，设备先开启 WPA2 SoftAP：

| 项 | 值 |
|----|----|
| 热点名 | `RadarSetup` |
| 密码 | `radar1234` |
| 配置页 | `http://192.168.4.1` |
| 默认倒计时 | 3 分钟 |

步骤：

1. 手机或电脑连接 `RadarSetup`。
2. 浏览器打开 `http://192.168.4.1`，也可以扫屏幕上的网址二维码。
3. 从扫描建议选择网络，或手动输入 SSID。
4. 明确选择认证类型：普通密码 WiFi、企业 WiFi（PEAP/MSCHAPv2）、开放网络 / MAC 白名单。
5. 填写经纬度和显示选项，点击“保存并继续”。

门户行为：

- 打开配置页或手机保持连接热点后，倒计时会暂停，避免填表时自动关闭。
- 门户内短按 S键可跳过配置，使用 NVS 中已保存的配置；没有保存过则使用 [include/config.h](include/config.h) 默认值。
- 运行中长按 S键约 10 秒可重新进入配置门户。
- 修改经纬度后会清空旧位置的成品帧缓存并重新生成。

## PEAP / 校园网说明

PEAP 配置项：

| 字段 | 说明 |
|------|------|
| SSID | 企业 WiFi 名称，例如 `HKUSTGZ` |
| PEAP 用户名 | MSCHAPv2 内层用户名 |
| Password | PEAP 用户密码 |
| 外层 Identity | 可选；留空则使用 PEAP 用户名 |

当前固件不校验 CA 证书，适合“用户名 + 密码”的 PEAP/MSCHAPv2 场景。

ESP32-C3 在企业 WiFi 上容易自动选到弱 BSSID，导致认证阶段 `TIMEOUT` 或 `AUTH_EXPIRE`。固件连接前会扫描同名 SSID 的所有 AP，选择最强的 WPA2 Enterprise AP 并锁定 BSSID/channel 后再认证。串口日志会打印候选 AP、RSSI、选中的 BSSID 和失败 reason。

如果学校要求智能设备走 IoT 白名单网络，可申请设备 MAC 白名单后，在门户选择“开放网络 / MAC 白名单”。当前板卡 MAC：

```text
B8:1F:3F:0C:7A:A0
```

## 操作

| 操作 | 行为 |
|------|------|
| 上电 / R键 | 进入 3 分钟 SoftAP 配置门户 |
| 门户内 S键短按 | 跳过门户，用保存配置连网 |
| 运行中 S键短按 | 切换缩放档 z3 -> z4 -> ... -> z12 |
| 运行中 S键长按约 10 秒 | 重新进入配置门户 |
| 已缓存档位 | 直接刷 RGB565 成品帧，秒切 |
| 未缓存档位 | 下载瓦片、解码、合成、写缓存，耗时较长 |

## 显示与缓存

显示内容：

- 中心白十字和红点。
- 底栏北京时间，格式如 `WED 14:35`；无雷达帧时间时显示 `--- --:--`。
- 底图压暗混合，降低 RGB565 下的刺眼和偏色。
- 底栏上沿显示细进度条；进度条只前进，不回撤，完成后隐藏。

缓存结构：

- 成品帧：`/frames/zNN.rgb565`，每档 115200 字节。
- 就绪标记：`/frames/zNN.ready`。
- 临时文件：下载 PNG 后流式解码为 raw/alpha，贴图后立即删除。
- 启动和造片前会清理失败残留，保留已 commit 的成品帧。

首次启动或清空缓存后会比较慢。z7 首屏通常要下载 4 张底图和 4 张雷达图，再完成 PNG 解码、合成和写入 Flash。已缓存后切档会快很多。

## 配置项

主要编译期配置在 [include/config.h](include/config.h)：

| 宏 | 含义 |
|----|------|
| `WIFI_SSID` / `WIFI_PASS` | NVS 未保存时的默认普通 WiFi |
| `WIFI_CONNECT_TIMEOUT_MS` | 普通 WiFi 单次连接超时 |
| `SOFTAP_SSID` / `SOFTAP_PASS` | 配置门户热点名和密码 |
| `CONFIG_PORTAL_TIMEOUT_MS` | 门户倒计时，当前为 180000 ms |
| `MAP_LAT` / `MAP_LON` | 默认地图中心 |
| `MAP_ZOOM` | 默认缩放档 |
| `ZOOM_MIN` / `ZOOM_MAX` | 缩放档范围，当前 z3-z12 |
| `RAINVIEWER_MAX_ZOOM` | RainViewer 免费雷达瓦片最大原生 zoom |
| `RADAR_REFRESH_MS` | 雷达刷新周期，默认 5 分钟 |
| `RADAR_REFRESH_RETRY_MS` | 刷新失败后的重试间隔 |
| `FRAME_CACHE_GEN` | 提升后使旧缓存失效并重建 |

运行时配置写入 NVS，namespace 为 `radar`。普通更新固件不会清除 NVS；`pio run -t erase` 会清除已保存 WiFi 和坐标。

## 分区

使用自定义 [partitions.csv](partitions.csv)：

| 区域 | 大小 |
|------|------|
| app0 | 0x1B0000，约 1.69 MiB |
| LittleFS | 0x230000，约 2.19 MiB |
| coredump | 0x10000 |

正常更新直接烧录即可。只有文件系统损坏或需要清空所有 NVS/缓存时才全擦：

```bash
pio run -t erase
pio run -t upload
```

## 常见问题

**一直 WiFi fail**

先看串口 reason。PEAP 下常见原因：

- `TIMEOUT` / `AUTH_EXPIRE` 且 RSSI 很低：多半选到了弱 AP。新固件会锁定最强 BSSID；如果仍失败，尝试换位置或使用 IoT 白名单网络。
- `AUTH_FAIL` 且持续失败：用户名、密码、外层 Identity 或账号权限可能不对。
- 扫描不到企业 AP：确认 SSID 拼写、位置和 2.4 GHz 覆盖。

**加载很慢**

首次造片慢是正常的：ESP32-C3 需要下载、解码、合成并写入 Flash。已缓存档位应明显更快。后台预取时如果连续内存不足，串口可能出现 `frame malloc fail`，固件会冷却后重试。

**进度条回撤**

已修复。显示层会忽略小于当前可见进度的值，避免后台预取或失败重试造成视觉倒退。

**配置页没有自动弹出**

这是预期行为。固件不启用强制门户或通配 DNS，请手动打开 `http://192.168.4.1` 或扫屏幕上的网址码。

**上传失败**

按住 S键，点 R键，松开 S键后重新上传；必要时指定 `--upload-port`。

## 开发备注

- 项目使用 Arduino framework，PlatformIO 平台固定为 `platformio/espressif32@6.12.0`。
- `src/services/wifi_sta.cpp` 是 WiFi/PEAP 连接核心。
- `src/services/config_portal.cpp` 是 SoftAP 配网页面。
- `src/render/compose.cpp` 负责下载瓦片、解码和合成 RGB565。
- `src/storage/frame_cache.cpp` 负责 LittleFS 成品帧缓存。
