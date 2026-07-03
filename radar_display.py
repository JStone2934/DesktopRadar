#!/usr/bin/env python3
"""在 GC9A01 圆屏上显示以当前位置为中心的多图层气象图。"""

from __future__ import annotations

import argparse
import json
import math
import re
import shutil
import sys
import threading
import time
import xml.etree.ElementTree as ET
from abc import ABC, abstractmethod
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from io import BytesIO
from pathlib import Path
from typing import Callable

import requests
from PIL import Image, ImageDraw, ImageFont

# 圆盘图分辨率高达上亿像素，放开 Pillow 的解压炸弹保护
Image.MAX_IMAGE_PIXELS = None

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gc9a01 import GC9A01, HEIGHT, WIDTH
from lcd_notifier import LcdNotifier

TILE_SIZE = 256
GRID = 3
COMPOSITE_SIZE = TILE_SIZE * GRID
HALF = WIDTH // 2

IP_API_URL = "http://ip-api.com/json"
RAINVIEWER_API = "https://api.rainviewer.com/public/weather-maps.json"
FY4B_XML_URL = (
    "http://img.nsmc.org.cn/CLOUDIMAGE/FY4B/AGRI/GCLR/SEC/xml/FY4B-china-72h.xml"
)
FY4B_DISK_XML_URL = (
    "http://img.nsmc.org.cn/PORTAL/NSMC/XML/FY4B/FY4B_AGRI_IMG_DISK_GCLR_NOM.xml"
)
NSMC_REFERER = "http://www.nsmc.org.cn/"
# FY-4B 中国区缩略图近似经纬度范围（等经纬度裁切）
FY4B_CHINA_BOUNDS = (70.0, 4.0, 140.0, 54.0)  # west, south, east, north
# FY-4B 全圆盘 GEOS 地球静止投影参数（星下点经度 105E）
FY4B_SUB_LON = 105.0
FY4B_DISK_KEEP_RAW = 30  # 保留的原始圆盘 JPEG 帧数（约 6h 动画，每帧约 16MB）
AMAP_TILE = (
    "https://webrd0{sub}.is.autonavi.com/appmaptile"
    "?lang=zh_cn&size=1&scale=1&style=8&x={x}&y={y}&z={z}"
)
AMAP_REFERER = "https://www.amap.com/"
BASEMAP_PROVIDER = "amap"
BASEMAP_BLEND = 0.35
USER_AGENT = "gc9a01-radar-display/2.0"
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_BOLD_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = BASE_DIR / "config.json"
CACHE_DIR = BASE_DIR / "cache"
FRAMES_DIR = CACHE_DIR / "frames"
FY4B_DISK_RAW_DIR = CACHE_DIR / "disk_raw"
BASEMAP_OK_FILE = CACHE_DIR / "basemap_ok"
KEEP_FRAME_DIRS = 40
LOCATION_THRESHOLD = 0.05

OUTLINE_SOURCES = {
    "ne_50m_coastline.geojson":
        "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_50m_coastline.geojson",
    "ne_50m_admin_0_boundary_lines_land.geojson":
        "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_50m_admin_0_boundary_lines_land.geojson",
}
OUTLINE_COLOR = (110, 130, 150)
OUTLINE_WIDTH = 1

ZOOM_MIN = 3
ZOOM_MAX = 12
RADAR_MAX_ZOOM = 7
KNOB_DEBOUNCE = 0.15
FRAME_TTL = 60
DEFAULT_LAYERS = ["radar", "satellite_fy4b", "satellite_fy4b_disk", "nowcast"]
DEFAULT_LONG_PRESS_MS = 500
DEFAULT_ANIM_FPS = 5
DEFAULT_ANIM_WINDOW_HOURS = 6

SESSION = requests.Session()
SESSION.headers.update({"User-Agent": USER_AGENT})
_adapter = requests.adapters.HTTPAdapter(pool_connections=16, pool_maxsize=16)
SESSION.mount("https://", _adapter)
SESSION.mount("http://", _adapter)
TILE_TIMEOUT = 6

_TILE_CACHE: OrderedDict[str, Image.Image] = OrderedDict()
_TILE_CACHE_MAX = 2000
_basemap_reachable: bool | None = None
_TILE_POOL = ThreadPoolExecutor(max_workers=9)

_rv_cache: tuple[float, dict] | None = None
_fy4b_cache: tuple[float, list[WeatherFrame]] | None = None
_fy4b_disk_cache: tuple[float, list[WeatherFrame]] | None = None
FY4B_XML_TTL = 300


@dataclass
class WeatherFrame:
    token: str
    timestamp: int
    payload: dict = field(default_factory=dict)


class LayerProvider(ABC):
    layer_id: str
    display_name: str

    @abstractmethod
    def supports_zoom(self) -> bool:
        ...

    @abstractmethod
    def frames(self, window_hours: float = DEFAULT_ANIM_WINDOW_HOURS) -> list[WeatherFrame]:
        ...

    @abstractmethod
    def render(
        self,
        frame: WeatherFrame,
        lat: float,
        lon: float,
        city: str,
        zoom: int,
        use_basemap: bool,
        outline_geometries: list | None,
    ) -> Image.Image:
        ...


def load_config() -> dict:
    defaults = {
        "default_lat": 39.9042,
        "default_lon": 116.4074,
        "default_city": "Beijing",
        "layers": DEFAULT_LAYERS,
        "caiyun_token": "",
        "long_press_ms": DEFAULT_LONG_PRESS_MS,
        "anim_fps": DEFAULT_ANIM_FPS,
        "anim_window_hours": DEFAULT_ANIM_WINDOW_HOURS,
    }
    if CONFIG_PATH.exists():
        with open(CONFIG_PATH, encoding="utf-8") as f:
            cfg = json.load(f)
        defaults.update(cfg)
    return defaults


def frame_token_slug(token: str) -> str:
    return re.sub(r"[^\w\-.]+", "_", token)


def zoom_priority_order(center: int) -> list[int]:
    order = [center]
    for delta in range(1, ZOOM_MAX - ZOOM_MIN + 1):
        up = center + delta
        down = center - delta
        if up <= ZOOM_MAX:
            order.append(up)
        if down >= ZOOM_MIN:
            order.append(down)
    return order


def get_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    path = FONT_BOLD_PATH if bold else FONT_PATH
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


