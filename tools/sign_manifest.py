#!/usr/bin/env python3
"""Create the exact signed schema-2 manifest consumed by the device."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


PROJECT_RELEASE_PREFIX = (
    "https://github.com/JStone2934/DesktopRadar/releases/download/"
)
MAIN_LIMIT = 1_650_000


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--version-code", required=True, type=int)
    parser.add_argument("--tag")
    parser.add_argument("--build-sha", required=True)
    parser.add_argument("--notes", default="")
    parser.add_argument("--url")
    parser.add_argument("--published-at", type=int, default=int(time.time()))
    parser.add_argument("--key", type=Path,
                        default=os.environ.get("RADAR_SIGNING_KEY"))
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def make_payload(args: argparse.Namespace) -> bytes:
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        raise SystemExit("--version must be numeric semver, for example 0.2.1")
    if not re.fullmatch(r"[0-9a-fA-F]{40}", args.build_sha):
        raise SystemExit("--build-sha must contain exactly 40 hexadecimal digits")
    if not args.key or not args.key.is_file():
        raise SystemExit("provide --key or set RADAR_SIGNING_KEY")
    if not args.firmware.is_file():
        raise SystemExit(f"firmware not found: {args.firmware}")
    if len(args.notes.encode("utf-8")) > 256:
        raise SystemExit("--notes must be at most 256 UTF-8 bytes")
    size = args.firmware.stat().st_size
    if size <= 0 or size > MAIN_LIMIT:
        raise SystemExit(f"firmware size {size} exceeds release limit {MAIN_LIMIT}")
    tag = args.tag or f"v{args.version}"
    if tag != f"v{args.version}":
        raise SystemExit("tag must be exactly v<version>")
    asset_name = f"DesktopRadar-{tag}-esp32c3.bin"
    url = args.url or f"{PROJECT_RELEASE_PREFIX}{tag}/{asset_name}"
    if url != f"{PROJECT_RELEASE_PREFIX}{tag}/{asset_name}":
        raise SystemExit("URL must be the fixed release URL for this version")
    notes_b64 = base64.b64encode(args.notes.encode("utf-8")).decode("ascii")
    digest = hashlib.sha256(args.firmware.read_bytes()).hexdigest()
    fields = [
        ("manifest", "radar-update-v1"),
        ("key_id", "radar-prod-1"),
        ("channel", "stable"),
        ("version", args.version),
        ("version_code", str(args.version_code)),
        ("tag", tag),
        ("build_sha", args.build_sha.lower()),
        ("chip", "esp32c3"),
        ("layout", "radar-4m-recovery-v1"),
        ("size", str(size)),
        ("sha256", digest),
        ("url", url),
        ("min_recovery", "1"),
        ("published_at", str(args.published_at)),
        ("notes_b64", notes_b64),
    ]
    return ("".join(f"{key}={value}\n" for key, value in fields)).encode()


def sign(key: Path, payload: bytes) -> bytes:
    with tempfile.TemporaryDirectory(prefix="radar-sign-") as directory:
        payload_path = Path(directory) / "payload.txt"
        signature_path = Path(directory) / "signature.der"
        payload_path.write_bytes(payload)
        subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", str(key),
             "-out", str(signature_path), str(payload_path)],
            check=True,
        )
        return signature_path.read_bytes()


def main() -> None:
    args = parse_args()
    payload = make_payload(args)
    signature = sign(args.key, payload)
    manifest = {
        "schema": 2,
        "algorithm": "ecdsa-p256-sha256",
        "key_id": "radar-prod-1",
        "payload_b64": base64.b64encode(payload).decode("ascii"),
        "signature_b64": base64.b64encode(signature).decode("ascii"),
    }
    encoded = (json.dumps(manifest, separators=(",", ":")) + "\n").encode()
    if len(encoded) > 4096:
        raise SystemExit(f"manifest is {len(encoded)} bytes, maximum is 4096")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(encoded)
    print(f"wrote {args.output} ({len(encoded)} bytes)")


if __name__ == "__main__":
    main()
