# GC9A01 多图层气象图

在树莓派 GC9A01 圆形 IPS 屏（240×240）上显示以当前位置为中心的多图层气象图，支持图层切换与历史动画。

**数据源（可在 `config.json` 中配置顺序）：**

| 图层 ID | 说明 | 历史时长 |
|---------|------|----------|
| `radar` | [RainViewer](https://www.rainviewer.com/) 雷达回波（默认，免 token） | ~2 小时 |
| `satellite_fy4b` | 国家卫星气象中心 FY-4B 中国区真彩色（15 分钟/帧） | ~72 小时 |
| `satellite_gibs` | NASA GIBS Himawari 红外（墨卡托瓦片，10 分钟/帧） | ~6 小时 |
| `nowcast` | RainViewer 降水短临预报（有降雨时才有帧） | 未来数小时 |
| `radar_caiyun` | 彩云雷达拼图（需在 config 填写 `caiyun_token`） | ~2 小时 |

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

## 空格键图层切换与动画

宏键盘 **KEY_SPACE**（短按/长按）：

- **短按**：循环切换图层（雷达 → 风云4B → GIBS卫星 → 短临预报 → …）
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
| `--layer` | 配置首项 | 起始图层 ID |
| `--no-display` | - | 仅下载渲染，不刷屏幕（调试） |

## 默认坐标配置

IP 定位失败时使用 `config.json`：

```json
{
  "default_lat": 39.9042,
  "default_lon": 116.4074,
  "default_city": "Beijing",
  "layers": ["radar", "satellite_fy4b", "satellite_gibs", "nowcast"],
  "caiyun_token": "",
  "long_press_ms": 500,
  "anim_fps": 5,
  "anim_window_hours": 6
}
```

- `layers`：启用图层及循环顺序
- `caiyun_token`：填写后可在 `layers` 中加入 `radar_caiyun`
- `long_press_ms`：长按判定毫秒数
- `anim_fps` / `anim_window_hours`：动画帧率与回溯小时数

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
| `radar_display.py` | 多图层气象图主程序 |
| `test_display.py` | 屏幕色彩测试 |
| `config.json` | 默认经纬度 |
| `cache/frames/` | 预渲染成品图（自动生成，勿提交） |
| `radar.service` | systemd 服务单元 |