def lat_lon_to_global_pixel(lat: float, lon: float, zoom: int) -> tuple[float, float]:
    scale = TILE_SIZE * (2 ** zoom)
    x = (lon + 180.0) / 360.0 * scale
    lat_rad = math.radians(lat)
    y = (1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * scale
    return x, y


def lat_lon_to_tile(lat: float, lon: float, zoom: int) -> tuple[int, int]:
    x, y = lat_lon_to_global_pixel(lat, lon, zoom)
    return int(x // TILE_SIZE), int(y // TILE_SIZE)


def fetch_location(lat: float | None, lon: float | None) -> tuple[float, float, str]:
    cfg = load_config()
    if lat is not None and lon is not None:
        return lat, lon, cfg.get("default_city", "Custom")
    try:
        resp = SESSION.get(IP_API_URL, timeout=10)
        resp.raise_for_status()
        data = resp.json()
        if data.get("status") == "success":
            city = data.get("city") or data.get("regionName") or "Unknown"
            return float(data["lat"]), float(data["lon"]), city
    except Exception as exc:
        print(f"IP 定位失败: {exc}，使用配置文件默认坐标")
    return (
        float(cfg.get("default_lat", 39.9042)),
        float(cfg.get("default_lon", 116.4074)),
        cfg.get("default_city", "Beijing"),
    )


def fetch_rainviewer_data() -> dict:
    global _rv_cache
    now = time.time()
    if _rv_cache and now - _rv_cache[0] < FRAME_TTL:
        return _rv_cache[1]
    resp = SESSION.get(RAINVIEWER_API, timeout=15)
    resp.raise_for_status()
    data = resp.json()
    _rv_cache = (now, data)
    return data


def rainviewer_frames(kind: str, window_hours: float) -> list[WeatherFrame]:
    data = fetch_rainviewer_data()
    host = data["host"]
    items = data.get("radar", {}).get(kind, [])
    if not items:
        return []
    cutoff = int(time.time() - window_hours * 3600)
    frames = []
    for item in items:
        ts = int(item["time"])
        path = item["path"]
        token = f"{kind}_{path.rstrip('/').split('/')[-1]}"
        frames.append(WeatherFrame(token=token, timestamp=ts, payload={"host": host, "path": path}))
    frames = [f for f in frames if f.timestamp >= cutoff]
    return frames if frames else [
        WeatherFrame(
            token=f"{kind}_{items[-1]['path'].rstrip('/').split('/')[-1]}",
            timestamp=int(items[-1]["time"]),
            payload={"host": host, "path": items[-1]["path"]},
        )
    ]


def geos_norm(lat: float, lon: float, sub_lon: float = FY4B_SUB_LON) -> tuple[float, float] | None:
    """经纬度 -> 地球静止圆盘归一化坐标 (nx, ny)，各 [-1, 1]；不可见返回 None。"""
    lat_r = math.radians(lat)
    lon_r = math.radians(lon)
    slon_r = math.radians(sub_lon)
    r_eq = 6378137.0
    r_pol = 6356752.31414
    h = 42164160.0
    e2 = 1.0 - (r_pol ** 2) / (r_eq ** 2)
    c_lat = math.atan((r_pol ** 2) / (r_eq ** 2) * math.tan(lat_r))
    rl = r_pol / math.sqrt(1.0 - e2 * math.cos(c_lat) ** 2)
    r1 = h - rl * math.cos(c_lat) * math.cos(lon_r - slon_r)
    r2 = -rl * math.cos(c_lat) * math.sin(lon_r - slon_r)
    r3 = rl * math.sin(c_lat)
    if r1 <= 0:
        return None
    max_a = math.asin(r_eq / h)
    nx = math.atan(-r2 / r1) / max_a
    ny = math.atan(r3 / math.sqrt(r1 * r1 + r2 * r2)) / max_a
    if nx * nx + ny * ny > 1.0:
        return None
    return nx, ny


def parse_fy4b_disk_xml(xml_text: str) -> list[WeatherFrame]:
    root = ET.fromstring(xml_text)
    seen: set[str] = set()
    frames: list[WeatherFrame] = []
    for image in root.findall("image"):
        url = image.get("url", "")
        if not url or "thumb" in url.lower():
            continue
        if url.startswith("//"):
            url = "http:" + url
        m = re.search(r"(\d{14})", url)
        if not m:
            continue
        stamp = m.group(1)
        if stamp in seen:
            continue
        seen.add(stamp)
        dt = datetime.strptime(stamp, "%Y%m%d%H%M%S").replace(tzinfo=timezone.utc)
        frames.append(
            WeatherFrame(
                token=f"fy4bdisk_{stamp}",
                timestamp=int(dt.timestamp()),
                payload={"url": url},
            )
        )
    frames.sort(key=lambda f: f.timestamp)
    return frames


def fetch_fy4b_disk_frames(window_hours: float) -> list[WeatherFrame]:
    global _fy4b_disk_cache
    now = time.time()
    if _fy4b_disk_cache and now - _fy4b_disk_cache[0] < FY4B_XML_TTL:
        all_frames = _fy4b_disk_cache[1]
    else:
        resp = SESSION.get(FY4B_DISK_XML_URL, timeout=20, headers={"Referer": NSMC_REFERER})
        resp.raise_for_status()
        all_frames = parse_fy4b_disk_xml(resp.text)
        _fy4b_disk_cache = (now, all_frames)
    cutoff = int(time.time() - window_hours * 3600)
    recent = [f for f in all_frames if f.timestamp >= cutoff]
    return recent if recent else (all_frames[-1:] if all_frames else [])


def parse_fy4b_xml(xml_text: str) -> list[WeatherFrame]:
    root = ET.fromstring(xml_text)
    seen: set[str] = set()
    frames: list[WeatherFrame] = []
    for image in root.findall("image"):
        url = image.get("url", "")
        if not url or "thumb" in url.lower():
            continue
        if url.startswith("//"):
            url = "http:" + url
        thumb = url + "-thumb.JPG"
        m = re.search(r"(\d{14})", url)
        if not m:
            continue
        stamp = m.group(1)
        if stamp in seen:
            continue
        seen.add(stamp)
        dt = datetime.strptime(stamp, "%Y%m%d%H%M%S").replace(tzinfo=timezone.utc)
        frames.append(
            WeatherFrame(
                token=f"fy4b_{stamp}",
                timestamp=int(dt.timestamp()),
                payload={"url": thumb},
            )
        )
    frames.sort(key=lambda f: f.timestamp)
    return frames


def fetch_fy4b_frames(window_hours: float) -> list[WeatherFrame]:
    global _fy4b_cache
    now = time.time()
    if _fy4b_cache and now - _fy4b_cache[0] < FY4B_XML_TTL:
        all_frames = _fy4b_cache[1]
    else:
        resp = SESSION.get(
            FY4B_XML_URL,
            timeout=20,
            headers={"Referer": NSMC_REFERER},
        )
        resp.raise_for_status()
        all_frames = parse_fy4b_xml(resp.text)
        _fy4b_cache = (now, all_frames)
    cutoff = int(time.time() - window_hours * 3600)
    recent = [f for f in all_frames if f.timestamp >= cutoff]
    return recent if recent else (all_frames[-1:] if all_frames else [])


def amap_tile_url(zoom: int, tx: int, ty: int) -> str:
    sub = (tx + ty) % 4 + 1
    return AMAP_TILE.format(sub=sub, x=tx, y=ty, z=zoom)


def prepare_basemap_tile(tile: Image.Image) -> Image.Image:
    tile = tile.convert("RGBA")
    backdrop = Image.new("RGBA", tile.size, (15, 20, 30, 255))
    return Image.blend(backdrop, tile, BASEMAP_BLEND)


def fetch_tile(
    url: str,
    quiet: bool = False,
    cache_key: str | None = None,
    extra_headers: dict | None = None,
) -> Image.Image | None:
    key = cache_key or url
    cached = _TILE_CACHE.get(key)
    if cached is not None:
        _TILE_CACHE.move_to_end(key)
        return cached
    try:
        resp = SESSION.get(url, timeout=TILE_TIMEOUT, headers=extra_headers or {})
        resp.raise_for_status()
        if len(resp.content) < 200:
            return None
        img = Image.open(BytesIO(resp.content)).convert("RGBA")
    except Exception as exc:
        if not quiet:
            print(f"  瓦片下载失败: {exc}")
        return None
    _TILE_CACHE[key] = img
    _TILE_CACHE.move_to_end(key)
    while len(_TILE_CACHE) > _TILE_CACHE_MAX:
        _TILE_CACHE.popitem(last=False)
    return img


def fetch_basemap_tile(zoom: int, tx: int, ty: int) -> Image.Image | None:
    url = amap_tile_url(zoom, tx, ty)
    key = f"amap/{zoom}/{tx}/{ty}"
    cached = _TILE_CACHE.get(key)
    if cached is not None:
        _TILE_CACHE.move_to_end(key)
        return cached
    try:
        resp = SESSION.get(url, timeout=TILE_TIMEOUT, headers={"Referer": AMAP_REFERER})
        resp.raise_for_status()
        if len(resp.content) < 2000:
            return None
        img = Image.open(BytesIO(resp.content)).convert("RGBA")
    except Exception:
        return None
    _TILE_CACHE[key] = img
    _TILE_CACHE.move_to_end(key)
    while len(_TILE_CACHE) > _TILE_CACHE_MAX:
        _TILE_CACHE.popitem(last=False)
    return img


def basemap_available(zoom: int, tx: int, ty: int) -> bool:
    global _basemap_reachable
    if _basemap_reachable is not None:
        return _basemap_reachable
    CACHE_DIR.mkdir(exist_ok=True)
    if BASEMAP_OK_FILE.exists():
        cached = BASEMAP_OK_FILE.read_text(encoding="utf-8").strip()
        if cached == BASEMAP_PROVIDER:
            _basemap_reachable = True
            return True
        if cached == "unavailable":
            _basemap_reachable = False
            return False
    url = amap_tile_url(zoom, tx, ty)
    try:
        resp = SESSION.get(url, timeout=3, headers={"Referer": AMAP_REFERER})
        resp.raise_for_status()
        _basemap_reachable = True
        BASEMAP_OK_FILE.write_text(BASEMAP_PROVIDER, encoding="utf-8")
    except Exception:
        _basemap_reachable = False
        BASEMAP_OK_FILE.write_text("unavailable", encoding="utf-8")
    return _basemap_reachable


def load_outline_geometries() -> list:
    CACHE_DIR.mkdir(exist_ok=True)
    features_coords: list = []
    for fname, url in OUTLINE_SOURCES.items():
        cache_file = CACHE_DIR / fname
        if not cache_file.exists():
            try:
                print(f"  下载轮廓数据: {fname}")
                resp = SESSION.get(url, timeout=30)
                resp.raise_for_status()
                cache_file.write_bytes(resp.content)
            except Exception as exc:
                print(f"  轮廓数据下载失败 {fname}: {exc}")
                continue
        try:
            with open(cache_file, encoding="utf-8") as f:
                data = json.load(f)
        except Exception as exc:
            print(f"  轮廓数据解析失败 {fname}: {exc}")
            continue
        for feature in data.get("features", []):
            geom = feature.get("geometry") or {}
            gtype = geom.get("type")
            coords = geom.get("coordinates")
            if gtype == "LineString":
                features_coords.append(coords)
            elif gtype == "MultiLineString":
                features_coords.extend(coords)
    return features_coords


def draw_outline_mercator(
    composite: Image.Image,
    lat: float,
    lon: float,
    zoom: int,
    geometries: list,
) -> None:
    if not geometries:
        return
    origin_tx, origin_ty = lat_lon_to_tile(lat, lon, zoom)
    origin_tx -= GRID // 2
    origin_ty -= GRID // 2
    origin_px = origin_tx * TILE_SIZE
    origin_py = origin_ty * TILE_SIZE
    scale = TILE_SIZE * (2 ** zoom)
    lon_min = origin_px / scale * 360.0 - 180.0
    lon_max = (origin_px + COMPOSITE_SIZE) / scale * 360.0 - 180.0
    draw = ImageDraw.Draw(composite)
    for line in geometries:
        lons = [pt[0] for pt in line]
        if max(lons) < lon_min or min(lons) > lon_max:
            continue
        pts = []
        for lon_pt, lat_pt in line:
            gx, gy = lat_lon_to_global_pixel(lat_pt, lon_pt, zoom)
            pts.append((gx - origin_px, gy - origin_py))
        if len(pts) >= 2:
            draw.line(pts, fill=OUTLINE_COLOR, width=OUTLINE_WIDTH, joint="curve")


def draw_outline_equirect(
    img: Image.Image,
    geometries: list,
    bounds: tuple[float, float, float, float],
) -> None:
    if not geometries:
        return
    west, south, east, north = bounds
    w, h = img.size
    draw = ImageDraw.Draw(img)
    for line in geometries:
        lons = [pt[0] for pt in line]
        lats = [pt[1] for pt in line]
        if max(lons) < west or min(lons) > east or max(lats) < south or min(lats) > north:
            continue
        pts = []
        for lon_pt, lat_pt in line:
            px = (lon_pt - west) / (east - west) * w
            py = (north - lat_pt) / (north - south) * h
            pts.append((px, py))
        if len(pts) >= 2:
            draw.line(pts, fill=OUTLINE_COLOR, width=OUTLINE_WIDTH, joint="curve")


OverlayFn = Callable[[int, int, int], tuple[str, str] | None]


def build_composite_tiles(
    lat: float,
    lon: float,
    zoom: int,
    use_basemap: bool,
    overlay_fn: OverlayFn,
    overlay_zoom: int | None = None,
) -> Image.Image:
    tile_zoom = overlay_zoom if overlay_zoom is not None else zoom
    center_tx, center_ty = lat_lon_to_tile(lat, lon, zoom)
    origin_tx = center_tx - GRID // 2
    origin_ty = center_ty - GRID // 2
    composite = Image.new("RGBA", (COMPOSITE_SIZE, COMPOSITE_SIZE), (15, 20, 30, 255))

    load_basemap = use_basemap
    if load_basemap and not basemap_available(zoom, center_tx, center_ty):
        print("  底图不可达，跳过高德底图")
        load_basemap = False

    jobs: list[tuple[int, int, str, object]] = []
    for dy in range(GRID):
        for dx in range(GRID):
            tx = origin_tx + dx
            ty = origin_ty + dy
            px = dx * TILE_SIZE
            py = dy * TILE_SIZE
            if load_basemap:
                jobs.append((px, py, "base", _TILE_POOL.submit(fetch_basemap_tile, zoom, tx, ty)))
            otx = int(tx * (2 ** (tile_zoom - zoom))) if tile_zoom != zoom else tx
            oty = int(ty * (2 ** (tile_zoom - zoom))) if tile_zoom != zoom else ty
            spec = overlay_fn(tile_zoom, otx, oty)
            if spec:
                url, key = spec
                jobs.append((px, py, "overlay", _TILE_POOL.submit(fetch_tile, url, True, key)))

    for kind in ("base", "overlay"):
        for px, py, k, future in jobs:
            if k != kind:
                continue
            tile = future.result()
            if not tile:
                continue
            if kind == "base":
                tile = prepare_basemap_tile(tile)
                composite.paste(tile, (px, py), tile)
            else:
                composite.alpha_composite(tile, (px, py))
    return composite


def crop_centered(composite: Image.Image, lat: float, lon: float, zoom: int) -> Image.Image:
    gx, gy = lat_lon_to_global_pixel(lat, lon, zoom)
    origin_tx, origin_ty = lat_lon_to_tile(lat, lon, zoom)
    origin_tx -= GRID // 2
    origin_ty -= GRID // 2
    local_x = gx - origin_tx * TILE_SIZE
    local_y = gy - origin_ty * TILE_SIZE
    left = int(local_x - HALF)
    top = int(local_y - HALF)
    return composite.crop((left, top, left + WIDTH, top + HEIGHT)).convert("RGB")


def apply_circle_mask(img: Image.Image) -> Image.Image:
    mask = Image.new("L", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(mask)
    draw.ellipse((0, 0, WIDTH - 1, HEIGHT - 1), fill=255)
    out = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0))
    out.paste(img, (0, 0), mask)
    return out


def draw_overlay(
    img: Image.Image,
    city: str,
    frame_ts: int,
    zoom: int | None,
    layer_name: str,
) -> Image.Image:
    draw = ImageDraw.Draw(img)
    cx, cy = HALF, HALF
    mark_color = (255, 255, 255)
    arm = 8
    draw.line([(cx - arm, cy), (cx + arm, cy)], fill=mark_color, width=2)
    draw.line([(cx, cy - arm), (cx, cy + arm)], fill=mark_color, width=2)
    draw.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], fill=(255, 60, 60))
    frame_time = datetime.fromtimestamp(frame_ts, tz=timezone.utc).astimezone()
    time_str = frame_time.strftime("%H:%M")
    if zoom is not None:
        label = f"{layer_name} {city} {time_str} z{zoom}"
    else:
        label = f"{layer_name} {city} {time_str}"
    font = get_font(14)
    bbox = draw.textbbox((0, 0), label, font=font)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    bar_h = th + 8
    draw.rectangle([(0, HEIGHT - bar_h), (WIDTH, HEIGHT)], fill=(0, 0, 0))
    draw.text(((WIDTH - tw) // 2, HEIGHT - bar_h + 3), label, fill=(200, 200, 200), font=font)
    return img


def make_error_image(message: str) -> Image.Image:
    img = Image.new("RGB", (WIDTH, HEIGHT), (20, 25, 40))
    draw = ImageDraw.Draw(img)
    font = get_font(16, bold=True)
    y = 90
    for line in message.split("\n"):
        bbox = draw.textbbox((0, 0), line, font=font)
        tw = bbox[2] - bbox[0]
        draw.text(((WIDTH - tw) // 2, y), line, fill=(255, 100, 100), font=font)
        y += 22
    return apply_circle_mask(img)


def render_tile_layer(
    lat: float,
    lon: float,
    city: str,
    zoom: int,
    frame_ts: int,
    layer_name: str,
    overlay_fn: OverlayFn,
    use_basemap: bool,
    outline_geometries: list | None,
    overlay_zoom: int | None = None,
) -> Image.Image:
    composite = build_composite_tiles(
        lat, lon, zoom, use_basemap, overlay_fn, overlay_zoom=overlay_zoom
    )
    if outline_geometries:
        draw_outline_mercator(composite, lat, lon, zoom, outline_geometries)
    cropped = crop_centered(composite, lat, lon, zoom)
    cropped = draw_overlay(cropped, city, frame_ts, zoom, layer_name)
    return apply_circle_mask(cropped)


class RainViewerLayer(LayerProvider):
    def __init__(self, layer_id: str, display_name: str, kind: str) -> None:
        self.layer_id = layer_id
        self.display_name = display_name
        self.kind = kind

    def supports_zoom(self) -> bool:
        return True

    def frames(self, window_hours: float = DEFAULT_ANIM_WINDOW_HOURS) -> list[WeatherFrame]:
        return rainviewer_frames(self.kind, window_hours)

    def render(
        self,
        frame: WeatherFrame,
        lat: float,
        lon: float,
        city: str,
        zoom: int,
        use_basemap: bool,
        outline_geometries: list | None,
    ) -> Image.Image:
        host = frame.payload["host"]
        path = frame.payload["path"]
        eff_zoom = min(zoom, RADAR_MAX_ZOOM)

        def overlay_fn(z: int, tx: int, ty: int) -> tuple[str, str] | None:
            url = f"{host}{path}/256/{z}/{tx}/{ty}/4/1_1.png"
            key = f"{self.layer_id}/{frame.token}/{z}/{tx}/{ty}"
            return url, key

        return render_tile_layer(
            lat, lon, city, eff_zoom, frame.timestamp, self.display_name,
            overlay_fn, use_basemap, outline_geometries,
        )


class FY4BDiskLayer(LayerProvider):
    """FY-4B 全圆盘真彩色（DISK GCLR NOM），GEOS 投影，以当前位置为中心，可缩放。"""

    layer_id = "satellite_fy4b_disk"
    display_name = "风云4B盘"

    def supports_zoom(self) -> bool:
        return True

    def frames(self, window_hours: float = DEFAULT_ANIM_WINDOW_HOURS) -> list[WeatherFrame]:
        return fetch_fy4b_disk_frames(window_hours)

    def _raw_path(self, frame: WeatherFrame) -> Path:
        return FY4B_DISK_RAW_DIR / f"{frame_token_slug(frame.token)}.jpg"

    def _raw_bytes(self, frame: WeatherFrame) -> bytes:
        path = self._raw_path(frame)
        if path.exists():
            try:
                return path.read_bytes()
            except Exception:
                pass
        resp = SESSION.get(frame.payload["url"], timeout=60, headers={"Referer": NSMC_REFERER})
        resp.raise_for_status()
        FY4B_DISK_RAW_DIR.mkdir(parents=True, exist_ok=True)
        try:
            path.write_bytes(resp.content)
            self._prune_raw()
        except Exception:
            pass
        return resp.content

    def _prune_raw(self) -> None:
        if not FY4B_DISK_RAW_DIR.exists():
            return
        files = sorted(
            FY4B_DISK_RAW_DIR.glob("*.jpg"),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        for old in files[FY4B_DISK_KEEP_RAW:]:
            old.unlink(missing_ok=True)

    def render(
        self,
        frame: WeatherFrame,
        lat: float,
        lon: float,
        city: str,
        zoom: int,
        use_basemap: bool,
        outline_geometries: list | None,
    ) -> Image.Image:
        # 缩放：frac 是屏幕半宽占圆盘半径的比例，zoom 越大看得越近
        frac = 1.0 / (1.6 ** (zoom - ZOOM_MIN))
        frac = max(0.012, min(1.0, frac))
        desired_w = int(min(10992, max(480, 240 / frac)))

        raw = self._raw_bytes(frame)
        im = Image.open(BytesIO(raw))
        im.draft("RGB", (desired_w, desired_w))
        im = im.convert("RGB")

        w = im.width
        radius = w / 2.0
        center = w / 2.0  # 圆盘水平充满图宽，圆心 (w/2, w/2)
        pos = geos_norm(lat, lon)
        if pos is None:
            nx, ny = 0.0, 0.0
        else:
            nx, ny = pos
        px = center + nx * radius
        py = center - ny * radius
        half = max(HALF, frac * radius)
        left = int(round(px - half))
        top = int(round(py - half))
        size = int(round(2 * half))
        crop = im.crop((left, top, left + size, top + size))
        if crop.size != (WIDTH, HEIGHT):
            crop = crop.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        crop = draw_overlay(crop, city, frame.timestamp, zoom, self.display_name)
        return apply_circle_mask(crop)


class FY4BLayer(LayerProvider):
    layer_id = "satellite_fy4b"
    display_name = "风云4B"

    def supports_zoom(self) -> bool:
        return False

    def frames(self, window_hours: float = DEFAULT_ANIM_WINDOW_HOURS) -> list[WeatherFrame]:
        return fetch_fy4b_frames(window_hours)

    def render(
        self,
        frame: WeatherFrame,
        lat: float,
        lon: float,
        city: str,
        zoom: int,
        use_basemap: bool,
        outline_geometries: list | None,
    ) -> Image.Image:
        url = frame.payload["url"]
        key = f"fy4b/{frame.token}"
        cached = _TILE_CACHE.get(key)
        if cached is None:
            resp = SESSION.get(url, timeout=20, headers={"Referer": NSMC_REFERER})
            resp.raise_for_status()
            src = Image.open(BytesIO(resp.content)).convert("RGB")
            _TILE_CACHE[key] = src
        else:
            src = cached.copy()

        west, south, east, north = FY4B_CHINA_BOUNDS
        w, h = src.size
        cx = (lon - west) / (east - west) * w
        cy = (north - lat) / (north - south) * h
        half_w = WIDTH / 2
        half_h = HEIGHT / 2
        left = max(0, int(cx - half_w))
        top = max(0, int(cy - half_h))
        right = min(w, left + WIDTH)
        bottom = min(h, top + HEIGHT)
        if right - left < WIDTH:
            left = max(0, right - WIDTH)
        if bottom - top < HEIGHT:
            top = max(0, bottom - HEIGHT)
        cropped = src.crop((left, top, left + WIDTH, top + HEIGHT))
        if cropped.size != (WIDTH, HEIGHT):
            cropped = cropped.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        if outline_geometries:
            draw_outline_equirect(cropped, outline_geometries, FY4B_CHINA_BOUNDS)
        cropped = draw_overlay(cropped, city, frame.timestamp, None, self.display_name)
        return apply_circle_mask(cropped)


class CaiyunRadarLayer(LayerProvider):
    layer_id = "radar_caiyun"
    display_name = "彩云雷达"

    def __init__(self, token: str) -> None:
        self.token = token

    def supports_zoom(self) -> bool:
        return False

    def frames(self, window_hours: float = DEFAULT_ANIM_WINDOW_HOURS) -> list[WeatherFrame]:
        url = (
            f"http://api.caiyunapp.com/v1/radar/images"
            f"?lon=116.4&lat=39.9&level=2&token={self.token}"
        )
        resp = SESSION.get(url, timeout=15)
        resp.raise_for_status()
        data = resp.json()
        if data.get("status") != "ok":
            raise RuntimeError(data.get("msg", "彩云雷达不可用"))
        cutoff = int(time.time() - window_hours * 3600)
        frames = []
        for item in data.get("images", []):
            img_url, ts, bounds = item[0], int(item[1]), item[2]
            if ts < cutoff:
                continue
            frames.append(
                WeatherFrame(
                    token=f"caiyun_{ts}",
                    timestamp=ts,
                    payload={"url": img_url, "bounds": bounds},
                )
            )
        return frames if frames else []

    def render(
        self,
        frame: WeatherFrame,
        lat: float,
        lon: float,
        city: str,
        zoom: int,
        use_basemap: bool,
        outline_geometries: list | None,
    ) -> Image.Image:
        south, west, north, east = frame.payload["bounds"]
        url = frame.payload["url"]
        if url.startswith("/"):
            url = "https://cdn.caiyunapp.com" + url
        key = f"caiyun/{frame.token}"
        cached = _TILE_CACHE.get(key)
        if cached is None:
            resp = SESSION.get(url, timeout=20)
            resp.raise_for_status()
            src = Image.open(BytesIO(resp.content)).convert("RGB")
            _TILE_CACHE[key] = src
        else:
            src = cached.copy()
        w, h = src.size
        cx = (lon - west) / (east - west) * w
        cy = (north - lat) / (north - south) * h
        left = max(0, int(cx - HALF))
        top = max(0, int(cy - HALF))
        cropped = src.crop((left, top, left + WIDTH, top + HEIGHT))
        if cropped.size != (WIDTH, HEIGHT):
            cropped = cropped.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        cropped = draw_overlay(cropped, city, frame.timestamp, None, self.display_name)
        return apply_circle_mask(cropped)


LAYER_LCD_LABELS = {
    "radar": "Radar",
    "nowcast": "Nowcast",
    "satellite_fy4b": "FY-4B CN",
    "satellite_fy4b_disk": "FY-4B Disk",
    "radar_caiyun": "Caiyun Radar",
}


def layer_lcd_label(layer: LayerProvider) -> str:
    return LAYER_LCD_LABELS.get(layer.layer_id, layer.layer_id)


def frame_lcd_time(frame: WeatherFrame) -> str | None:
    """把帧时间戳格式化为本地时间 'MM-DD HH:MM'，无有效时间戳返回 None。"""
    if not frame.timestamp or frame.timestamp <= 0:
        return None
    local = datetime.fromtimestamp(frame.timestamp, tz=timezone.utc).astimezone()
    return local.strftime("%m-%d %H:%M")


def build_layer_registry(cfg: dict) -> dict[str, LayerProvider]:
    registry: dict[str, LayerProvider] = {
        "radar": RainViewerLayer("radar", "雷达", "past"),
        "nowcast": RainViewerLayer("nowcast", "短临", "nowcast"),
        "satellite_fy4b": FY4BLayer(),
        "satellite_fy4b_disk": FY4BDiskLayer(),
    }
    token = (cfg.get("caiyun_token") or "").strip()
    if token:
        registry["radar_caiyun"] = CaiyunRadarLayer(token)
    return registry


def resolve_layers(cfg: dict) -> list[LayerProvider]:
    registry = build_layer_registry(cfg)
    ids = cfg.get("layers", DEFAULT_LAYERS)
    layers: list[LayerProvider] = []
    for lid in ids:
        if lid in registry:
            layers.append(registry[lid])
        else:
            print(f"未知图层 {lid}，已跳过")
    if not layers:
        layers = [registry["radar"]]
    return layers


def make_frame_meta(
    layer_id: str,
    frame: WeatherFrame,
    lat: float,
    lon: float,
    city: str,
) -> dict:
    return {
        "layer_id": layer_id,
        "frame_token": frame.token,
        "frame_ts": frame.timestamp,
        "lat": lat,
        "lon": lon,
        "city": city,
    }


class FrameCache:
    """预渲染成品图：内存 + 磁盘 cache/frames/{layer}/{token}/zNN.png。"""

    def __init__(self) -> None:
        self._mem: dict[tuple[str, str, int], Image.Image] = {}
        self._lock = threading.Lock()
        FRAMES_DIR.mkdir(parents=True, exist_ok=True)

    def _frame_dir(self, layer_id: str, frame_token: str) -> Path:
        return FRAMES_DIR / layer_id / frame_token_slug(frame_token)

    def _zoom_path(self, layer_id: str, frame_token: str, zoom: int) -> Path:
        return self._frame_dir(layer_id, frame_token) / f"z{zoom:02d}.png"

    def get(self, layer_id: str, frame_token: str, zoom: int) -> Image.Image | None:
        key = (layer_id, frame_token, zoom)
        with self._lock:
            cached = self._mem.get(key)
            if cached is not None:
                return cached.copy()
        path = self._zoom_path(layer_id, frame_token, zoom)
        if not path.exists():
            return None
        try:
            img = Image.open(path).convert("RGB")
        except Exception:
            return None
        with self._lock:
            self._mem[key] = img
        return img.copy()

    def put(
        self,
        layer_id: str,
        frame_token: str,
        zoom: int,
        img: Image.Image,
        meta: dict,
    ) -> None:
        frame_dir = self._frame_dir(layer_id, frame_token)
        frame_dir.mkdir(parents=True, exist_ok=True)
        meta_path = frame_dir / "meta.json"
        if not meta_path.exists():
            with open(meta_path, "w", encoding="utf-8") as f:
                json.dump(meta, f, ensure_ascii=False)
        img.save(self._zoom_path(layer_id, frame_token, zoom), optimize=True)
        key = (layer_id, frame_token, zoom)
        with self._lock:
            self._mem[key] = img.copy()

    def clear_memory(self) -> None:
        with self._lock:
            self._mem.clear()

    def invalidate_old(self, layer_id: str, keep_token: str) -> None:
        layer_dir = FRAMES_DIR / layer_id
        if not layer_dir.exists():
            return
        keep_slug = frame_token_slug(keep_token)
        dirs = [d for d in layer_dir.iterdir() if d.is_dir()]
        others = sorted(
            (d for d in dirs if d.name != keep_slug),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        keep_names = {keep_slug}
        for d in others[: max(0, KEEP_FRAME_DIRS - 1)]:
            keep_names.add(d.name)
        for d in dirs:
            if d.name not in keep_names:
                shutil.rmtree(d, ignore_errors=True)
        with self._lock:
            self._mem = {
                k: v for k, v in self._mem.items()
                if k[0] != layer_id or frame_token_slug(k[1]) in keep_names
            }


class PrefetchWorker(threading.Thread):
    def __init__(
        self,
        frame_cache: FrameCache,
        state: AppState,
        use_basemap: bool,
        outline_geometries: list | None,
    ) -> None:
        super().__init__(daemon=True)
        self.frame_cache = frame_cache
        self.state = state
        self.use_basemap = use_basemap
        self.outline_geometries = outline_geometries
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._generation = 0
        self._job: tuple | None = None

    def schedule(
        self,
        layer: LayerProvider,
        frame: WeatherFrame,
        lat: float,
        lon: float,
        city: str,
        center_zoom: int,
    ) -> None:
        with self._lock:
            self._generation += 1
            self._job = (
                self._generation,
                layer,
                frame,
                lat,
                lon,
                city,
                center_zoom,
            )
        self._wake.set()

    def run(self) -> None:
        while True:
            self._wake.wait()
            self._wake.clear()
            with self._lock:
                job = self._job
                self._job = None
            if job is None:
                continue
            gen, layer, frame, lat, lon, city, center_zoom = job
            if not layer.supports_zoom():
                continue
            meta = make_frame_meta(layer.layer_id, frame, lat, lon, city)
            for z in zoom_priority_order(center_zoom):
                with self._lock:
                    if self._generation != gen:
                        break
                if self.frame_cache.get(layer.layer_id, frame.token, z) is not None:
                    continue
                try:
                    img = layer.render(
                        frame, lat, lon, city, z,
                        self.use_basemap, self.outline_geometries,
                    )
                    self.frame_cache.put(layer.layer_id, frame.token, z, img, meta)
                    print(f"  预取 {layer.display_name} z{z}")
                except Exception as exc:
                    print(f"  预取 {layer.display_name} z{z} 失败: {exc}")


class AppState:
    def __init__(
        self,
        layers: list[LayerProvider],
        zoom: int,
        default_zoom: int,
        start_layer: str | None = None,
        long_press_ms: int = DEFAULT_LONG_PRESS_MS,
    ) -> None:
        self.layers = layers
        self.zoom = self._clamp(zoom)
        self.default_zoom = self._clamp(default_zoom)
        self.long_press_ms = long_press_ms
        self._lock = threading.Lock()
        self.wake = threading.Event()
        self.anim_active = False
        idx = 0
        if start_layer:
            for i, layer in enumerate(layers):
                if layer.layer_id == start_layer:
                    idx = i
                    break
        self.layer_index = idx
        self.notifier: LcdNotifier | None = None

    def _lcd_notify(self, line1: str, line2: str = "") -> None:
        if self.notifier is not None:
            self.notifier.notify(line1, line2)

    def notify_lcd(self, line1: str, line2: str = "") -> None:
        self._lcd_notify(line1, line2)

    @staticmethod
    def _clamp(z: int) -> int:
        return max(ZOOM_MIN, min(ZOOM_MAX, z))

    def get_zoom(self) -> int:
        with self._lock:
            return self.zoom

    def get_layer(self) -> LayerProvider:
        with self._lock:
            return self.layers[self.layer_index]

    def get_layer_index(self) -> int:
        with self._lock:
            return self.layer_index

    def is_anim_active(self) -> bool:
        with self._lock:
            return self.anim_active

    def bump_zoom(self, delta: int) -> None:
        with self._lock:
            new = self._clamp(self.zoom + delta)
            if new == self.zoom:
                return
            self.zoom = new
        print(f"旋钮: zoom -> {new}")
        self._lcd_notify("Zoom", f"z{new}")
        self.wake.set()

    def reset_zoom(self) -> None:
        with self._lock:
            if self.zoom != self.default_zoom:
                self.zoom = self.default_zoom
        print(f"旋钮按下: zoom 重置为 {self.default_zoom}")
        self._lcd_notify("Zoom Reset", f"z{self.default_zoom}")
        self.wake.set()

    def next_layer(self) -> None:
        with self._lock:
            self.layer_index = (self.layer_index + 1) % len(self.layers)
            self.anim_active = False
            layer = self.layers[self.layer_index]
            name = layer.display_name
        print(f"图层: -> {name}")
        self._lcd_notify("Layer", layer_lcd_label(layer))
        self.wake.set()

    def start_anim(self) -> None:
        with self._lock:
            if self.anim_active:
                return
            self.anim_active = True
            layer = self.layers[self.layer_index]
        print("动画: 开始播放")
        self._lcd_notify("Animation", f"Play {layer_lcd_label(layer)}")
        self.wake.set()

    def stop_anim(self) -> None:
        with self._lock:
            if not self.anim_active:
                return
            self.anim_active = False
        print("动画: 停止")
        self._lcd_notify("Animation", "Stopped")
        self.wake.set()


def find_knob_device():
    try:
        import evdev
        from evdev import ecodes
    except ImportError:
        return None
    for path in evdev.list_devices():
        try:
            dev = evdev.InputDevice(path)
        except Exception:
            continue
        keys = dev.capabilities().get(ecodes.EV_KEY, [])
        if ecodes.KEY_VOLUMEUP in keys and ecodes.KEY_VOLUMEDOWN in keys:
            return dev
    return None


def find_keyboard_device():
    try:
        import evdev
        from evdev import ecodes
    except ImportError:
        return None
    for path in evdev.list_devices():
        try:
            dev = evdev.InputDevice(path)
        except Exception:
            continue
        keys = dev.capabilities().get(ecodes.EV_KEY, [])
        if ecodes.KEY_SPACE in keys:
            return dev
    return None


class KnobController(threading.Thread):
    def __init__(self, state: AppState) -> None:
        super().__init__(daemon=True)
        self.state = state

    def run(self) -> None:
        try:
            import evdev
            from evdev import ecodes
        except ImportError:
            print("未安装 python3-evdev，旋钮功能禁用")
            return
        while True:
            dev = find_knob_device()
            if dev is None:
                time.sleep(5)
                continue
            print(f"旋钮已连接: {dev.name}")
            try:
                for ev in dev.read_loop():
                    if ev.type != ecodes.EV_KEY or ev.value != 1:
                        continue
                    if ev.code == ecodes.KEY_VOLUMEUP:
                        self.state.bump_zoom(+1)
                    elif ev.code == ecodes.KEY_VOLUMEDOWN:
                        self.state.bump_zoom(-1)
                    elif ev.code == ecodes.KEY_MUTE:
                        self.state.reset_zoom()
            except OSError as exc:
                print(f"旋钮读取中断: {exc}")
                time.sleep(2)


class LayerKeyController(threading.Thread):
    """KEY_SPACE 短按切换图层，长按播放动画。"""

    def __init__(self, state: AppState) -> None:
        super().__init__(daemon=True)
        self.state = state
        self._press_t: float | None = None
        self._long_triggered = False
        self._held = False
        self._timer: threading.Timer | None = None

    def _on_long_press(self) -> None:
        if self._held and not self._long_triggered:
            self._long_triggered = True
            self.state.start_anim()

    def _cancel_timer(self) -> None:
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None

    def run(self) -> None:
        try:
            import evdev
            from evdev import ecodes
        except ImportError:
            print("未安装 python3-evdev，空格键图层切换禁用")
            return
        while True:
            dev = find_keyboard_device()
            if dev is None:
                time.sleep(5)
                continue
            print(f"键盘已连接: {dev.name}")
            try:
                for ev in dev.read_loop():
                    if ev.type != ecodes.EV_KEY or ev.code != ecodes.KEY_SPACE:
                        continue
                    if ev.value == 1:
                        self._press_t = time.time()
                        self._long_triggered = False
                        self._held = True
                        self._cancel_timer()
                        self._timer = threading.Timer(
                            self.state.long_press_ms / 1000.0,
                            self._on_long_press,
                        )
                        self._timer.daemon = True
                        self._timer.start()
                    elif ev.value == 0 and self._press_t is not None:
                        self._held = False
                        self._cancel_timer()
                        held_ms = (time.time() - self._press_t) * 1000
                        if held_ms < self.state.long_press_ms:
                            self.state.next_layer()
                        elif self._long_triggered:
                            self.state.stop_anim()
                        self._press_t = None
                        self._long_triggered = False
            except OSError as exc:
                print(f"键盘读取中断: {exc}")
                time.sleep(2)


def cache_zoom_for_layer(layer: LayerProvider, zoom: int) -> int:
    return 0 if not layer.supports_zoom() else zoom


def render_layer_frame(
    layer: LayerProvider,
    frame: WeatherFrame,
    lat: float,
    lon: float,
    city: str,
    zoom: int,
    use_basemap: bool,
    outline_geometries: list | None,
    frame_cache: FrameCache,
) -> tuple[Image.Image, bool]:
    cache_z = cache_zoom_for_layer(layer, zoom)
    cached = frame_cache.get(layer.layer_id, frame.token, cache_z)
    if cached is not None:
        return cached, True
    img = layer.render(
        frame, lat, lon, city, zoom, use_basemap, outline_geometries,
    )
    meta = make_frame_meta(layer.layer_id, frame, lat, lon, city)
    frame_cache.put(layer.layer_id, frame.token, cache_z, img, meta)
    return img, False


def play_animation(
    layer: LayerProvider,
    frames: list[WeatherFrame],
    lat: float,
    lon: float,
    city: str,
    zoom: int,
    use_basemap: bool,
    outline_geometries: list | None,
    frame_cache: FrameCache,
    state: AppState,
    show: Callable[[Image.Image], None],
    fps: int,
) -> None:
    if len(frames) < 2:
        print("动画: 可用帧不足，跳过")
        return
    interval = 1.0 / max(1, fps)
    print(f"动画: {layer.display_name} {len(frames)} 帧 @ {fps}fps")
    label = layer_lcd_label(layer)
    while state.is_anim_active():
        for frame in frames:
            if not state.is_anim_active():
                break
            if state.wake.is_set():
                break
            try:
                img, _ = render_layer_frame(
                    layer, frame, lat, lon, city, zoom,
                    use_basemap, outline_geometries, frame_cache,
                )
                show(img)
                time_str = frame_lcd_time(frame)
                if time_str is not None:
                    state.notify_lcd(label, time_str)
            except Exception as exc:
                print(f"  动画帧失败: {exc}")
            deadline = time.time() + interval
            while time.time() < deadline:
                if not state.is_anim_active() or state.wake.is_set():
                    break
                time.sleep(0.02)
        if state.wake.is_set():
            break


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="GC9A01 多图层气象图显示")
    parser.add_argument("--lat", type=float, default=None)
    parser.add_argument("--lon", type=float, default=None)
    parser.add_argument("--zoom", type=int, default=7)
    parser.add_argument("--interval", type=int, default=300)
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--no-basemap", action="store_true")
    parser.add_argument("--no-outline", action="store_true")
    parser.add_argument("--no-knob", action="store_true")
    parser.add_argument("--no-keys", action="store_true", help="禁用空格键图层/动画控制")
    parser.add_argument("--no-display", action="store_true")
    parser.add_argument("--layer", default=None, help="起始图层 ID")
    parser.add_argument("--no-anim", action="store_true", help="禁用长按动画")
    parser.add_argument("--no-lcd", action="store_true", help="禁用 1602 LCD 操作提示")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    cfg = load_config()
    use_basemap = not args.no_basemap
    anim_fps = int(cfg.get("anim_fps", DEFAULT_ANIM_FPS))
    anim_hours = float(cfg.get("anim_window_hours", DEFAULT_ANIM_WINDOW_HOURS))
    long_press_ms = int(cfg.get("long_press_ms", DEFAULT_LONG_PRESS_MS))

    outline_geometries: list = []
    if not args.no_outline:
        outline_geometries = load_outline_geometries()
        print(f"轮廓线段: {len(outline_geometries)} 条")

    layers = resolve_layers(cfg)
    layer_names = ", ".join(l.display_name for l in layers)
    print(f"图层: {layer_names}")

    state = AppState(
        layers=layers,
        zoom=args.zoom,
        default_zoom=args.zoom,
        start_layer=args.layer,
        long_press_ms=long_press_ms,
    )
    notifier: LcdNotifier | None = None
    if not args.no_lcd and not args.once:
        lcd_backlight_seconds = float(cfg.get("lcd_backlight_seconds", 5))
        notifier = LcdNotifier(backlight_seconds=lcd_backlight_seconds)
        if notifier.enabled:
            state.notifier = notifier
            notifier.notify("Weather Radar", layer_lcd_label(state.get_layer()))
    frame_cache = FrameCache()
    prefetch: PrefetchWorker | None = None
    if not args.once:
        prefetch = PrefetchWorker(frame_cache, state, use_basemap, outline_geometries)
        prefetch.start()

    if not args.no_knob and not args.once:
        KnobController(state).start()
    if not args.no_keys and not args.once and not args.no_anim:
        LayerKeyController(state).start()

    lcd = None
    if not args.no_display:
        print("初始化 GC9A01 ...")
        lcd = GC9A01()
        print("初始化完成")

    def show(img: Image.Image) -> None:
        if lcd:
            lcd.display(img)

    last_lat: float | None = None
    last_lon: float | None = None
    last_tokens: dict[str, str] = {}

    try:
        while True:
            zoom = state.get_zoom()
            layer = state.get_layer()
            try:
                lat, lon, city = fetch_location(args.lat, args.lon)
                if last_lat is not None and (
                    abs(lat - last_lat) > LOCATION_THRESHOLD
                    or abs(lon - last_lon) > LOCATION_THRESHOLD
                ):
                    print("位置变化，清空内存热缓存")
                    frame_cache.clear_memory()
                last_lat, last_lon = lat, lon

                if state.is_anim_active():
                    anim_frames = layer.frames(anim_hours)
                    play_animation(
                        layer, anim_frames, lat, lon, city, zoom,
                        use_basemap, outline_geometries, frame_cache,
                        state, show, anim_fps,
                    )
                    state.wake.clear()
                    continue

                frames = layer.frames(anim_hours)
                if not frames:
                    print(f"{layer.display_name} 当前无可用帧")
                    show(make_error_image(f"{layer.display_name}\n暂无数据"))
                    if args.once:
                        break
                    if state.wake.wait(timeout=min(30, args.interval)):
                        time.sleep(KNOB_DEBOUNCE)
                        state.wake.clear()
                    continue
                frame = frames[-1]

                if last_tokens.get(layer.layer_id) != frame.token:
                    frame_cache.invalidate_old(layer.layer_id, frame.token)
                    last_tokens[layer.layer_id] = frame.token

                print(
                    f"位置: {city} ({lat:.4f}, {lon:.4f}), "
                    f"图层={layer.display_name}, zoom={zoom}"
                )
                print(f"  帧: {frame.token} ({frame.timestamp})")

                t0 = time.time()
                img, from_cache = render_layer_frame(
                    layer, frame, lat, lon, city, zoom,
                    use_basemap, outline_geometries, frame_cache,
                )
                if state.wake.is_set() and (
                    state.get_zoom() != zoom
                    or state.get_layer().layer_id != layer.layer_id
                    or state.is_anim_active()
                ):
                    continue

                show(img)
                elapsed_ms = (time.time() - t0) * 1000
                if from_cache:
                    print(f"缓存命中 ({elapsed_ms:.0f}ms)")
                else:
                    print(f"已更新 ({elapsed_ms:.0f}ms)")

                if prefetch is not None and layer.supports_zoom():
                    prefetch.schedule(layer, frame, lat, lon, city, state.get_zoom())
            except Exception as exc:
                print(f"刷新失败: {exc}")
                show(make_error_image("离线\n重试中..."))

            if args.once:
                break

            print(f"等待 {args.interval} 秒（旋钮/空格可打断）...")
            if state.wake.wait(timeout=args.interval):
                time.sleep(KNOB_DEBOUNCE)
                state.wake.clear()
    except KeyboardInterrupt:
        print("\n退出")
    finally:
        if notifier is not None:
            notifier.close()
        if lcd:
            lcd.close()


if __name__ == "__main__":
    main()
