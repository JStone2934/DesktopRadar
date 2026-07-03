"""1602A LCD notifier: show operation hints with timed backlight."""

from __future__ import annotations

import queue
import threading
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from lcd_i2c import LCD1602

_SHUTDOWN = object()


def _ascii(text: str) -> str:
    return text.encode("ascii", "replace").decode()


class LcdNotifier:
    """Background LCD notifier with auto-dimming backlight."""

    def __init__(
        self,
        bus: int = 1,
        address: int | None = None,
        backlight_seconds: float = 5.0,
    ) -> None:
        self._backlight_seconds = max(0.1, float(backlight_seconds))
        self._queue: queue.Queue = queue.Queue()
        self._stop = threading.Event()
        self._timer: threading.Timer | None = None
        self._timer_lock = threading.Lock()
        self.lcd: LCD1602 | None = None
        self._worker: threading.Thread | None = None

        try:
            from lcd_i2c import LCD1602, probe_address
        except ImportError:
            print("未安装 smbus2/smbus，1602 LCD 功能禁用")
            return

        addr = address
        if addr is None:
            addr = probe_address(bus)
        if addr is None:
            print("未检测到 1602 LCD（I2C 0x27/0x3F），LCD 功能禁用")
            return

        try:
            self.lcd = LCD1602(bus_num=bus, address=addr)
            print(f"1602 LCD 已连接 (I2C 0x{addr:02X})")
        except OSError as exc:
            print(f"1602 LCD 初始化失败: {exc}")
            self.lcd = None
            return

        self._worker = threading.Thread(target=self._run, daemon=True)
        self._worker.start()

    @property
    def enabled(self) -> bool:
        return self.lcd is not None

    def notify(self, line1: str, line2: str = "") -> None:
        if not self.enabled:
            return
        self._queue.put((_ascii(line1), _ascii(line2)))

    def _reset_backlight_timer(self) -> None:
        with self._timer_lock:
            if self._timer is not None:
                self._timer.cancel()
            self._timer = threading.Timer(
                self._backlight_seconds,
                self._backlight_off,
            )
            self._timer.daemon = True
            self._timer.start()

    def _backlight_off(self) -> None:
        if self.lcd is None:
            return
        try:
            self.lcd.set_backlight(False)
        except OSError as exc:
            print(f"1602 背光关闭失败: {exc}")

    def _show(self, line1: str, line2: str) -> None:
        if self.lcd is None:
            return
        try:
            self.lcd.set_backlight(True)
            self.lcd.display(line1, line2)
            self._reset_backlight_timer()
        except OSError as exc:
            print(f"1602 显示失败: {exc}")

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                item = self._queue.get(timeout=0.5)
            except queue.Empty:
                continue
            if item is _SHUTDOWN:
                break

            line1, line2 = item
            while True:
                try:
                    next_item = self._queue.get_nowait()
                except queue.Empty:
                    break
                if next_item is _SHUTDOWN:
                    self._stop.set()
                    return
                line1, line2 = next_item

            self._show(line1, line2)

    def close(self) -> None:
        self._stop.set()
        self._queue.put(_SHUTDOWN)
        if self._worker is not None:
            self._worker.join(timeout=2.0)

        with self._timer_lock:
            if self._timer is not None:
                self._timer.cancel()
                self._timer = None

        if self.lcd is not None:
            try:
                self.lcd.set_backlight(False)
                self.lcd.close()
            except OSError:
                pass
            self.lcd = None
