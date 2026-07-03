#!/usr/bin/env python3
"""在 GC9A01 圆屏上显示以当前位置为中心的气象雷达图（RainViewer）。"""

from __future__ import annotations

import argparse
import json
import math
import sys
import time
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
CARTO_BASE = "https://a.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}.png"
USER_AGENT = "gc9a01-radar-display/1.0"
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONT_BOLD_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = BASE_DIR / "config.json"
CACHE_DIR = BASE_DIR / "cache"

# Natural Earth 矢量轮廓（经纬度 GeoJSON），本地缓存后离线可用
OUTLINE_SOURCES = {
    "ne_50m_coastline.geojson":
        "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_50m_coastline.geojson",
    "ne_50m_admin_0_boundary_lines_land.geojson":
        "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_50m_admin_0_boundary_lines_land.geojson",
}
OUTLINE_COLOR = (110, 130, 150)  # 冷灰色勾线
OUTLINE_WIDTH = 1

SESSION = requests.Session()
SESSION.headers.update({"User-Agent": USER_AGENT})
TILE_TIMEOUT = 8  # 单瓦片超时（秒）


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


def fetch_rainviewer_frame() -> tuple[str, str, int]:
    """返回 (host, path, timestamp)。"""
    resp = SESSION.get(RAINVIEWER_API, timeout=15)
    resp.raise_for_status()
    data = resp.json()
    host = data["host"]
    past = data.get("radar", {}).get("past", [])
    if not past:
        raise RuntimeError("RainViewer 无可用雷达帧")
    latest = past[-1]
    return host, latest["path"], int(latest["time"])


def fetch_tile(url: str, quiet: bool = False) -> Image.Image | None:
    try:
        resp = SESSION.get(url, timeout=TILE_TIMEOUT)
        resp.raise_for_status()
        return Image.open(BytesIO(resp.content)).convert("RGBA")
    except Exception as exc:
        if not quiet:
            print(f"  瓦片下载失败: {exc}")
        return None


def basemap_available(zoom: int, tx: int, ty: int) -> bool:
    """探测底图是否可达；国内网络常无法访问 CartoDB。"""
    url = CARTO_BASE.format(z=zoom, x=tx, y=ty)
    try:
        resp = SESSION.get(url, timeout=3)
        resp.raise_for_status()
        return True
    except Exception:
        return False


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
        print("  底图不可达，跳过 CartoDB（仅显示雷达层）")
        load_basemap = False

    for dy in range(GRID):
        for dx in range(GRID):
            tx = origin_tx + dx
            ty = origin_ty + dy
            px = dx * TILE_SIZE
            py = dy * TILE_SIZE

            if load_basemap:
                base_url = CARTO_BASE.format(z=zoom, x=tx, y=ty)
                base_tile = fetch_tile(base_url, quiet=True)
                if base_tile:
                    composite.paste(base_tile, (px, py), base_tile)

            radar_url = f"{host}{radar_path}/256/{zoom}/{tx}/{ty}/4/1_1.png"
            radar_tile = fetch_tile(radar_url, quiet=True)
            if radar_tile:
                composite.alpha_composite(radar_tile, (px, py))

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


def draw_overlay(img: Image.Image, city: str, frame_ts: int) -> Image.Image:
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
    label = f"{city}  {time_str}"
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


def render_radar_frame(
    lat: float,
    lon: float,
    city: str,
    zoom: int,
    use_basemap: bool,
    outline_geometries: list | None,
) -> Image.Image:
    host, radar_path, frame_ts = fetch_rainviewer_frame()
    print(f"  雷达帧: {radar_path} ({frame_ts})")

    composite = build_composite(lat, lon, zoom, host, radar_path, use_basemap)
    if outline_geometries:
        draw_outline(composite, lat, lon, zoom, outline_geometries)
    cropped = crop_centered(composite, lat, lon, zoom)
    cropped = draw_overlay(cropped, city, frame_ts)
    return apply_circle_mask(cropped)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="GC9A01 气象雷达图显示")
    parser.add_argument("--lat", type=float, default=None, help="手动指定纬度")
    parser.add_argument("--lon", type=float, default=None, help="手动指定经度")
    parser.add_argument("--zoom", type=int, default=7, help="地图缩放级别（默认 7）")
    parser.add_argument("--interval", type=int, default=300, help="刷新间隔秒数（默认 300）")
    parser.add_argument("--once", action="store_true", help="只刷新一次")
    parser.add_argument("--no-basemap", action="store_true", help="不加载底图瓦片")
    parser.add_argument("--no-outline", action="store_true", help="不绘制海岸线/国界轮廓勾线")
    parser.add_argument("--no-display", action="store_true", help="仅下载渲染，不刷屏幕（调试用）")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    use_basemap = not args.no_basemap

    outline_geometries: list = []
    if not args.no_outline:
        outline_geometries = load_outline_geometries()
        print(f"轮廓线段: {len(outline_geometries)} 条")

    lcd = None
    if not args.no_display:
        print("初始化 GC9A01 ...")
        lcd = GC9A01()
        print("初始化完成")

    def show(img: Image.Image) -> None:
        if lcd:
            lcd.display(img)

    try:
        while True:
            try:
                lat, lon, city = fetch_location(args.lat, args.lon)
                print(f"位置: {city} ({lat:.4f}, {lon:.4f}), zoom={args.zoom}")

                img = render_radar_frame(
                    lat, lon, city, args.zoom, use_basemap, outline_geometries
                )
                show(img)
                print("雷达图已更新")
            except Exception as exc:
                print(f"刷新失败: {exc}")
                show(make_error_image("离线\n重试中..."))

            if args.once:
                break
            print(f"等待 {args.interval} 秒后刷新 ...")
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n退出")
    finally:
        if lcd:
            lcd.close()


if __name__ == "__main__":
    main()
