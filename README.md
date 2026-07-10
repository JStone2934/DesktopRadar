# 桌面气象雷达

在树莓派上将 **GC9A01 圆形 IPS 屏（240×240）** 与可选 **1602A I2C 字符屏** 组合，显示以当前位置为中心的多图层气象图、飞机雷达、短临预报，并支持远程 GPU 状态查询。
<img width="4096" height="3072" alt="5f2d9b2b9efe151d5fd62cbc57716e23" src="https://github.com/user-attachments/assets/9a167de9-d3ce-43b5-8259-5f699aef28ea" />

<img width="1835" height="1370" alt="image" src="https://github.com/user-attachments/assets/cca9febc-5ed4-47bf-96b6-3602aa7d1247" />

## 功能概览

| 模块 | 说明 |
|------|------|
| **圆屏 GC9A01** | 主显示：雷达、卫星、短临、ADSB 飞机雷达等图层 |
| **1602 LCD** | 操作提示：图层名、缩放、通道、GPU 状态等（背光自动熄灭） |
| **宏键盘** | 空格切图层/动画；组合键切通道；`0` 查 GPU |
| **旋钮** | 缩放 / 短临时间步进 / 飞机量程 / 经纬度设置 |
| **远程 GPU** | SSH 查询另一台机器的 `nvidia-smi`，显示双卡占用率与显存 |

### 两大通道

程序把图层分为 **天气** 与 **飞机** 两个通道，各自独立记忆当前图层：

| 通道 | 默认图层 | 切换方式 |
|------|----------|----------|
| **天气** | 雷达、FY-4B 中国/圆盘、短临 | `ctrl+c`（可配置） |
| **飞机** | ADSB 雷达四种风格 | `ctrl+v`（可配置） |

- **空格短按**：在当前通道内循环切换图层
- **空格长按**（≥500ms）：播放当前图层近 6 小时历史动画（松手停止；卫星图层按真实帧时间步进，不跳帧）
- **旋钮**：天气通道缩放；短临图层改预报时间；飞机通道改雷达量程

---

## 数据源与图层

可在 `config.json` 的 `layers` / `aircraft_layers` 中配置顺序与启用项。

