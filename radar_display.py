#!/usr/bin/env python3
"""在 GC9A01 圆屏上显示以当前位置为中心的气象雷达图（RainViewer）。"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import sys
import threading
import time
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from io import BytesIO
from pathlib import Path

import requests
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gc9a01 import GC9A01, HEIGHT, WIDTH

TILE_SIZE = 256
GRID = 3
COMPOSITE_SIZE = TILE_SIZE * GRID
HALF = WIDTH // 2

IP_API_URL = "http://ip-api.com/json"
RAINVIEWER_API = "https://api.rainviewer.com/public/weather-maps.json"
# 高德矢量底图（style=8 路网+地名，国内可达，最高约 z18）
AMAP_TILE = (
    "https://webrd0{sub}.is.autonavi.com/appmaptile"
    "?lang=zh_cn&size=1&scale=1&style=8&x={x}&y={y}&z={z}"
)
AMAP_REFERER = "https://www.amap.com/"
BASEMAP_PROVIDER = "amap"
BASEMAP_BLEND = 0.35  # 压暗亮色底图，突出雷达回波
USER_AGENT = "gc9a01-radar-display/1.0"
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_BOLD_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = BASE_DIR / "config.json"
CACHE_DIR = BASE_DIR / "cache"
FRAMES_DIR = CACHE_DIR / "frames"
BASEMAP_OK_FILE = CACHE_DIR / "basemap_ok"
KEEP_FRAME_DIRS = 2
LOCATION_THRESHOLD = 0.05  # 坐标变化超过此值则作废内存热缓存

# Natural Earth 矢量轮廓（经纬度 GeoJSON），本地缓存后离线可用
OUTLINE_SOURCES = {
    "ne_50m_coastline.geojson":
        "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_50m_coastline.geojson",
    "ne_50m_admin_0_boundary_lines_land.geojson":
        "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_50m_admin_0_boundary_lines_land.geojson",
}
OUTLINE_COLOR = (110, 130, 150)  # 冷灰色勾线
OUTLINE_WIDTH = 1

ZOOM_MIN = 3
ZOOM_MAX = 12
RADAR_MAX_ZOOM = 7  # RainViewer 雷达瓦片最高 zoom
KNOB_DEBOUNCE = 0.15  # 连续转动合并窗口（秒）

SESSION = requests.Session()
SESSION.headers.update({"User-Agent": USER_AGENT})
# 连接池，供并行瓦片下载复用
_adapter = requests.adapters.HTTPAdapter(pool_connections=16, pool_maxsize=16)
SESSION.mount("https://", _adapter)
SESSION.mount("http://", _adapter)
TILE_TIMEOUT = 6  # 单瓦片超时（秒）

# 瓦片内存缓存（LRU）；重复缩放/刷新时秒开
_TILE_CACHE: OrderedDict[str, Image.Image] = OrderedDict()
_TILE_CACHE_MAX = 2000
# 底图可达性只探测一次（None=未知, True/False=结果）
_basemap_reachable: bool | None = None
# 并行下载线程池
_TILE_POOL = ThreadPoolExecutor(max_workers=9)


def radar_path_slug(radar_path: str) -> str:
    return radar_path.rstrip("/").split("/")[-1]


def zoom_priority_order(center: int) -> list[int]:
    """从当前 zoom 向外扩展的预取优先级。"""
    order = [center]
    for delta in range(1, ZOOM_MAX - ZOOM_MIN + 1):
        up = center + delta
        down = center - delta
        if up <= ZOOM_MAX:
            order.append(up)
        if down >= ZOOM_MIN:
            order.append(down)
    return order


def load_config() -> dict:
    if CONFIG_PATH.exists():
        with open(CONFIG_PATH, encoding="utf-8") as f:
            return json.load(f)
    return {"default_lat": 39.9042, "default_lon": 116.4074, "default_city": "Beijing"}


def get_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    path = FONT_BOLD_PATH if bold else FONT_PATH
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


def lat_lon_to_global_pixel(lat: float, lon: float, zoom: int) -> tuple[float, float]:
    """将经纬度转换为 zoom 级别下的全局像素坐标。"""
    scale = TILE_SIZE * (2 ** zoom)
    x = (lon + 180.0) / 360.0 * scale
    lat_rad = math.radians(lat)
    y = (1.0 - math.log(math.tan(lat_rad) + 1.0 / math.cos(lat_rad)) / math.pi) / 2.0 * scale
    return x, y


def lat_lon_to_tile(lat: float, lon: float, zoom: int) -> tuple[int, int]:
    x, y = lat_lon_to_global_pixel(lat, lon, zoom)
    return int(x // TILE_SIZE), int(y // TILE_SIZE)


def fetch_location(lat: float | None, lon: float | None) -> tuple[float, float, str]:
    """获取位置：命令行参数 > IP 定位 > 配置文件默认值。"""
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


_frame_cache: tuple[float, tuple[str, str, int]] | None = None
FRAME_TTL = 120  # 雷达帧元数据缓存秒数（雷达约 10 分钟更新一次）


def fetch_rainviewer_frame() -> tuple[str, str, int]:
    """返回 (host, path, timestamp)，短期缓存以避免缩放时反复请求 API。"""
    global _frame_cache
    now = time.time()
    if _frame_cache and now - _frame_cache[0] < FRAME_TTL:
        return _frame_cache[1]

    resp = SESSION.get(RAINVIEWER_API, timeout=15)
    resp.raise_for_status()
    data = resp.json()
    host = data["host"]
    past = data.get("radar", {}).get("past", [])
    if not past:
        raise RuntimeError("RainViewer 无可用雷达帧")
    latest = past[-1]
    result = (host, latest["path"], int(latest["time"]))
    _frame_cache = (now, result)
    return result


def amap_tile_url(zoom: int, tx: int, ty: int) -> str:
    sub = (tx + ty) % 4 + 1
    return AMAP_TILE.format(sub=sub, x=tx, y=ty, z=zoom)


def prepare_basemap_tile(tile: Image.Image) -> Image.Image:
    """将亮色高德瓦片压暗后叠加，避免盖住雷达回波。"""
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
        resp = SESSION.get(
            url,
            timeout=TILE_TIMEOUT,
            headers={"Referer": AMAP_REFERER},
        )
        resp.raise_for_status()
        # 高德对无数据区域返回带 “Zoom Level Not Supported” 的占位小图
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
    """探测高德底图是否可达；内存 + 磁盘缓存，只探测一次。"""
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
        resp = SESSION.get(
            url,
            timeout=3,
            headers={"Referer": AMAP_REFERER},
        )
        resp.raise_for_status()
        _basemap_reachable = True
        BASEMAP_OK_FILE.write_text(BASEMAP_PROVIDER, encoding="utf-8")
    except Exception:
        _basemap_reachable = False
        BASEMAP_OK_FILE.write_text("unavailable", encoding="utf-8")
    return _basemap_reachable


def load_outline_geometries() -> list:
    """加载海岸线/国界 GeoJSON，返回 LineString 坐标序列列表；本地缓存优先。"""
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


def draw_outline(
    composite: Image.Image,
    lat: float,
    lon: float,
    zoom: int,
    geometries: list,
) -> None:
    """把海岸线/国界勾线画到 composite（像素坐标系）上。"""
    if not geometries:
        return

    origin_tx, origin_ty = lat_lon_to_tile(lat, lon, zoom)
    origin_tx -= GRID // 2
    origin_ty -= GRID // 2
    origin_px = origin_tx * TILE_SIZE
    origin_py = origin_ty * TILE_SIZE

    # composite 覆盖的经度范围，用于快速裁剪
    scale = TILE_SIZE * (2 ** zoom)
    lon_min = origin_px / scale * 360.0 - 180.0
    lon_max = (origin_px + COMPOSITE_SIZE) / scale * 360.0 - 180.0

    draw = ImageDraw.Draw(composite)
    for line in geometries:
        # 粗过滤：整段经度都在窗口外则跳过
        lons = [pt[0] for pt in line]
        if max(lons) < lon_min or min(lons) > lon_max:
            continue

        pts = []
        for lon_pt, lat_pt in line:
            gx, gy = lat_lon_to_global_pixel(lat_pt, lon_pt, zoom)
            px = gx - origin_px
            py = gy - origin_py
            pts.append((px, py))
        if len(pts) >= 2:
            draw.line(pts, fill=OUTLINE_COLOR, width=OUTLINE_WIDTH, joint="curve")


def build_composite(
    lat: float,
    lon: float,
    zoom: int,
    host: str,
    radar_path: str,
    use_basemap: bool,
) -> Image.Image:
    """拼接 3x3 瓦片并叠加雷达。"""
    center_tx, center_ty = lat_lon_to_tile(lat, lon, zoom)
    origin_tx = center_tx - GRID // 2
    origin_ty = center_ty - GRID // 2

    composite = Image.new("RGBA", (COMPOSITE_SIZE, COMPOSITE_SIZE), (15, 20, 30, 255))

    load_basemap = use_basemap
    if load_basemap and not basemap_available(zoom, center_tx, center_ty):
        print("  底图不可达，跳过高德底图（仅显示雷达层）")
        load_basemap = False

    # 收集所有瓦片请求，并行下载
    jobs = []  # (px, py, kind, future)
    for dy in range(GRID):
        for dx in range(GRID):
            tx = origin_tx + dx
            ty = origin_ty + dy
            px = dx * TILE_SIZE
            py = dy * TILE_SIZE

            if load_basemap:
                jobs.append((px, py, "base",
                             _TILE_POOL.submit(fetch_basemap_tile, zoom, tx, ty)))

            radar_url = f"{host}{radar_path}/256/{zoom}/{tx}/{ty}/4/1_1.png"
            radar_key = f"radar/{radar_path}/{zoom}/{tx}/{ty}"
            jobs.append((px, py, "radar",
                         _TILE_POOL.submit(fetch_tile, radar_url, True, radar_key)))

    # 先贴底图再叠雷达，保证图层顺序
    for kind in ("base", "radar"):
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
    """以当前位置为中心裁出 240x240。"""
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


def draw_overlay(img: Image.Image, city: str, frame_ts: int, zoom: int) -> Image.Image:
    draw = ImageDraw.Draw(img)
    cx, cy = HALF, HALF

    # 位置标记：十字 + 圆点
    mark_color = (255, 255, 255)
    arm = 8
    draw.line([(cx - arm, cy), (cx + arm, cy)], fill=mark_color, width=2)
    draw.line([(cx, cy - arm), (cx, cy + arm)], fill=mark_color, width=2)
    draw.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], fill=(255, 60, 60))

    # 底部信息条
    frame_time = datetime.fromtimestamp(frame_ts, tz=timezone.utc).astimezone()
    time_str = frame_time.strftime("%H:%M")
    label = f"{city}  {time_str}  z{zoom}"
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
    lines = message.split("\n")
    y = 90
    for line in lines:
        bbox = draw.textbbox((0, 0), line, font=font)
        tw = bbox[2] - bbox[0]
        draw.text(((WIDTH - tw) // 2, y), line, fill=(255, 100, 100), font=font)
        y += 22
    return apply_circle_mask(img)


def render_radar_frame_internal(
    lat: float,
    lon: float,
    city: str,
    zoom: int,
    host: str,
    radar_path: str,
    frame_ts: int,
    use_basemap: bool,
    outline_geometries: list | None,
) -> Image.Image:
    """纯渲染：瓦片拼接 + 轮廓 + 裁切 + 叠加 + 圆形蒙版。"""
    composite = build_composite(lat, lon, zoom, host, radar_path, use_basemap)
    if outline_geometries:
        draw_outline(composite, lat, lon, zoom, outline_geometries)
    cropped = crop_centered(composite, lat, lon, zoom)
    cropped = draw_overlay(cropped, city, frame_ts, zoom)
    return apply_circle_mask(cropped)


def make_frame_meta(
    lat: float,
    lon: float,
    city: str,
    host: str,
    radar_path: str,
    frame_ts: int,
) -> dict:
    return {
        "lat": lat,
        "lon": lon,
        "city": city,
        "host": host,
        "radar_path": radar_path,
        "frame_ts": frame_ts,
    }


class FrameCache:
    """预渲染成品图：内存热缓存 + 磁盘 cache/frames/{slug}/zNN.png。"""

    def __init__(self) -> None:
        self._mem: dict[tuple[str, int], Image.Image] = {}
        self._lock = threading.Lock()
        FRAMES_DIR.mkdir(parents=True, exist_ok=True)

    def _frame_dir(self, radar_path: str) -> Path:
        return FRAMES_DIR / radar_path_slug(radar_path)

    def _zoom_path(self, radar_path: str, zoom: int) -> Path:
        return self._frame_dir(radar_path) / f"z{zoom:02d}.png"

    def get(self, radar_path: str, zoom: int) -> Image.Image | None:
        key = (radar_path, zoom)
        with self._lock:
            cached = self._mem.get(key)
            if cached is not None:
                return cached.copy()
        path = self._zoom_path(radar_path, zoom)
        if not path.exists():
            return None
        try:
            img = Image.open(path).convert("RGB")
        except Exception:
            return None
        with self._lock:
            self._mem[key] = img
        return img.copy()

    def put(self, radar_path: str, zoom: int, img: Image.Image, meta: dict) -> None:
        frame_dir = self._frame_dir(radar_path)
        frame_dir.mkdir(parents=True, exist_ok=True)
        meta_path = frame_dir / "meta.json"
        if not meta_path.exists():
            with open(meta_path, "w", encoding="utf-8") as f:
                json.dump(meta, f, ensure_ascii=False)
        self._zoom_path(radar_path, zoom).parent.mkdir(parents=True, exist_ok=True)
        img.save(self._zoom_path(radar_path, zoom), optimize=True)
        key = (radar_path, zoom)
        with self._lock:
            self._mem[key] = img.copy()

    def clear_memory(self) -> None:
        with self._lock:
            self._mem.clear()

    def invalidate_old(self, keep_path: str) -> None:
        """只保留当前雷达帧 + 最多一个上一帧目录。"""
        if not FRAMES_DIR.exists():
            return
        keep_slug = radar_path_slug(keep_path)
        dirs = [d for d in FRAMES_DIR.iterdir() if d.is_dir()]
        others = sorted(
            (d for d in dirs if d.name != keep_slug),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        keep_names = {keep_slug}
        if others:
            keep_names.add(others[0].name)
        for d in dirs:
            if d.name not in keep_names:
                shutil.rmtree(d, ignore_errors=True)
        with self._lock:
            self._mem.clear()


class PrefetchWorker(threading.Thread):
    """后台按优先级预渲染全部 zoom 并落盘。"""

    def __init__(
        self,
        frame_cache: FrameCache,
        use_basemap: bool,
        outline_geometries: list | None,
    ) -> None:
        super().__init__(daemon=True)
        self.frame_cache = frame_cache
        self.use_basemap = use_basemap
        self.outline_geometries = outline_geometries
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._generation = 0
        self._job: tuple | None = None

    def schedule(
        self,
        lat: float,
        lon: float,
        city: str,
        host: str,
        radar_path: str,
        frame_ts: int,
        center_zoom: int,
    ) -> None:
        with self._lock:
            self._generation += 1
            self._job = (
                self._generation,
                lat,
                lon,
                city,
                host,
                radar_path,
                frame_ts,
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
            gen, lat, lon, city, host, radar_path, frame_ts, center_zoom = job
            meta = make_frame_meta(lat, lon, city, host, radar_path, frame_ts)
            for z in zoom_priority_order(center_zoom):
                with self._lock:
                    if self._generation != gen:
                        break
                if self.frame_cache.get(radar_path, z) is not None:
                    continue
                try:
                    img = render_radar_frame_internal(
                        lat,
                        lon,
                        city,
                        z,
                        host,
                        radar_path,
                        frame_ts,
                        self.use_basemap,
                        self.outline_geometries,
                    )
                    self.frame_cache.put(radar_path, z, img, meta)
                    print(f"  预取: z{z}")
                except Exception as exc:
                    print(f"  预取 z{z} 失败: {exc}")


class AppState:
    """线程共享状态：缩放级别 + 唤醒事件（供旋钮打断刷新等待）。"""

    def __init__(self, zoom: int, default_zoom: int):
        self.zoom = self._clamp(zoom)
        self.default_zoom = self._clamp(default_zoom)
        self._lock = threading.Lock()
        self.wake = threading.Event()

    @staticmethod
    def _clamp(z: int) -> int:
        return max(ZOOM_MIN, min(ZOOM_MAX, z))

    def get_zoom(self) -> int:
        with self._lock:
            return self.zoom

    def bump_zoom(self, delta: int) -> None:
        with self._lock:
            new = self._clamp(self.zoom + delta)
            if new == self.zoom:
                return
            self.zoom = new
        print(f"旋钮: zoom -> {new}")
        self.wake.set()

    def reset_zoom(self) -> None:
        with self._lock:
            if self.zoom == self.default_zoom:
                # 已是默认值，则触发一次立即刷新
                self.wake.set()
                return
            self.zoom = self.default_zoom
        print(f"旋钮按下: zoom 重置为 {self.default_zoom}")
        self.wake.set()


def find_knob_device():
    """查找带音量键的输入设备（旋钮通过 Consumer Control 发送音量键）。"""
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


class KnobController(threading.Thread):
    """后台监听旋钮：向上转放大、向下转缩小、按下重置缩放。"""

    def __init__(self, state: AppState):
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
                        continue  # 只处理按下（每档一次）
                    if ev.code == ecodes.KEY_VOLUMEUP:
                        self.state.bump_zoom(+1)
                    elif ev.code == ecodes.KEY_VOLUMEDOWN:
                        self.state.bump_zoom(-1)
                    elif ev.code == ecodes.KEY_MUTE:
                        self.state.reset_zoom()
            except OSError as exc:
                print(f"旋钮读取中断（可能已拔出）: {exc}")
                time.sleep(2)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="GC9A01 气象雷达图显示")
    parser.add_argument("--lat", type=float, default=None, help="手动指定纬度")
    parser.add_argument("--lon", type=float, default=None, help="手动指定经度")
    parser.add_argument("--zoom", type=int, default=7, help="地图缩放级别（默认 7）")
    parser.add_argument("--interval", type=int, default=300, help="刷新间隔秒数（默认 300）")
    parser.add_argument("--once", action="store_true", help="只刷新一次")
    parser.add_argument("--no-basemap", action="store_true", help="不加载底图瓦片")
    parser.add_argument("--no-outline", action="store_true", help="不绘制海岸线/国界轮廓勾线")
    parser.add_argument("--no-knob", action="store_true", help="禁用旋钮缩放控制")
    parser.add_argument("--no-display", action="store_true", help="仅下载渲染，不刷屏幕（调试用）")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    use_basemap = not args.no_basemap

    outline_geometries: list = []
    if not args.no_outline:
        outline_geometries = load_outline_geometries()
        print(f"轮廓线段: {len(outline_geometries)} 条")

    state = AppState(zoom=args.zoom, default_zoom=args.zoom)
    frame_cache = FrameCache()
    prefetch: PrefetchWorker | None = None
    if not args.once:
        prefetch = PrefetchWorker(frame_cache, use_basemap, outline_geometries)
        prefetch.start()

    if not args.no_knob and not args.once:
        KnobController(state).start()

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
    last_radar_path: str | None = None

    try:
        while True:
            zoom = state.get_zoom()
            try:
                lat, lon, city = fetch_location(args.lat, args.lon)
                if last_lat is not None and (
                    abs(lat - last_lat) > LOCATION_THRESHOLD
                    or abs(lon - last_lon) > LOCATION_THRESHOLD
                ):
                    print("位置变化，清空内存热缓存")
                    frame_cache.clear_memory()
                last_lat, last_lon = lat, lon

                host, radar_path, frame_ts = fetch_rainviewer_frame()
                if radar_path != last_radar_path:
                    frame_cache.invalidate_old(radar_path)
                    last_radar_path = radar_path

                print(f"位置: {city} ({lat:.4f}, {lon:.4f}), zoom={zoom}")
                print(f"  雷达帧: {radar_path} ({frame_ts})")

                t0 = time.time()
                img = frame_cache.get(radar_path, zoom)
                from_cache = img is not None
                if img is None:
                    meta = make_frame_meta(lat, lon, city, host, radar_path, frame_ts)
                    img = render_radar_frame_internal(
                        lat,
                        lon,
                        city,
                        zoom,
                        host,
                        radar_path,
                        frame_ts,
                        use_basemap,
                        outline_geometries,
                    )
                    if state.wake.is_set() and state.get_zoom() != zoom:
                        continue
                    frame_cache.put(radar_path, zoom, img, meta)
                elif state.wake.is_set() and state.get_zoom() != zoom:
                    continue

                show(img)
                elapsed_ms = (time.time() - t0) * 1000
                if from_cache:
                    print(f"缓存命中 z{zoom} ({elapsed_ms:.0f}ms)")
                else:
                    print(f"雷达图已更新 ({elapsed_ms:.0f}ms)")

                if prefetch is not None:
                    prefetch.schedule(
                        lat,
                        lon,
                        city,
                        host,
                        radar_path,
                        frame_ts,
                        state.get_zoom(),
                    )
            except Exception as exc:
                print(f"刷新失败: {exc}")
                show(make_error_image("离线\n重试中..."))

            if args.once:
                break

            print(f"等待 {args.interval} 秒后刷新（旋钮可打断）...")
            if state.wake.wait(timeout=args.interval):
                time.sleep(KNOB_DEBOUNCE)
                state.wake.clear()
    except KeyboardInterrupt:
        print("\n退出")
    finally:
        if lcd:
            lcd.close()


if __name__ == "__main__":
    main()
