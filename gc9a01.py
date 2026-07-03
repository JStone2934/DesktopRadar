"""GC9A01 240x240 圆形 IPS 屏驱动（基于 lgpio，GPIO + 硬件 SPI）。

接线（BCM 编号）：
  VCC  -> 3.3V   (物理引脚 1 或 17)
  GND  -> GND    (物理引脚 9 等)
  SCL  -> GPIO11 (SCLK, 物理引脚 23)
  SDA  -> GPIO10 (MOSI, 物理引脚 19)
  CS   -> GPIO8  (CE0,  物理引脚 24)  由 spidev 自动控制
  DC   -> GPIO25 (物理引脚 22)
  RST  -> GPIO27 (物理引脚 13)
  （本模块无 BLK 引脚，背光内部常亮）
"""

import time
import lgpio

WIDTH = 240
HEIGHT = 240


class GC9A01:
    def __init__(self, dc=25, rst=27, bl=None, spi_bus=0, spi_ce=0,
                 baud=32_000_000, gpiochip=0):
        self.dc = dc
        self.rst = rst
        self.bl = bl

        self._chip = lgpio.gpiochip_open(gpiochip)
        lgpio.gpio_claim_output(self._chip, self.dc, 0)
        lgpio.gpio_claim_output(self._chip, self.rst, 1)
        if self.bl is not None:
            lgpio.gpio_claim_output(self._chip, self.bl, 0)

        self._spi = lgpio.spi_open(spi_bus, spi_ce, baud, 0)

        self.reset()
        self._init_display()
        self.backlight(True)

    # ---- 底层 ----
    def _cmd(self, c):
        lgpio.gpio_write(self._chip, self.dc, 0)
        lgpio.spi_write(self._spi, bytes([c]))

    def _data(self, data):
        lgpio.gpio_write(self._chip, self.dc, 1)
        if isinstance(data, int):
            data = bytes([data])
        # spidev 单次传输有大小限制，分块写
        mv = memoryview(data)
        step = 4096
        for i in range(0, len(mv), step):
            lgpio.spi_write(self._spi, bytes(mv[i:i + step]))

    def reset(self):
        lgpio.gpio_write(self._chip, self.rst, 1)
        time.sleep(0.05)
        lgpio.gpio_write(self._chip, self.rst, 0)
        time.sleep(0.05)
        lgpio.gpio_write(self._chip, self.rst, 1)
        time.sleep(0.12)

    def backlight(self, on):
        if self.bl is not None:
            lgpio.gpio_write(self._chip, self.bl, 1 if on else 0)

    # ---- 初始化序列（GC9A01 通用序列）----
    def _init_display(self):
        seq = [
            (0xEF, []),
            (0xEB, [0x14]),
            (0xFE, []), (0xEF, []),
            (0xEB, [0x14]),
            (0x84, [0x40]),
            (0x85, [0xFF]), (0x86, [0xFF]), (0x87, [0xFF]),
            (0x88, [0x0A]), (0x89, [0x21]), (0x8A, [0x00]),
            (0x8B, [0x80]), (0x8C, [0x01]), (0x8D, [0x01]),
            (0x8E, [0xFF]), (0x8F, [0xFF]),
            (0xB6, [0x00, 0x20]),
            (0x36, [0x08]),          # MADCTL: BGR
            (0x3A, [0x05]),          # 16 bit/pixel (RGB565)
            (0x90, [0x08, 0x08, 0x08, 0x08]),
            (0xBD, [0x06]),
            (0xBC, [0x00]),
            (0xFF, [0x60, 0x01, 0x04]),
            (0xC3, [0x13]), (0xC4, [0x13]),
            (0xC9, [0x22]),
            (0xBE, [0x11]),
            (0xE1, [0x10, 0x0E]),
            (0xDF, [0x21, 0x0C, 0x02]),
            (0xF0, [0x45, 0x09, 0x08, 0x08, 0x26, 0x2A]),
            (0xF1, [0x43, 0x70, 0x72, 0x36, 0x37, 0x6F]),
            (0xF2, [0x45, 0x09, 0x08, 0x08, 0x26, 0x2A]),
            (0xF3, [0x43, 0x70, 0x72, 0x36, 0x37, 0x6F]),
            (0xED, [0x1B, 0x0B]),
            (0xAE, [0x77]),
            (0xCD, [0x63]),
            (0x70, [0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03]),
            (0xE8, [0x34]),
            (0x62, [0x18, 0x0D, 0x71, 0xED, 0x70, 0x70,
                    0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70]),
            (0x63, [0x18, 0x11, 0x71, 0xF1, 0x70, 0x70,
                    0x18, 0x13, 0x71, 0xF3, 0x70, 0x70]),
            (0x64, [0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07]),
            (0x66, [0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45,
                    0x10, 0x00, 0x00, 0x00]),
            (0x67, [0x00, 0x3C, 0x00, 0x00, 0x00, 0x01,
                    0x54, 0x10, 0x32, 0x98]),
            (0x74, [0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00]),
            (0x98, [0x3E, 0x07]),
            (0x35, []), (0x21, []),
            (0x11, []),
        ]
        for cmd, data in seq:
            self._cmd(cmd)
            if data:
                self._data(bytes(data))
        time.sleep(0.12)
        self._cmd(0x29)          # display on
        time.sleep(0.02)

    # ---- 绘制 ----
    def set_window(self, x0, y0, x1, y1):
        self._cmd(0x2A)
        self._data(bytes([x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF]))
        self._cmd(0x2B)
        self._data(bytes([y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF]))
        self._cmd(0x2C)

    def fill(self, color565):
        hi = color565 >> 8
        lo = color565 & 0xFF
        self.set_window(0, 0, WIDTH - 1, HEIGHT - 1)
        lgpio.gpio_write(self._chip, self.dc, 1)
        line = bytes([hi, lo]) * WIDTH
        for _ in range(HEIGHT):
            lgpio.spi_write(self._spi, line)

    def display(self, image):
        """把一张 240x240 的 PIL 图像刷到屏上。"""
        import numpy as np

        image = image.convert("RGB")
        if image.size != (WIDTH, HEIGHT):
            image = image.resize((WIDTH, HEIGHT))
        arr = np.asarray(image, dtype=np.uint16)
        r, g, b = arr[..., 0], arr[..., 1], arr[..., 2]
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        # RGB565 大端字节序
        buf = rgb565.astype(">u2").tobytes()
        self.set_window(0, 0, WIDTH - 1, HEIGHT - 1)
        self._data(buf)

    def close(self):
        try:
            lgpio.spi_close(self._spi)
        except Exception:
            pass
        try:
            lgpio.gpiochip_close(self._chip)
        except Exception:
            pass


# 常用 RGB565 颜色
def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


BLACK = 0x0000
WHITE = 0xFFFF
RED = rgb565(255, 0, 0)
GREEN = rgb565(0, 255, 0)
BLUE = rgb565(0, 0, 255)
