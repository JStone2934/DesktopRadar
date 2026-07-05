"""1602A LCD notifier: show operation hints with timed backlight."""

from __future__ import annotations

import queue
import threading
from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from lcd_i2c import LCD1602

_SHUTDOWN = object()


@dataclass(frozen=True)
class _LcdMsg:
    line1: str
    line2: str
    transient: bool = False
    live: bool = False


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
        self._baseline = ("", "")
        self._transient_active = False
        self._live_mode = False
        self._backlight_on = False
        self._restore_snapshot = ("", "")
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
        line1 = _ascii(line1)
        line2 = _ascii(line2)
        self._baseline = (line1, line2)
        if not self.enabled:
            return
        self._queue.put(_LcdMsg(line1, line2, transient=False))

    def notify_transient(self, line1: str, line2: str = "") -> None:
        """Show temporary content; restore baseline text when backlight dims."""
        if not self.enabled:
            return
        self._queue.put(_LcdMsg(_ascii(line1), _ascii(line2), transient=True))

    def notify_live(self, line1: str, line2: str = "") -> None:
        """Update live overlay; keep backlight on without auto-restore timer."""
        if not self.enabled:
            return
        self._queue.put(_LcdMsg(_ascii(line1), _ascii(line2), live=True))

    def is_transient_lit(self) -> bool:
        """True while a transient GPU overlay is lit and not yet in live mode."""
        return (
            self.enabled
            and self._transient_active
            and self._backlight_on
            and not self._live_mode
        )

    def enter_live_mode(self) -> bool:
        """Cancel transient dim timer and keep refreshing until exit_live_mode."""
        if not self.enabled or not self._transient_active:
            return False
        self._cancel_backlight_timer()
        self._transient_active = False
        self._live_mode = True
        return True

    def exit_live_mode(self) -> None:
        """Leave live mode and restore the pre-overlay baseline (backlight off)."""
        if not self.enabled:
            return
        self._live_mode = False
        self._apply_lines(*self._restore_snapshot, backlight=False)

    def backlight_off(self) -> None:
        """Turn off backlight; restore baseline if a transient overlay is active."""
        if not self.enabled:
            return
        if self._live_mode:
            self.exit_live_mode()
            return
        self._cancel_backlight_timer()
        if self._transient_active:
            self._apply_lines(*self._restore_snapshot, backlight=False)
            self._transient_active = False
        else:
            self._backlight_off_only()

    def _cancel_backlight_timer(self) -> None:
        with self._timer_lock:
            if self._timer is not None:
                self._timer.cancel()
                self._timer = None

    def _schedule_backlight_timer(self, *, restore_baseline: bool) -> None:
        if restore_baseline:
            snapshot = self._baseline
            self._restore_snapshot = snapshot
            self._transient_active = True

            def _on_timeout(
                line1: str = snapshot[0],
                line2: str = snapshot[1],
            ) -> None:
                if self._live_mode:
                    return
                self._apply_lines(line1, line2, backlight=False)
                self._transient_active = False

        else:
            self._transient_active = False

            def _on_timeout() -> None:
                self._backlight_off_only()

        with self._timer_lock:
            if self._timer is not None:
                self._timer.cancel()
            self._timer = threading.Timer(
                self._backlight_seconds,
                _on_timeout,
            )
            self._timer.daemon = True
            self._timer.start()

    def _apply_lines(self, line1: str, line2: str, *, backlight: bool) -> None:
        if self.lcd is None:
            return
        try:
            self.lcd.display(line1, line2)
            self.lcd.set_backlight(backlight)
            self._backlight_on = backlight
        except OSError as exc:
            print(f"1602 显示失败: {exc}")

    def _backlight_off_only(self) -> None:
        if self.lcd is None:
            return
        try:
            self.lcd.set_backlight(False)
            self._backlight_on = False
        except OSError as exc:
            print(f"1602 背光关闭失败: {exc}")

    def _show(self, msg: _LcdMsg) -> None:
        if self.lcd is None:
            return
        try:
            if msg.live:
                if not self._live_mode:
                    return
                self._cancel_backlight_timer()
                self.lcd.set_backlight(True)
                self.lcd.display(msg.line1, msg.line2)
                self._backlight_on = True
                return
            self.lcd.set_backlight(True)
            self.lcd.display(msg.line1, msg.line2)
            self._backlight_on = True
            self._schedule_backlight_timer(restore_baseline=msg.transient)
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

            msg = item
            while True:
                try:
                    next_item = self._queue.get_nowait()
                except queue.Empty:
                    break
                if next_item is _SHUTDOWN:
                    self._stop.set()
                    return
                msg = next_item

            self._show(msg)

    def close(self) -> None:
        self._stop.set()
        self._queue.put(_SHUTDOWN)
        if self._worker is not None:
            self._worker.join(timeout=2.0)

        self._cancel_backlight_timer()

        if self.lcd is not None:
            try:
                self.lcd.set_backlight(False)
                self.lcd.close()
            except OSError:
                pass
            self.lcd = None
