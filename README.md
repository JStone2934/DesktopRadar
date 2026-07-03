# GC9A01 气象雷达图

在树莓派 GC9A01 圆形 IPS 屏（240×240）上显示以当前位置为中心的气象雷达图。

数据源：[RainViewer](https://www.rainviewer.com/)（全球免费、无需 API key）  
定位：公网 IP 自动定位（[ip-api.com](http://ip-api.com)），失败时回退到 `config.json` 默认坐标。

> 注：CartoDB 暗色底图在国内网络可能无法访问，程序会自动跳过底图、仅显示雷达层。可用 `--no-basemap` 显式关闭底图以加快首次加载。

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
sudo apt install python3-lgpio fonts-dejavu-core
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

## 命令行参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--lat` | 自动 | 手动指定纬度 |
| `--lon` | 自动 | 手动指定经度 |
| `--zoom` | 7 | 地图缩放级别（越大越近） |
| `--interval` | 300 | 刷新间隔（秒） |
| `--once` | - | 只刷新一次后退出 |
| `--no-basemap` | - | 不加载 CartoDB 暗色底图 |
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
| `radar.service` | systemd 服务单元 |
