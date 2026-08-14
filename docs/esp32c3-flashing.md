# ESP32-C3 烧录命令

在项目根目录执行：

```bash
/Users/js/.platformio/penv/bin/pio run -e esp32-c3-supermini -t upload --upload-port /dev/cu.usbmodem101
```

说明：

- `esp32-c3-supermini` 是当前使用的 PlatformIO 构建环境。
- `/dev/cu.usbmodem101` 是本机当前识别到的 ESP32-C3 串口。
- 如果设备串口变化，需要把 `--upload-port` 后面的路径替换成新的串口。
