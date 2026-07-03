# GC9A01 气象雷达图

在树莓派 GC9A01 圆形 IPS 屏（240×240）上显示以当前位置为中心的气象雷达图。

数据源：[RainViewer](https://www.rainviewer.com/)（全球免费、无需 API key）  
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

缩放范围 `3`~`12`，转动后立即重绘。需要 `python3-evdev`，且运行用户要能读取 `/dev/input/event*`（服务单元已通过 `SupplementaryGroups=input` 授权；交互运行可执行 `sudo usermod -aG input $USER` 后重新登录）。用 `--no-knob` 可禁用。

## 预渲染磁盘缓存

为加快旋钮缩放响应，程序会在后台把每个雷达帧、每个 zoom 级别（z3~z12）预渲染为 240×240 成品图，落盘到 `cache/frames/{雷达帧ID}/zNN.png`：

- **旋钮切换**：优先读内存热缓存或磁盘成品图，目标 **<50ms** 显示
- **后台预取**：首帧显示后按「当前 zoom → 相邻 zoom」优先级预渲染其余级别
- **自动清理**：只保留最近 2 个雷达帧目录；新雷达帧到达时删除更旧的
- **重启复用**：同一雷达帧内重启进程仍可从磁盘秒开

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
| `--no-display` | - | 仅下载渲染，不刷屏幕（调试） |

## 默认坐标配置

IP 定位失败时使用 `config.json`：

```json
{
  "default_lat": 39.9042,
  "default_lon": 116.4074,
  "default_city": "Beijing"
}
```

按你的实际位置修改即可。

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
| `radar_display.py` | 气象雷达主程序 |
| `test_display.py` | 屏幕色彩测试 |
| `config.json` | 默认经纬度 |
| `cache/frames/` | 预渲染成品图（自动生成，勿提交） |
| `radar.service` | systemd 服务单元 |