| 图层 ID | 说明 | 历史时长 |
|---------|------|----------|
| `radar` | [RainViewer](https://www.rainviewer.com/) 雷达回波（默认，免 token） | ~2 小时 |
| `satellite_fy4b` | 国家卫星气象中心 FY-4B 中国区真彩色（15 分钟/帧） | ~72 小时 |
| `satellite_fy4b_disk` | FY-4B 全圆盘真彩色（DISK GCLR，GEOS 投影，可缩放） | ~72 小时 |
| `nowcast` | 中央气象台（NMC）文本天气（旋钮做时间步进 -1h~+8h） | 见「短临图层」 |
| `radar_caiyun` | 彩云雷达拼图（需 `caiyun_token`） | ~2 小时 |
| `adsb_radar` | 附近 ADSB 飞机雷达（纯雷达环，正北朝上） | 实时 |
| `adsb_map` | ADSB 雷达叠压暗地图底图 | 实时 |
| `adsb_outline` | ADSB 雷达叠绿色海岸线/国界轮廓 | 实时 |
| `adsb_sweep` | ADSB 雷达 + 老式 PPI 扫描余晖 | 实时 |

> **FY-4B 圆盘图**（`satellite_fy4b_disk`）每帧原始图约 16MB，首次拉取较慢（~10s）。原始图缓存在 `cache/disk_raw/`，渲染成品在 `cache/frames/`。

**定位**：默认公网 IP 自动定位（[ip-api.com](http://ip-api.com)），失败时回退 `config.json` 默认坐标；可启用 `manual_location` 持久化手动坐标。

**底图**：高德矢量瓦片（国内可达，最高约 z18），叠加时压暗以突出雷达。可用 `--no-basemap` 关闭。

**刷新间隔**：气象图层默认每 **300 秒** 更新一帧（`--interval`）；飞机图层约 **0.12s**（~8fps），扫描图层 **0.05s**（~20fps）。

---

## 硬件接线

### 3D打印外壳

<img width="1126" height="1249" alt="image" src="https://github.com/user-attachments/assets/a82fe59d-608f-4d1e-85df-5e452c7ddeb6" />


### GC9A01 圆屏（SPI，BCM 编号）

| 屏幕引脚 | 树莓派引脚 |
|---------|-----------|
| VCC | 3.3V（物理引脚 1 或 17）|
| GND | GND（物理引脚 9 等）|
| SCL | GPIO11 / SCLK（物理引脚 23）|
| SDA | GPIO10 / MOSI（物理引脚 19）|
| CS | GPIO8 / CE0（物理引脚 24）|
| DC | GPIO25（物理引脚 22）|
| RST | GPIO27（物理引脚 13）|

> 本模块背光内部常亮，无独立 BLK 引脚。VCC 接 3.3V。

启用 SPI：

```bash
sudo raspi-config   # Interface Options -> SPI -> Enable
ls /dev/spidev*     # 应看到 spidev0.0 / spidev0.1
```

### 1602A LCD（I2C，可选）

| LCD 引脚 | 树莓派引脚 |
|---------|-----------|
| VCC | 5V（物理引脚 2 或 4）|
| GND | GND（物理引脚 6）|
| SDA | GPIO2（物理引脚 3）|
| SCL | GPIO3（物理引脚 5）|

> VCC 必须接 **5V**。I2C 地址通常 `0x27` 或 `0x3F`，可用 `i2cdetect -y 1` 确认。

```bash
sudo raspi-config   # Interface Options -> I2C -> Enable
sudo apt install i2c-tools python3-smbus
```

背光点亮时长由 `lcd_backlight_seconds` 控制（默认 5 秒）。禁用：`--no-lcd`。

### 宏键盘与旋钮

- **宏键盘**：通过 USB 连接，程序用 `evdev` 读取 `/dev/input/event*`
- **旋钮**：识别为音量 +/- 与 MUTE 键的设备（排除 HDMI 虚拟音量）

systemd 服务需加入 `input` 组（见 `radar.service` 的 `SupplementaryGroups=input`）。

---

## 安装与运行

### 依赖

```bash
cd /path/to/gc9a01-display
pip3 install -r requirements.txt

# 系统包（如未安装）：
sudo apt install python3-lgpio fonts-dejavu-core python3-evdev
sudo apt install i2c-tools python3-smbus   # 1602 LCD 可选
```

### 屏幕测试

```bash
python3 test_display.py
```

### 运行主程序

```bash
# 常驻刷新（默认每 300 秒）
python3 radar_display.py

# 只显示一帧
python3 radar_display.py --once

# 手动指定坐标
python3 radar_display.py --lat 22.87 --lon 113.51 --once

# 调整缩放与刷新间隔
python3 radar_display.py --zoom 8 --interval 180
```

### 开机自启

```bash
sudo cp radar.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now radar.service

sudo systemctl status radar.service
journalctl -u radar.service -f
```

---

## 按键与操作

### 识别宏键盘键码

```bash
python3 radar_display.py --detect-keys
# 按下目标键，记下 KEY_* 名称，写入 config.json
```

### 通道切换（`channel_keys`）

```json
"channel_keys": {
  "weather": "ctrl+c",
  "aircraft": "ctrl+v"
}
```

支持单键或组合键（`ctrl+c`、`KEY_1` 等）。

### 空格键

| 操作 | 行为 |
|------|------|
| **短按** | 当前通道内循环切换图层 |
| **长按** | 播放近 6 小时历史动画（松手停止） |
| **GPU 亮屏期间短按** | 进入 GPU **连续查询模式**（见下） |
| **连续查询模式中短按** | 退出连续查询，恢复原有 LCD 内容 |

GPU 亮屏或连续查询期间，空格**不会**切图层，长按**不会**启动动画。

### 旋钮

| 上下文 | 旋转（音量+ / 音量-） | 短按（MUTE） | 长按（MUTE ≥500ms） |
|--------|----------------------|--------------|---------------------|
| 天气图层 | 缩放 +/- | 重置缩放 | 进入经纬度设置 |
| 短临图层 | 预报时间 +/- 30min | 回到当前实况 | 进入经纬度设置 |
| 飞机通道 | 量程缩小/放大 | 重置量程 | 进入经纬度设置 |
| 设置模式 | 调整当前字段 | 切换字段 | 保存并退出 |

缩放范围 **3~12**；飞机量程 **20/50/100/150/200/300 km**。

### 1602 LCD 提示内容

| 操作 | 第一行 | 第二行示例 |
|------|--------|-----------|
| 切换图层 | Layer | Radar / FY-4B CN / … |
| 缩放 | Zoom | z7 |
| 缩放复位 | Zoom Reset | z7 |
| 动画 | Animation | Play Radar / Stopped |
| 短临步进 | Nowcast +1.5h | 07-05 18:30 |
| 切换通道 | Channel | Weather Radar |
| 飞机量程 | ADSB Range | 100 km |
| 最近飞机 | Nearest | CCA742 12km |
| GPU 查询 | GPU Query | Please wait |
| GPU 结果 | GPU0 85% 12/32G | GPU1 42% 8/32G |

飞机通道：仅有飞机在量程内时才自动点亮 LCD 显示最近航班；无飞机时不亮屏。用户操作（切图层、调量程等）仍会点亮 LCD。

---

## 远程 GPU 状态（1602 LCD）

通过 SSH 在另一台机器上执行 `nvidia-smi`，在 1602 屏显示**双 GPU** 占用率与显存（每行一张卡）。

### 前置条件

树莓派需能**免密 SSH** 登录目标主机：

```bash
ssh -o BatchMode=yes js@10.7.162.172 \
  "nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits"
# 期望输出两行数字
```

### 配置（`config.json`）

```json
"action_keys": {
  "gpu_status": "0"
},
"ssh_gpu": {
  "host": "10.7.162.172",
  "user": "js",
  "timeout_sec": 5,
  "cache_ttl_sec": 3,
  "live_interval_sec": 2
}
```

| 字段 | 说明 |
|------|------|
| `action_keys.gpu_status` | 触发单次查询的按键（默认 `0`） |
| `host` / `user` | SSH 目标 |
| `timeout_sec` | SSH 连接超时 |
| `cache_ttl_sec` | 单次查询模式下的结果缓存（秒） |
| `live_interval_sec` | 连续查询模式的刷新间隔（秒） |

### 操作流程

1. 按 **0** → 单次查询，LCD 亮屏约 5 秒，显示例如：
   - 第 1 行：`GPU0 85% 12/32G`
   - 第 2 行：`GPU1 42% 8/32G`
2. 亮屏未熄灭前按 **空格** → 进入**连续查询**，按 `live_interval_sec` 不断刷新（绕过缓存）
3. 连续模式中再按 **空格** → 退出，LCD 文字恢复为查询前的内容并熄屏
4. 若未按空格，5 秒后自动熄屏并同样恢复原有文字

连续模式中按 **0** 可立即触发一次刷新。

每次查询使用一次性 `ssh` 短连接，执行完即断开，不会在服务器上残留长连接。

---

## 短临图层（NMC）

`nowcast` 从 [nmc.cn](http://www.nmc.cn/) 拉取最近国家站文本天气，圆屏以文字 + 图标展示。

- 旋钮：预报时间步进 **±30 分钟**，范围 **[-1h, +8h]**
- 过去/当前：逐小时实况；未来：逐日白天/夜间预报（半天粒度）
- `nmc_cache_ttl_sec`：数据缓存（默认 300 秒）

---

## 飞机雷达通道（ADSB）

数据来自免费 API（[adsb.lol](https://api.adsb.lol) / adsb.fi / airplanes.live），无需 API key。

| 图层 | 特点 |
|------|------|
| `adsb_radar` | 纯雷达环 + 航向箭头 |
| `adsb_map` | 叠暗色地图 |
| `adsb_outline` | 叠绿色海岸线/国界 |
| `adsb_sweep` | PPI 扫描余晖（真实扫描规则：仅余晖扇区内更新） |

前三种图层对飞机位置做航向/地速外推插值（~8fps 丝滑移动）。`adsb_ttl_sec` 控制数据缓存（默认 8 秒）。

---

## 经纬度设置（旋钮长按）

长按旋钮进入设置界面：高德地图 + 十字准星，旋转调整度/分/秒，短按切换字段，再长按保存。

**进入设置时**，始终从当前**全局坐标**（`manual_lat` / `manual_lon`）起算，不会跳到 FY-4B 等图层的旧独立中心。

| 当前图层 | 保存目标 | 写入字段 |
|----------|----------|----------|
| 雷达、短临等 | **全局坐标** | `manual_lat` / `manual_lon` / `manual_location`，并**同步** `fy4b_cn_*` / `fy4b_disk_*` |
| `satellite_fy4b` | FY-4B 中国区独立中心 | 仅 `fy4b_cn_lat` / `fy4b_cn_lon` |
| `satellite_fy4b_disk` | FY-4B 圆盘独立中心 | 仅 `fy4b_disk_lat` / `fy4b_disk_lon` |

> 在雷达图层保存广州坐标后，切到风云4B盘也会显示广州附近，不会再误跳到广西等旧坐标。启动时若独立中心与全局偏差超过 1°，会自动同步。

---

## 预渲染缓存与后台预取

后台 `CacheWorker` 线程预渲染各图层、各帧、各 zoom（默认 **z3–z12**）为 240×240 PNG，存于 `cache/frames/{图层ID}/{帧ID}/zNN.png`：

| 机制 | 说明 |
|------|------|
| **全局预取** | `prefetch_global: true` 时，后台按优先级预取全部天气图层 × 全部 zoom × 动画帧 |
| **蓝色进度圆环** | 预取进行中时在圆屏内缘显示顺时针进度弧（`cache_progress_ring`）；消失即表示当前批次完成 |
| **动画帧常驻内存** | 当前图层的 6 小时动画序列（全部 zoom）会 pin 在内存 LRU 中，避免播放时反复读盘；约 **60MB/图层** |
| **FY-4B 原始图** | 圆盘图层原始 JPEG 预下载至 `cache/disk_raw/`（每帧 ~16MB） |

日常使用：

- 旋钮缩放优先读内存/磁盘缓存，目标 **<50ms**（内存命中时）
- 每图层保留约 40 个帧目录（覆盖 6 小时动画）
- 瓦片 LRU 内存缓存（上限 2000）
- 长按动画前会同步预热；未落盘的帧仍需现场渲染，播放会自动降速但不跳时间

相关配置见下表 `prefetch_*` / `frame_cache_memory_max_mb` / `cache_progress_ring`。

## 命令行参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--lat` / `--lon` | 自动 | 手动指定坐标 |
| `--zoom` | 7 | 缩放级别 |
| `--interval` | 300 | 气象图层刷新间隔（秒） |
| `--once` | - | 只刷新一帧后退出 |
| `--layer` | 配置首项 | 起始图层 ID |
| `--no-basemap` | - | 不加载高德底图 |
| `--no-outline` | - | 不绘制海岸线轮廓 |
| `--no-knob` | - | 禁用旋钮 |
| `--no-keys` | - | 禁用键盘（空格、通道键、GPU 键） |
| `--no-anim` | - | 禁用长按动画 |
| `--no-lcd` | - | 禁用 1602 LCD |
| `--no-display` | - | 仅渲染不刷屏（调试） |
| `--detect-keys` | - | 识别按键码后退出 |

---

## 配置参考（`config.json`）

```json
{
  "default_lat": 39.9042,
  "default_lon": 116.4074,
  "default_city": "Beijing",
  "manual_location": true,
  "manual_lat": 22.866111,
  "manual_lon": 113.513611,
  "fy4b_disk_lat": 22.866111,
  "fy4b_disk_lon": 113.513611,
  "fy4b_cn_lat": 22.866111,
  "fy4b_cn_lon": 113.513611,
  "layers": ["radar", "satellite_fy4b", "satellite_fy4b_disk", "nowcast"],
  "aircraft_layers": ["adsb_radar", "adsb_map", "adsb_outline", "adsb_sweep"],
  "channel_keys": {
    "weather": "ctrl+c",
    "aircraft": "ctrl+v"
  },
  "action_keys": {
    "gpu_status": "0"
  },
  "ssh_gpu": {
    "host": "10.7.162.172",
    "user": "js",
    "timeout_sec": 5,
    "cache_ttl_sec": 3,
    "live_interval_sec": 2
  },
  "caiyun_token": "",
  "long_press_ms": 500,
  "anim_fps": 5,
  "anim_window_hours": 6,
  "prefetch_global": true,
  "prefetch_anim_frames": true,
  "prefetch_neighbor_layers": true,
  "prefetch_fy4b_disk_raw": true,
  "prefetch_zoom_min": 3,
  "prefetch_zoom_max": 12,
  "fy4b_disk_keep_raw": 48,
  "frame_cache_memory_max_mb": 512,
  "cache_progress_ring": true,
  "lcd_backlight_seconds": 5,
  "nmc_cache_ttl_sec": 300,
  "adsb_ttl_sec": 8,
  "adsb_default_range_km": 100
}
```

### 预取与缓存配置说明

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `prefetch_global` | `true` | 后台全局预取（全图层 × 全 zoom × 动画帧） |
| `prefetch_zoom_min` / `prefetch_zoom_max` | 3 / 12 | 预取与动画常驻内存的 zoom 范围 |
| `prefetch_anim_frames` | `true` | 预取当前图层动画序列 |
| `prefetch_neighbor_layers` | `true` | 预取同通道相邻图层最新帧 |
| `prefetch_fy4b_disk_raw` | `true` | 预下载 FY-4B 圆盘原始 JPEG |
| `frame_cache_memory_max_mb` | 512 | 预渲染成品图内存 LRU 上限（MB） |
| `cache_progress_ring` | `true` | 预取时在圆屏显示蓝色进度圆环 |
| `anim_fps` | 5 | 长按动画播放帧率 |
| `anim_window_hours` | 6 | 动画/预取历史窗口（小时） |
| `fy4b_disk_keep_raw` | 48 | 保留的圆盘原始 JPEG 帧数 |

---

## 项目结构

| 文件 | 说明 |
|------|------|
| `radar_display.py` | 主程序：图层渲染、输入控制、主循环 |
| `gc9a01.py` | GC9A01 SPI 圆屏驱动 |
| `lcd_i2c.py` | 1602A HD44780 + PCF8574 I2C 驱动 |
| `lcd_notifier.py` | 1602 异步显示、背光计时、临时/连续 overlay |
| `gpu_client.py` | SSH + nvidia-smi 远程 GPU 查询 |
| `adsb_client.py` | ADSB 飞机数据客户端 |
| `nmc_client.py` | 中央气象台短临天气客户端 |
| `test_display.py` | 圆屏色彩测试 |
| `config.json` | 运行时配置 |
| `radar.service` | systemd 自启单元 |
| `requirements.txt` | Python 依赖 |
| `cache/` | 瓦片与预渲染缓存（自动生成，勿提交） |

---

## 架构简图

```
输入 (evdev)                主循环                    输出
─────────────              ────────                  ──────
KnobController  ──►  AppState  ◄──  HTTP 客户端     GC9A01 圆屏
LayerKeyController         │      (RainViewer/NMC/   1602 LCD
  · 空格/通道键            │       ADSB/高德)
  · 0 → GpuClient ──SSH──► │      nvidia-smi
                           ▼
                    CacheWorker → cache/frames/
                         │              cache/disk_raw/
                         ▼
                  动画帧 pin（z3–z12）
```
