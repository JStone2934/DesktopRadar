"""ADSB 飞机数据客户端（adsb.lol / adsb.fi / airplanes.live 免费 API）。"""

from __future__ import annotations

import math
import threading
import time
from dataclasses import dataclass

import requests

ADSB_SOURCES = [
    "https://api.adsb.lol/v2/lat/{lat}/lon/{lon}/dist/{nm}",
    "https://opendata.adsb.fi/api/v2/lat/{lat}/lon/{lon}/dist/{nm}",
    "https://api.airplanes.live/v2/point/{lat}/{lon}/{nm}",
]
MAX_NM = 250
KM_PER_NM = 1.852


class AdsbError(RuntimeError):
    pass


@dataclass
class Aircraft:
    hex: str
    flight: str
    ac_type: str
    alt_ft: int | None
    gs_kt: float | None
    track_deg: float | None
    lat: float
    lon: float
    dist_km: float
    bearing_deg: float
    sample_ts: float = 0.0

    @property
    def ident(self) -> str:
        return (self.flight or self.hex[:6] or "?").strip()


def km_to_nm(km: float) -> int:
    nm = int(math.ceil(km / KM_PER_NM))
    return max(1, min(MAX_NM, nm))


def haversine_km(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r = 6371.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def bearing_deg(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    x = math.sin(dl) * math.cos(p2)
    y = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return (math.degrees(math.atan2(x, y)) + 360) % 360


def _parse_alt(raw) -> int | None:
    if raw is None:
        return None
    if isinstance(raw, str):
        if raw.lower() == "ground":
            return 0
        try:
            return int(float(raw))
        except ValueError:
            return None
    try:
        return int(raw)
    except (TypeError, ValueError):
        return None


def _parse_float(raw) -> float | None:
    if raw is None:
        return None
    try:
        return float(raw)
    except (TypeError, ValueError):
        return None


def extrapolate_position(ac: Aircraft, now: float) -> tuple[float, float]:
    """按航向/地速把飞机位置外推到当前时刻（球面前推），用于插值平滑。"""
    if ac.gs_kt is None or ac.track_deg is None or ac.sample_ts <= 0:
        return ac.lat, ac.lon
    dt = now - ac.sample_ts
    if dt <= 0:
        return ac.lat, ac.lon
    dist_km = ac.gs_kt * KM_PER_NM * (dt / 3600.0)
    if dist_km <= 0:
        return ac.lat, ac.lon
    r = 6371.0
    ang = dist_km / r
    brng = math.radians(ac.track_deg)
    lat1 = math.radians(ac.lat)
    lon1 = math.radians(ac.lon)
    lat2 = math.asin(
        math.sin(lat1) * math.cos(ang)
        + math.cos(lat1) * math.sin(ang) * math.cos(brng)
    )
    lon2 = lon1 + math.atan2(
        math.sin(brng) * math.sin(ang) * math.cos(lat1),
        math.cos(ang) - math.sin(lat1) * math.sin(lat2),
    )
    return math.degrees(lat2), math.degrees(lon2)


def extrapolated_polar(
    ac: Aircraft, ref_lat: float, ref_lon: float, now: float,
) -> tuple[float, float]:
    """返回外推后相对参考点的 (dist_km, bearing_deg)。"""
    lat, lon = extrapolate_position(ac, now)
    return (
        haversine_km(ref_lat, ref_lon, lat, lon),
        bearing_deg(ref_lat, ref_lon, lat, lon),
    )


def _parse_aircraft(
    item: dict, ref_lat: float, ref_lon: float, range_km: float, now: float = 0.0,
) -> Aircraft | None:
    lat = item.get("lat")
    lon = item.get("lon")
    if lat is None or lon is None:
        return None
    try:
        lat_f, lon_f = float(lat), float(lon)
    except (TypeError, ValueError):
        return None
    dist = haversine_km(ref_lat, ref_lon, lat_f, lon_f)
    if dist > range_km:
        return None
    flight = (item.get("flight") or "").strip()
    ac_type = (item.get("t") or "").strip()
    return Aircraft(
        hex=str(item.get("hex") or ""),
        flight=flight,
        ac_type=ac_type,
        alt_ft=_parse_alt(item.get("alt_baro")),
        gs_kt=_parse_float(item.get("gs")),
        track_deg=_parse_float(item.get("track")),
        lat=lat_f,
        lon=lon_f,
        dist_km=dist,
        bearing_deg=bearing_deg(ref_lat, ref_lon, lat_f, lon_f),
        sample_ts=now,
    )


class AdsbClient:
    """按位置与量程拉取附近飞机，带 TTL 缓存。"""

    def __init__(
        self,
        session: requests.Session | None = None,
        ttl_sec: float = 8.0,
        timeout: float = 10.0,
    ) -> None:
        self._session = session or requests.Session()
        self._ttl = ttl_sec
        self._timeout = timeout
        self._lock = threading.Lock()
        self._cache: dict[tuple, tuple[float, list[Aircraft]]] = {}

    def _cache_key(self, lat: float, lon: float, range_km: float) -> tuple:
        return (round(lat, 2), round(lon, 2), int(range_km))

    def _fetch_raw(self, lat: float, lon: float, nm: int) -> list[dict]:
        errors: list[str] = []
        for template in ADSB_SOURCES:
            url = template.format(lat=lat, lon=lon, nm=nm)
            try:
                resp = self._session.get(url, timeout=self._timeout)
                resp.raise_for_status()
                data = resp.json()
                ac = data.get("ac")
                if isinstance(ac, list):
                    return ac
            except Exception as exc:
                errors.append(f"{url}: {exc}")
        raise AdsbError("; ".join(errors) if errors else "无可用 ADSB 数据源")

    def fetch(self, lat: float, lon: float, range_km: float) -> list[Aircraft]:
        key = self._cache_key(lat, lon, range_km)
        now = time.time()
        with self._lock:
            cached = self._cache.get(key)
            if cached and now - cached[0] < self._ttl:
                return list(cached[1])

        nm = km_to_nm(range_km)
        raw = self._fetch_raw(lat, lon, nm)
        aircraft: list[Aircraft] = []
        for item in raw:
            if not isinstance(item, dict):
                continue
            ac = _parse_aircraft(item, lat, lon, range_km, now)
            if ac is not None:
                aircraft.append(ac)
        aircraft.sort(key=lambda a: a.dist_km)

        with self._lock:
            self._cache[key] = (now, aircraft)
        return list(aircraft)
