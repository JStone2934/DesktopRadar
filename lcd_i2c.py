"""1602A LCD driver over I2C (PCF8574 backpack), HD44780 4-bit mode."""

import threading
import time

try:
    from smbus2 import SMBus
except ImportError:
    from smbus import SMBus

# Common PCF8574 addresses for LCD backpacks
LCD_ADDR_DEFAULT = 0x27
LCD_ADDR_ALT = 0x3F

# PCF8574 -> LCD pin bit mapping
LCD_BACKLIGHT = 0x08
LCD_ENABLE = 0x04  # E bit
LCD_RW = 0x02      # RW bit (always 0, write only)
LCD_RS = 0x01      # RS bit: 0 = command, 1 = data

MODE_CMD = 0x00
MODE_DATA = LCD_RS

# HD44780 commands
LCD_CLEAR = 0x01
LCD_HOME = 0x02
LCD_ENTRY_MODE = 0x06        # increment, no shift
LCD_DISPLAY_ON = 0x0C        # display on, cursor off, blink off
LCD_FUNCTION_SET = 0x28      # 4-bit, 2 line, 5x8 font


class LCD1602:
    def __init__(self, bus_num: int = 1, address: int = LCD_ADDR_DEFAULT):
        self.address = address
        self.bus = SMBus(bus_num)
        self.backlight = LCD_BACKLIGHT
        self._lock = threading.Lock()
        self._pwm_thread = None
        self._pwm_stop = threading.Event()
        time.sleep(0.05)
        self._init_lcd()

    def _write_expander(self, data: int) -> None:
        with self._lock:
            self.bus.write_byte(self.address, data | self.backlight)

    def _pulse(self, data: int) -> None:
        self._write_expander(data | LCD_ENABLE)
        time.sleep(0.0005)
        self._write_expander(data & ~LCD_ENABLE)
        time.sleep(0.0001)

    def _send_nibble(self, nibble: int, mode: int) -> None:
        """Send a single 4-bit nibble held in the HIGH 4 bits of `nibble`."""
        data = (nibble & 0xF0) | mode
        self._write_expander(data)
        self._pulse(data)

    def _send_byte(self, value: int, mode: int) -> None:
        self._send_nibble(value & 0xF0, mode)
        self._send_nibble((value << 4) & 0xF0, mode)

    def _command(self, cmd: int) -> None:
        self._send_byte(cmd, MODE_CMD)
        time.sleep(0.0001)

    def _data(self, value: int) -> None:
        self._send_byte(value, MODE_DATA)

    def _init_lcd(self) -> None:
        # HD44780 power-on init for 4-bit mode
        time.sleep(0.05)
        self._send_nibble(0x30, MODE_CMD)
        time.sleep(0.005)
        self._send_nibble(0x30, MODE_CMD)
        time.sleep(0.0002)
        self._send_nibble(0x30, MODE_CMD)
        time.sleep(0.0002)
        self._send_nibble(0x20, MODE_CMD)  # switch to 4-bit
        time.sleep(0.0002)

        self._command(LCD_FUNCTION_SET)
        self._command(LCD_DISPLAY_ON)
        self._command(LCD_ENTRY_MODE)
        self.clear()

    def clear(self) -> None:
        self._command(LCD_CLEAR)
        time.sleep(0.002)

    def home(self) -> None:
        self._command(LCD_HOME)
        time.sleep(0.002)

    def set_cursor(self, row: int, col: int) -> None:
        offsets = (0x80, 0xC0)
        self._command(offsets[row] + col)

    def write_string(self, text: str) -> None:
        for char in text:
            self._data(ord(char))

    def display(self, line1: str, line2: str = "") -> None:
        self.set_cursor(0, 0)
        self.write_string(line1[:16].ljust(16))
        self.set_cursor(1, 0)
        self.write_string(line2[:16].ljust(16))

    def set_backlight(self, on: bool) -> None:
        """Turn backlight fully on or off."""
        self.set_brightness(1.0 if on else 0.0)

    def _stop_pwm(self) -> None:
        if self._pwm_thread is not None:
            self._pwm_stop.set()
            self._pwm_thread.join()
            self._pwm_thread = None
            self._pwm_stop.clear()

    def set_brightness(self, level: float) -> None:
        """Set backlight brightness 0.0..1.0.

        The PCF8574 backpack has no hardware PWM, so intermediate levels are
        produced by software PWM (rapidly toggling the backlight bit over
        I2C). This may show slight flicker depending on bus load.
        """
        level = max(0.0, min(1.0, level))
        self._stop_pwm()

        if level >= 1.0:
            self.backlight = LCD_BACKLIGHT
            self._write_expander(0x00)
            return
        if level <= 0.0:
            self.backlight = 0x00
            self._write_expander(0x00)
            return

        # Software PWM. Keep command/data writes dark so they don't flash the
        # backlight fully on; the PWM thread owns the idle backlight state.
        self.backlight = 0x00
        self._pwm_stop.clear()
        self._pwm_thread = threading.Thread(
            target=self._pwm_loop, args=(level,), daemon=True
        )
        self._pwm_thread.start()

    def _pwm_loop(self, level: float) -> None:
        period = 0.02  # 50 Hz
        on_time = period * level
        off_time = period - on_time
        while not self._pwm_stop.is_set():
            with self._lock:
                self.bus.write_byte(self.address, LCD_BACKLIGHT)
            if self._pwm_stop.wait(on_time):
                break
            with self._lock:
                self.bus.write_byte(self.address, 0x00)
            if self._pwm_stop.wait(off_time):
                break

    def close(self) -> None:
        self._stop_pwm()
        self.bus.close()


class GpioBacklight:
    """Hardware-timed PWM backlight control via lgpio on a GPIO pin.

    Use when the LCD backpack backlight jumper is rewired to a GPIO pin
    (e.g. GPIO18). Produces flicker-free dimming, unlike I2C software PWM.
    """

    def __init__(self, gpio: int = 18, chip: int = 0, freq: int = 800):
        import lgpio

        self._lgpio = lgpio
        self.gpio = gpio
        self.freq = freq
        self.handle = lgpio.gpiochip_open(chip)
        lgpio.gpio_claim_output(self.handle, gpio)
        self.set_brightness(1.0)

    def set_brightness(self, level: float) -> None:
        """Set brightness 0.0..1.0."""
        duty = max(0.0, min(1.0, level)) * 100.0
        self._lgpio.tx_pwm(self.handle, self.gpio, self.freq, duty)

    def close(self) -> None:
        try:
            self._lgpio.tx_pwm(self.handle, self.gpio, self.freq, 0)
        except Exception:
            pass
        self._lgpio.gpiochip_close(self.handle)


def probe_address(bus_num: int = 1) -> int | None:
    """Return first responding LCD address, or None."""
    for addr in (LCD_ADDR_DEFAULT, LCD_ADDR_ALT):
        bus = SMBus(bus_num)
        try:
            bus.read_byte(addr)
            return addr
        except OSError:
            continue
        finally:
            bus.close()
    return None
