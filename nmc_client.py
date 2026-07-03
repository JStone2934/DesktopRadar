"""中央气象台（NMC）文本天气数据客户端。

通过 nmc.cn 的 rest 接口获取实况与逐日预报，供 Nowcast 图层按时间偏移
（-1h ~ +8h）选取最接近的文本天气报告。不下载 NMC 地图图片。
"""

from __future__ import annotations

import json
import re
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path

import requests

NMC_BASE = "http://www.nmc.cn"
PROVINCE_URL = NMC_BASE + "/rest/province"
WEATHER_URL = NMC_BASE + "/rest/weather"
GEOCODE_URL = "https://api.bigdatacloud.net/data/reverse-geocode-client"
CACHE_DIR = Path(__file__).resolve().parent / "cache"
CITY_CATALOG_PATH = CACHE_DIR / "nmc_cities.json"
GEOCODE_CACHE_PATH = CACHE_DIR / "nmc_geocode.json"
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
class NmcStation:
    code: str
    province: str
    city: str


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
    station_city: str = ""
    station_province: str = ""

    @property
    def icon(self) -> str:
        return weather_icon_category(self.weather_code)

    @property
    def display_city(self) -> str:
        if self.station_city:
            return self.station_city
        return "未知"


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
        self._station_cache: dict[str, tuple[float, NmcStation]] = {}
        self._weather_cache: dict[str, tuple[float, dict]] = {}
        self._city_catalog: list[dict] | None = None
        self._geocode_cache: dict[str, tuple[str, str]] | None = None

    def resolve_station(self, lat: float, lon: float) -> str:
        return self.resolve_station_info(lat, lon).code

    def resolve_station_info(self, lat: float, lon: float) -> NmcStation:
        """按经纬度逆地理编码后匹配 NMC 城市站点（非 IP 定位）。"""
        key = f"{lat:.3f},{lon:.3f}"
        now = time.time()
        with self._lock:
            cached = self._station_cache.get(key)
            if cached and now - cached[0] < self._station_ttl:
                return cached[1]
        province, city = self._reverse_geocode(lat, lon)
        station = self._match_city_station(province, city)
        with self._lock:
            self._station_cache[key] = (now, station)
        return station

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
        station = self.resolve_station_info(lat, lon)
        data = self.fetch_weather(station.code)
        if offset_min <= 0:
            report = self._report_from_observation(data, target, offset_min, now)
        else:
            report = self._report_from_forecast(data, target)
        report.station_city = station.city
        report.station_province = station.province
        return report

    def _load_city_catalog(self) -> list[dict]:
        if self._city_catalog is not None:
            return self._city_catalog
        CACHE_DIR.mkdir(parents=True, exist_ok=True)
        if CITY_CATALOG_PATH.exists():
            try:
                with open(CITY_CATALOG_PATH, encoding="utf-8") as f:
                    catalog = json.load(f)
                if isinstance(catalog, list) and catalog:
                    self._city_catalog = catalog
                    return catalog
            except (json.JSONDecodeError, OSError):
                pass
        resp = self._session.get(PROVINCE_URL, headers=NMC_HEADERS, timeout=15)
        resp.raise_for_status()
        provinces = resp.json()
        catalog: list[dict] = []
        for prov in provinces:
            code = prov.get("code")
            if not code:
                continue
            try:
                cr = self._session.get(
                    f"{PROVINCE_URL}/{code}",
                    headers=NMC_HEADERS,
                    timeout=15,
                )
                cr.raise_for_status()
                catalog.extend(cr.json())
            except Exception:
                continue
        if not catalog:
            raise NmcError("无法加载 NMC 城市列表")
        with open(CITY_CATALOG_PATH, "w", encoding="utf-8") as f:
            json.dump(catalog, f, ensure_ascii=False)
        self._city_catalog = catalog
        return catalog

    def _load_geocode_cache(self) -> dict[str, tuple[str, str]]:
        if self._geocode_cache is not None:
            return self._geocode_cache
        cache: dict[str, tuple[str, str]] = {}
        if GEOCODE_CACHE_PATH.exists():
            try:
                with open(GEOCODE_CACHE_PATH, encoding="utf-8") as f:
                    raw = json.load(f)
                if isinstance(raw, dict):
                    cache = {k: tuple(v) for k, v in raw.items()}
            except (json.JSONDecodeError, OSError, TypeError):
                pass
        self._geocode_cache = cache
        return cache

    def _save_geocode_cache(self) -> None:
        if self._geocode_cache is None:
            return
        CACHE_DIR.mkdir(parents=True, exist_ok=True)
        with open(GEOCODE_CACHE_PATH, "w", encoding="utf-8") as f:
            json.dump(self._geocode_cache, f, ensure_ascii=False)

    def _reverse_geocode(self, lat: float, lon: float) -> tuple[str, str]:
        key = f"{lat:.3f},{lon:.3f}"
        cache = self._load_geocode_cache()
        if key in cache:
            return cache[key]
        resp = self._session.get(
            GEOCODE_URL,
            params={
                "latitude": lat,
                "longitude": lon,
                "localityLanguage": "zh",
            },
            timeout=12,
        )
        resp.raise_for_status()
        data = resp.json()
        province = str(data.get("principalSubdivision") or "")
        city = str(
            data.get("city")
            or data.get("locality")
            or data.get("principalSubdivision")
            or "",
        )
        if not province:
            raise NmcError(f"逆地理编码无省份: {data}")
        cache[key] = (province, city)
        self._save_geocode_cache()
        return province, city

    def _match_city_station(self, province: str, city: str) -> NmcStation:
        catalog = self._load_city_catalog()
        prov_key = province[:2]
        candidates = [
            c for c in catalog
            if prov_key in str(c.get("province", ""))
        ]
        if not candidates:
            raise NmcError(f"NMC 无匹配省份: {province}")

        geo_norm = _norm_place_name(city)
        best: dict | None = None
        best_score = -1
        for item in candidates:
            nmc_city = str(item.get("city", ""))
            nmc_norm = _norm_place_name(nmc_city)
            score = 0
            if geo_norm and nmc_norm == geo_norm:
                score = 100
            elif geo_norm and geo_norm in nmc_city:
                score = 80
            elif geo_norm and nmc_norm and nmc_norm in geo_norm:
                score = 70
            elif geo_norm and nmc_norm and geo_norm in nmc_norm:
                score = 60
            if score > best_score:
                best_score = score
                best = item

        if best is None or best_score <= 0:
            best = candidates[0]
        return NmcStation(
            code=str(best["code"]),
            province=str(best.get("province", province)),
            city=str(best.get("city", city)),
        )

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


def _norm_place_name(name: str) -> str:
    """地名归一化：去括号、行政后缀，便于与 NMC 站名匹配。"""
    text = re.sub(r"[（(].*?[）)]", "", name or "")
    for suffix in ("特别行政区", "自治区", "自治州", "地区", "盟", "市", "区", "县"):
        if text.endswith(suffix) and len(text) > len(suffix):
            text = text[: -len(suffix)]
    return text.strip()


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
