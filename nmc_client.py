"""中央气象台（NMC）文本天气数据客户端。

通过 nmc.cn 的 rest 接口获取实况与逐日预报，供 Nowcast 图层按时间偏移
（-1h ~ +8h）选取最接近的文本天气报告。不下载 NMC 地图图片。
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from datetime import datetime, timedelta

import requests

NMC_BASE = "http://www.nmc.cn"
POSITION_URL = NMC_BASE + "/rest/position"
WEATHER_URL = NMC_BASE + "/rest/weather"
NMC_HEADERS = {
    "User-Agent": "Mozilla/5.0 (X11; Linux aarch64) gc9a01-radar-display/2.0",
    "Referer": "http://www.nmc.cn/",
}

MISSING = "9999"

# NMC 天气代码（img 字段）-> 图标大类
WEATHER_ICON_CATEGORIES = {
    "sunny": {"0", "33"},
    "cloudy": {"1"},
    "overcast": {"2"},
    "rain": {"3", "7", "8", "9", "10", "11", "12", "19", "21", "22", "23",
             "24", "25"},
    "thunder": {"4", "5"},
    "snow": {"6", "13", "14", "15", "16", "17", "26", "27", "28"},
    "fog": {"18", "32", "49", "57"},
    "dust": {"20", "29", "30", "31", "53"},
}


def weather_icon_category(code: str | None) -> str:
    if not code:
        return "unknown"
    code = str(code).strip()
    for cat, codes in WEATHER_ICON_CATEGORIES.items():
        if code in codes:
            return cat
    return "unknown"


@dataclass
class NmcReport:
    target_time: datetime
    valid_label: str
    weather_code: str
    weather_text: str
    temperature: str
    precip: str
    wind: str
    humidity: str | None
    is_forecast: bool

    @property
    def icon(self) -> str:
        return weather_icon_category(self.weather_code)


class NmcError(RuntimeError):
    pass


class NmcClient:
    """按站点缓存 NMC 天气 JSON，并按时间偏移选取文本报告。"""

    def __init__(
        self,
        session: requests.Session | None = None,
        weather_ttl_sec: float = 300.0,
        station_ttl_sec: float = 3600.0,
    ) -> None:
        self._session = session or requests.Session()
        self._weather_ttl = weather_ttl_sec
        self._station_ttl = station_ttl_sec
        self._lock = threading.Lock()
        self._station_cache: tuple[float, str] | None = None
        self._weather_cache: dict[str, tuple[float, dict]] = {}

    def resolve_station(self, lat: float, lon: float) -> str:
        now = time.time()
        with self._lock:
            if self._station_cache and now - self._station_cache[0] < self._station_ttl:
                return self._station_cache[1]
        resp = self._session.get(
            POSITION_URL,
            params={"lat": f"{lat:.4f}", "lon": f"{lon:.4f}"},
            headers=NMC_HEADERS,
            timeout=10,
        )
        resp.raise_for_status()
        data = resp.json()
        code = data.get("code")
        if not code:
            raise NmcError(f"NMC 定位无结果: {data}")
        with self._lock:
            self._station_cache = (now, code)
        return code

    def fetch_weather(self, station_id: str) -> dict:
        now = time.time()
        with self._lock:
            cached = self._weather_cache.get(station_id)
            if cached and now - cached[0] < self._weather_ttl:
                return cached[1]
        resp = self._session.get(
            WEATHER_URL,
            params={"stationid": station_id},
            headers=NMC_HEADERS,
            timeout=12,
        )
        resp.raise_for_status()
        payload = resp.json()
        data = payload.get("data")
        if not isinstance(data, dict):
            raise NmcError(f"NMC 天气数据异常: {payload.get('msg')}")
        with self._lock:
            self._weather_cache[station_id] = (now, data)
        return data

    def pick_report(
        self,
        lat: float,
        lon: float,
        offset_min: int,
        now: datetime | None = None,
    ) -> NmcReport:
        now = now or datetime.now().astimezone()
        target = now + timedelta(minutes=offset_min)
        station = self.resolve_station(lat, lon)
        data = self.fetch_weather(station)
        if offset_min <= 0:
            return self._report_from_observation(data, target, offset_min, now)
        return self._report_from_forecast(data, target)

    def _report_from_observation(
        self,
        data: dict,
        target: datetime,
        offset_min: int,
        now: datetime,
    ) -> NmcReport:
        real = data.get("real", {})
        if offset_min == 0:
            weather = real.get("weather", {})
            wind = real.get("wind", {})
            pub = real.get("publish_time", "")
            label = f"实况 {pub[-5:]}" if pub else "实况"
            rain = weather.get("rain")
            return NmcReport(
                target_time=target,
                valid_label=label,
                weather_code=str(weather.get("img", "")),
                weather_text=str(weather.get("info", "")),
                temperature=_fmt_temp(weather.get("temperature")),
                precip=_fmt_rain(rain),
                wind=_fmt_wind(wind.get("direct"), wind.get("power")),
                humidity=_fmt_humidity(weather.get("humidity")),
                is_forecast=False,
            )

        chart = data.get("passedchart", []) or []
        best = _nearest_by_time(chart, target)
        if best is None:
            return self._report_from_observation(data, target, 0, now)
        t = _parse_dt(best.get("time"), target)
        label = f"实况 {t.strftime('%H:%M')}" if t else "实况"
        return NmcReport(
            target_time=target,
            valid_label=label,
            weather_code="",
            weather_text="",
            temperature=_fmt_temp(best.get("temperature")),
            precip=_fmt_rain(best.get("rain1h")),
            wind=_fmt_wind_deg(best.get("windDirection"), best.get("windSpeed")),
            humidity=_fmt_humidity(best.get("humidity")),
            is_forecast=False,
        )

    def _report_from_forecast(self, data: dict, target: datetime) -> NmcReport:
        predict = data.get("predict", {})
        details = predict.get("detail", []) or []
        by_date = {d.get("date"): d for d in details if d.get("date")}

        is_day = 8 <= target.hour < 20
        if target.hour < 8:
            slot_date = (target - timedelta(days=1)).strftime("%Y-%m-%d")
            slot = "night"
        else:
            slot_date = target.strftime("%Y-%m-%d")
            slot = "day" if is_day else "night"

        detail, block, used_slot, used_date = _resolve_slot(by_date, slot_date, slot)
        if detail is None or block is None:
            raise NmcError("NMC 无可用逐日预报")

        weather = block.get("weather", {})
        wind = block.get("wind", {})
        day_label = "白天" if used_slot == "day" else "夜间"
        try:
            md = datetime.strptime(used_date, "%Y-%m-%d")
            date_txt = md.strftime("%m-%d")
        except (ValueError, TypeError):
            date_txt = used_date
        label = f"预报 {date_txt} {day_label}"
        return NmcReport(
            target_time=target,
            valid_label=label,
            weather_code=str(weather.get("img", "")),
            weather_text=str(weather.get("info", "")),
            temperature=_fmt_temp(weather.get("temperature")),
            precip=_fmt_precip_daily(detail.get("precipitation")),
            wind=_fmt_wind(wind.get("direct"), wind.get("power")),
            humidity=None,
            is_forecast=True,
        )


def _resolve_slot(by_date: dict, date: str, slot: str):
    """从目标时段起按时间顺序找到首个有效（非 9999）的昼夜预报块。

    返回 (detail, block, used_slot, used_date)。
    """
    dates = sorted(by_date.keys())
    if not dates:
        return None, None, slot, date
    start = dates.index(date) if date in dates else 0

    # 构造从目标时段开始的 (date, slot) 扫描序列
    sequence: list[tuple[str, str]] = []
    for di in range(start, len(dates)):
        d = dates[di]
        if di == start and slot == "night":
            slots = ("night",)  # 目标为夜间：当天白天已过
        else:
            slots = ("day", "night")
        for s in slots:
            sequence.append((d, s))

    for d, s in sequence:
        block = by_date[d].get(s)
        if _block_valid(block):
            return by_date[d], block, s, d
    return None, None, slot, date


def _block_valid(block: dict | None) -> bool:
    if not block:
        return False
    w = block.get("weather", {})
    return str(w.get("info")) != MISSING and str(w.get("temperature")) != MISSING


def _nearest_by_time(chart: list, target: datetime) -> dict | None:
    best = None
    best_diff = None
    for item in chart:
        t = _parse_dt(item.get("time"), target)
        if t is None:
            continue
        diff = abs((t - target).total_seconds())
        if best_diff is None or diff < best_diff:
            best_diff = diff
            best = item
    return best


def _parse_dt(text: str | None, ref: datetime) -> datetime | None:
    if not text:
        return None
    try:
        naive = datetime.strptime(text, "%Y-%m-%d %H:%M")
    except ValueError:
        return None
    return naive.replace(tzinfo=ref.tzinfo)


def _is_missing(value) -> bool:
    return value is None or str(value).strip() in ("", MISSING, "9999.0")


def _fmt_temp(value) -> str:
    if _is_missing(value):
        return "--"
    try:
        return f"{float(value):.0f}°C"
    except (ValueError, TypeError):
        return str(value)


def _fmt_rain(value) -> str:
    if _is_missing(value):
        return "无降水"
    try:
        v = float(value)
    except (ValueError, TypeError):
        return str(value)
    if v <= 0:
        return "无降水"
    return f"{v:.1f}mm/h"


def _fmt_precip_daily(value) -> str:
    if _is_missing(value):
        return "--"
    try:
        v = float(value)
    except (ValueError, TypeError):
        return str(value)
    if v <= 0:
        return "无降水"
    return f"{v:.0f}mm/日"


def _fmt_humidity(value) -> str | None:
    if _is_missing(value):
        return None
    try:
        return f"{float(value):.0f}%"
    except (ValueError, TypeError):
        return str(value)


def _fmt_wind(direct, power) -> str:
    parts = []
    if not _is_missing(direct):
        parts.append(str(direct))
    if not _is_missing(power):
        parts.append(str(power))
    return " ".join(parts) if parts else "--"


def _fmt_wind_deg(degree, speed) -> str:
    dir_txt = _wind_dir_from_deg(degree)
    if not _is_missing(speed):
        try:
            return f"{dir_txt} {float(speed):.1f}m/s".strip()
        except (ValueError, TypeError):
            pass
    return dir_txt or "--"


def _wind_dir_from_deg(degree) -> str:
    if _is_missing(degree):
        return ""
    try:
        deg = float(degree)
    except (ValueError, TypeError):
        return ""
    names = ["北", "东北", "东", "东南", "南", "西南", "西", "西北"]
    idx = int((deg + 22.5) % 360 // 45)
    return names[idx] + "风"
