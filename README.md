# GC9A01 多图层气象图

在树莓派 GC9A01 圆形 IPS 屏（240×240）上显示以当前位置为中心的多图层气象图，支持图层切换与历史动画。

**数据源（可在 `config.json` 中配置顺序）：**

| 图层 ID | 说明 | 历史时长 |
|---------|------|----------|
| `radar` | [RainViewer](https://www.rainviewer.com/) 雷达回波（默认，免 token） | ~2 小时 |
| `satellite_fy4b` | 国家卫星气象中心 FY-4B 中国区真彩色（15 分钟/帧） | ~72 小时 |
| `satellite_fy4b_disk` | FY-4B 全圆盘真彩色（DISK GCLR，GEOS 投影，以当前位置为中心、可缩放） | ~72 小时 |
| `nowcast` | 中央气象台（NMC）文本天气信息（文本+图标，旋钮做时间步进 -1h~+8h） | 见下方说明 |
| `radar_caiyun` | 彩云雷达拼图（需在 config 填写 `caiyun_token`） | ~2 小时 |

> `satellite_fy4b_disk` 使用 FY-4B 全圆盘原始大图（每帧约 16MB），首次拉取较慢（约 10 秒），
> 原始图缓存在 `cache/disk_raw/`，渲染后的成品图缓存在 `cache/frames/`。圆盘图动画首轮会逐帧下载，之后从缓存秒播。

定位：公网 IP 自动定位（[ip-api.com](http://ip-api.com)），失败时回退到 `config.json` 默认坐标。

> 底图使用高德矢量瓦片（国内可达，含路网/地名，最高约 z18），叠加时会压暗以突出雷达回波。不可用 `--no-basemap` 关闭。

## 接线（BCM 编号）

| 屏幕引脚 | 树莓派引脚 |
|---------|-----------|
| VCC     | 3.3V（物理引脚 1 或 17）|
| GND     | GND（物理引脚 9 等）|
| SCL     | GPIO11 / SCLK（物理引脚 23）|
| SDA     | GPIO10 / MOSI（物理引脚 19）|
| CS      | GPIO8 / CE0（物理引脚 24）|
| DC      | GPIO25（物理引脚 22）|
| RST     | GPIO27（物理引脚 13）|

> 本模块背光内部常亮，无独立 BLK 引脚。VCC 接 3.3V。

## 1602A LCD 操作提示（可选）

可额外连接一块 **1602A I2C 字符屏**（PCF8574 背包），在用户操作时点亮背光 5 秒并显示当前图层或操作（英文 ASCII）：

| 操作 | 第一行 | 第二行示例 |
|------|--------|-----------|
| 切换图层 | Layer | Radar / FY-4B CN / … |
| 旋钮缩放 | Zoom | z7 |
| 旋钮按下 | Zoom Reset | z7 |
| 长按动画 | Animation | Play Radar |
| 停止动画 | Animation | Stopped |
| 短临时间步进 | Nowcast +1.5h | 07-03 18:30 |
| 短临回当前 | Nowcast | Now |

### 接线（BCM / I2C）

| LCD 引脚 | 树莓派引脚 |
|---------|-----------|
| VCC | 5V（物理引脚 2 或 4）|
| GND | GND（物理引脚 6）|
| SDA | GPIO2（物理引脚 3）|
| SCL | GPIO3（物理引脚 5）|

> VCC 必须接 **5V**（不能 3.3V）。I2C 地址通常为 `0x27` 或 `0x3F`，可用 `i2cdetect -y 1` 确认。

启用 I2C：

```bash
sudo raspi-config   # Interface Options -> I2C -> Enable
# 或确认 /boot/firmware/config.txt 中有：
# dtparam=i2c_arm=on
```

背光点亮时长可在 `config.json` 的 `lcd_backlight_seconds` 调整（默认 5 秒）。禁用 LCD：`--no-lcd`。

## 启用 SPI

```bash
sudo raspi-config   # Interface Options -> SPI -> Enable
# 或确认 /boot/firmware/config.txt 中有：
# dtparam=spi=on
```

确认设备存在：

```bash
ls /dev/spidev*
# 应看到 /dev/spidev0.0 和 /dev/spidev0.1
```

## 安装依赖

```bash
cd /home/js/Project/gc9a01-display
pip3 install -r requirements.txt
# 系统包（如未安装）：
sudo apt install python3-lgpio fonts-dejavu-core python3-evdev
# 1602 LCD（可选）：
sudo apt install i2c-tools python3-smbus
```

## 屏幕测试

```bash
python3 test_display.py
```

## 运行雷达图

```bash
# 常驻刷新（默认每 300 秒）
python3 radar_display.py

# 只显示一帧
python3 radar_display.py --once

# 手动指定坐标
python3 radar_display.py --lat 31.23 --lon 121.47 --once

# 调整缩放与刷新间隔
python3 radar_display.py --zoom 8 --interval 180

# 不加载底图（仅雷达叠加层）
python3 radar_display.py --no-basemap
```

## 旋钮缩放（可选）

若连接了带旋钮的小键盘（旋钮通过 USB 发送音量键），程序会自动识别并支持旋钮缩放：

- 向上转（音量+）：放大（zoom +1）
- 向下转（音量-）：缩小（zoom -1）
- 按下旋钮（静音键）：重置为默认缩放

缩放范围 `3`~`12`，转动后立即重绘。需要 `python3-evdev`，且运行用户要能读取 `/dev/input/event*`。用 `--no-knob` 可禁用。

> **短临（nowcast）图层例外**：该图层为全国文本天气，不支持缩放，旋钮改为**预报时间步进**（见下）。

## 短临图层（中央气象台文本天气）

`nowcast` 图层不再使用 RainViewer 瓦片，而是从中央气象台（[nmc.cn](http://www.nmc.cn/)）拉取当前位置最近站点的文本天气，在圆屏上以**文字 + 天气图标**展示（天气现象、温度、降水、风、湿度）。定位通过 NMC `rest/position` 按经纬度解析到最近国家站。

在该图层下，**旋钮**语义变为预报时间步进：

- 向上转（音量+）：预报时间 **+30 分钟**
- 向下转（音量-）：预报时间 **-30 分钟**
- 按下旋钮（静音键）：回到**当前实况**

时间偏移在 **[-1h, +8h]** 内循环。圆屏与 1602 LCD 会标注请求的目标时间与数据实际时段。

数据粒度限制：
- **过去/当前**（-1h~0）：逐小时实况（`passedchart`/`real`）
- **未来**（+0.5h~+8h）：中央气象台逐日**白天/夜间**预报（半天粒度），落在同一时段的多个 30 分钟档会显示相同预报文字，圆屏顶部标注实际数据时段
- 境外坐标（如香港）会映射到最近的国内站点

空格键在该图层仍可切换到其它图层。`nmc_cache_ttl_sec` 控制 NMC 数据缓存秒数（默认 300）。

## 空格键图层切换与动画

宏键盘 **KEY_SPACE**（短按/长按）：

- **短按**：循环切换图层（雷达 → 风云4B中国区 → 风云4B圆盘 → 短临预报 → …）
- **长按**（默认 ≥500ms）：播放当前图层近 6 小时历史动画（松手停止）
- 动画帧率默认 5fps，可在 `config.json` 调整

禁用：`--no-keys` 或 `--no-anim`（仅禁用长按动画）。

## 预渲染磁盘缓存

为加快旋钮缩放响应，程序会在后台把每个图层、每个帧、每个 zoom 级别预渲染为 240×240 成品图，落盘到 `cache/frames/{图层ID}/{帧ID}/zNN.png`：

- **旋钮切换**：优先读内存热缓存或磁盘成品图，目标 **<50ms** 显示
- **后台预取**：首帧显示后按「当前 zoom → 相邻 zoom」优先级预渲染其余级别（仅支持缩放的图层）
- **自动清理**：每个图层保留最近约 40 个帧目录（覆盖 6 小时动画）
- **重启复用**：同一帧内重启进程仍可从磁盘秒开

瓦片下载使用 LRU 内存缓存（上限 2000）；底图可达性探测结果写入 `cache/basemap_ok`，重启跳过 3 秒超时。

## 命令行参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--lat` | 自动 | 手动指定纬度 |
| `--lon` | 自动 | 手动指定经度 |
| `--zoom` | 7 | 地图缩放级别（越大越近） |
| `--interval` | 300 | 刷新间隔（秒） |
| `--once` | - | 只刷新一次后退出 |
| `--no-basemap` | - | 不加载高德矢量底图 |
| `--no-outline` | - | 不绘制海岸线/国界轮廓 |
| `--no-knob` | - | 禁用旋钮缩放控制 |
| `--no-keys` | - | 禁用空格键图层/动画控制 |
| `--no-anim` | - | 禁用长按动画（仍可用短按切图层） |
| `--no-lcd` | - | 禁用 1602 LCD 操作提示 |
| `--layer` | 配置首项 | 起始图层 ID |
| `--no-display` | - | 仅下载渲染，不刷屏幕（调试） |

## 默认坐标配置

IP 定位失败时使用 `config.json`：

```json
{
  "default_lat": 39.9042,
  "default_lon": 116.4074,
  "default_city": "Beijing",
  "layers": ["radar", "satellite_fy4b", "satellite_fy4b_disk", "nowcast"],
  "caiyun_token": "",
  "long_press_ms": 500,
  "anim_fps": 5,
  "anim_window_hours": 6,
  "lcd_backlight_seconds": 5,
  "nmc_cache_ttl_sec": 300
}
```

- `layers`：启用图层及循环顺序
- `caiyun_token`：填写后可在 `layers` 中加入 `radar_caiyun`
- `long_press_ms`：长按判定毫秒数
- `anim_fps` / `anim_window_hours`：动画帧率与回溯小时数
- `lcd_backlight_seconds`：1602 LCD 操作提示背光点亮秒数
- `nmc_cache_ttl_sec`：短临图层的中央气象台数据缓存秒数

## 开机自启（可选）

```bash
sudo cp radar.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now radar.service
```

查看状态：

```bash
sudo systemctl status radar.service
journalctl -u radar.service -f
```

## 文件说明

| 文件 | 说明 |
|------|------|
| `gc9a01.py` | GC9A01 屏幕驱动 |
| `lcd_i2c.py` | 1602A I2C 字符屏驱动 |
| `lcd_notifier.py` | 1602 操作提示（背光计时） |
| `nmc_client.py` | 中央气象台文本天气客户端（短临图层） |
| `radar_display.py` | 多图层气象图主程序 |
| `test_display.py` | 屏幕色彩测试 |
| `config.json` | 默认经纬度 |
| `cache/frames/` | 预渲染成品图（自动生成，勿提交） |
| `radar.service` | systemd 服务单元 |
