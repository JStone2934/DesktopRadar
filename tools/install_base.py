#!/usr/bin/env python3
"""Install the v0.2 recovery layout over USB while preserving the NVS region."""

from __future__ import annotations

import argparse
from datetime import datetime
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
FACTORY_BUILD = ROOT / "factory/.pio/build/esp32-c3-recovery"
DEFAULT_PIO_PYTHON = Path.home() / ".platformio/penv/bin/python"
ESPTOOL_PYTHON = Path(os.environ.get(
    "RADAR_ESPTOOL_PYTHON",
    str(DEFAULT_PIO_PYTHON if DEFAULT_PIO_PYTHON.is_file()
        else Path(sys.executable)),
))


def validate_app_image(path: Path, maximum: int, label: str) -> None:
    size = path.stat().st_size
    with path.open("rb") as image:
        magic = image.read(1)
    if magic != b"\xe9" or size <= 0 or size > maximum:
        raise SystemExit(
            f"invalid {label} image: {path} ({size} bytes, magic={magic.hex()})"
        )


def command(port: str, *arguments: str) -> None:
    subprocess.run(
        [str(ESPTOOL_PYTHON), "-m", "esptool", "--chip", "esp32c3",
         "--port", port, *arguments],
        check=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--app", type=Path, required=True,
                        help="DesktopRadar-v0.2.0-esp32c3.bin")
    parser.add_argument("--factory", type=Path,
                        default=FACTORY_BUILD / "firmware.bin")
    parser.add_argument("--backup-dir", type=Path,
                        default=ROOT / "nvs-backups")
    args = parser.parse_args()
    for path in (args.app, args.factory, FACTORY_BUILD / "bootloader.bin",
                 FACTORY_BUILD / "partitions.bin"):
        if not path.is_file():
            raise SystemExit(f"required image is missing: {path}")
    validate_app_image(args.app, 1_650_000, "main")
    validate_app_image(args.factory, 500 * 1024, "factory")

    args.backup_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    nvs_backup = args.backup_dir / f"radar-nvs-{stamp}.bin"
    command(args.port, "read-flash", "0x9000", "0x5000", str(nvs_backup))

    # NVS 0x9000..0xdfff is deliberately excluded from every erase below.
    for offset, size in (
        ("0xe000", "0x2000"),       # otadata
        ("0x10000", "0x80000"),    # factory
        ("0x90000", "0x1b0000"),   # app0
        ("0x240000", "0x1b0000"),  # LittleFS
        ("0x3f0000", "0x10000"),   # coredump
    ):
        command(args.port, "erase-region", offset, size)
    command(
        args.port, "write-flash",
        "0x0", str(FACTORY_BUILD / "bootloader.bin"),
        "0x8000", str(FACTORY_BUILD / "partitions.bin"),
        "0x10000", str(args.factory),
        "0x90000", str(args.app),
    )
    print(f"base installation complete; NVS backup: {nvs_backup}")


if __name__ == "__main__":
    main()
