#!/usr/bin/env python3
"""GC9A01 屏幕测试：依次显示红/绿/蓝/白/黑 + 彩色圆环。"""

import sys
import time
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gc9a01 import GC9A01, BLACK, WHITE, RED, GREEN, BLUE


def main():
    print("初始化 GC9A01 ...")
    lcd = GC9A01()
    print("初始化完成，开始测试")

    colors = [
        ("红色", RED),
        ("绿色", GREEN),
        ("蓝色", BLUE),
        ("白色", WHITE),
        ("黑色", BLACK),
    ]
    for name, color in colors:
        print(f"  显示 {name} ...")
        lcd.fill(color)
        time.sleep(1.2)

    # 彩色圆环 + 文字
    print("  显示彩色圆环 ...")
    img = Image.new("RGB", (240, 240), (0, 0, 0))
    draw = ImageDraw.Draw(img)
    cx, cy, r = 120, 120, 110
    for i in range(360):
        hue = i / 360.0
        # HSV -> RGB 简易
        h6 = hue * 6
        c = 1.0
        x = h6 % 2 - 1
        if h6 < 1:
            rgb = (c, x, 0)
        elif h6 < 2:
            rgb = (x, c, 0)
        elif h6 < 3:
            rgb = (0, c, x)
        elif h6 < 4:
            rgb = (0, x, c)
        elif h6 < 5:
            rgb = (x, 0, c)
        else:
            rgb = (c, 0, x)
        col = tuple(int(v * 255) for v in rgb)
        draw.arc([cx - r, cy - r, cx + r, cy + r], i, i + 2, fill=col, width=8)

    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 22)
    except OSError:
        font = ImageFont.load_default()

    draw.text((48, 108), "GC9A01 OK", fill=(255, 255, 255), font=font)
    lcd.display(img)

    print("测试完成！屏幕应显示彩色圆环和 \"GC9A01 OK\" 字样。")
    print("按 Ctrl+C 退出（屏幕保持最后画面）。")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n退出")
    finally:
        lcd.close()


if __name__ == "__main__":
    main()
