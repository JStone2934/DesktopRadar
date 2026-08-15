#!/usr/bin/env python3
"""Build and package an OTA release plus the first-install USB image."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PIO = Path.home() / ".platformio" / "penv" / "bin" / "platformio"
MAIN_BUILD = ROOT / ".pio/build/esp32-c3-supermini"
FACTORY_BUILD = ROOT / "factory/.pio/build/esp32-c3-recovery"


def run(command: list[str], *, cwd: Path = ROOT,
        environment: dict[str, str] | None = None) -> None:
    subprocess.run(command, cwd=cwd, env=environment, check=True)


def patch_image(image: bytearray, offset: int, path: Path) -> None:
    data = path.read_bytes()
    end = offset + len(data)
    if end > len(image):
        raise SystemExit(f"{path} does not fit at 0x{offset:x}")
    image[offset:end] = data


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--version-code", required=True, type=int)
    parser.add_argument("--notes", default="")
    parser.add_argument("--build-sha")
    parser.add_argument("--key", type=Path,
                        default=os.environ.get("RADAR_SIGNING_KEY"))
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--skip-build", action="store_true",
                        help="package already-built images (verification only)")
    parser.add_argument("--allow-dirty", action="store_true")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.version_code <= 0 or args.version_code > 0xFFFFFFFF:
        raise SystemExit("--version-code must be between 1 and 4294967295")
    tag = f"v{args.version}"
    build_sha = args.build_sha or subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    if not args.key:
        raise SystemExit("provide --key or set RADAR_SIGNING_KEY")
    if not args.allow_dirty and subprocess.check_output(
        ["git", "status", "--porcelain"], cwd=ROOT, text=True
    ).strip():
        raise SystemExit("release build requires a clean worktree")
    output = args.output_dir or ROOT / "dist" / tag
    environment = os.environ.copy()
    environment["PLATFORMIO_BUILD_FLAGS"] = (
        "-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1 "
        "-DCORE_DEBUG_LEVEL=1 "
        f'-DRADAR_VERSION=\\"{args.version}\\" '
        f"-DRADAR_VERSION_CODE={args.version_code}U "
        f'-DRADAR_BUILD_SHA=\\"{build_sha}\\"'
    )

    if not args.skip_build:
        run([str(PIO), "run", "-e", "esp32-c3-supermini"],
            environment=environment)
        run([str(PIO), "run", "-d", "factory", "-e", "esp32-c3-recovery"])

    output.mkdir(parents=True, exist_ok=True)
    ota_name = f"DesktopRadar-{tag}-esp32c3.bin"
    ota = output / ota_name
    recovery = output / "factory-recovery-1.bin"
    shutil.copy2(MAIN_BUILD / "firmware.bin", ota)
    shutil.copy2(FACTORY_BUILD / "firmware.bin", recovery)
    if ota.stat().st_size > 1_650_000:
        raise SystemExit("main image exceeds 1,650,000-byte release cap")
    if recovery.stat().st_size > 500 * 1024:
        raise SystemExit("factory image exceeds 500-KiB release cap")

    # This recovery image deliberately leaves the NVS range as 0xff. It is for
    # full erase/recovery only; the NVS-preserving migration uses
    # tools/install_base.py and the individual images above.
    full = bytearray(b"\xff" * (4 * 1024 * 1024))
    patch_image(full, 0x0000, FACTORY_BUILD / "bootloader.bin")
    patch_image(full, 0x8000, FACTORY_BUILD / "partitions.bin")
    patch_image(full, 0xE000, FACTORY_BUILD / "ota_data_initial.bin")
    patch_image(full, 0x10000, recovery)
    patch_image(full, 0x90000, ota)
    usb = output / f"DesktopRadar-{tag}-usb-full.bin"
    usb.write_bytes(full)

    latest = output / "latest.json"
    run([
        sys.executable, str(ROOT / "tools/sign_manifest.py"),
        "--firmware", str(ota), "--version", args.version,
        "--version-code", str(args.version_code), "--build-sha", build_sha,
        "--notes", args.notes, "--key", str(args.key),
        "--output", str(latest),
    ])
    checksum = output / "SHA256SUMS"
    assets = [ota, recovery, usb, latest]
    checksum.write_text("".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
        for path in assets
    ))
    print(f"release package ready: {output}")


if __name__ == "__main__":
    main()
