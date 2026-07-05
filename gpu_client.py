"""远程 GPU 状态客户端（SSH + nvidia-smi）。"""

from __future__ import annotations

import subprocess
import threading
import time
from dataclasses import dataclass
from typing import Callable

NVIDIA_SMI_QUERY = (
    "nvidia-smi --query-gpu=index,utilization.gpu,memory.used,memory.total "
    "--format=csv,noheader,nounits"
)
EXPECTED_GPU_COUNT = 2


class GpuError(RuntimeError):
    pass


@dataclass
class GpuStats:
    index: int
    util_pct: int
    mem_used_mb: int
    mem_total_mb: int


def _mb_to_gb(mb: int) -> int:
    return max(0, mb // 1024)


def format_gpu_lcd_line(stats: GpuStats) -> str:
    used_g = _mb_to_gb(stats.mem_used_mb)
    total_g = _mb_to_gb(stats.mem_total_mb)
    return f"GPU{stats.index} {stats.util_pct}% {used_g}/{total_g}G"


def format_gpu_lcd_lines(stats: list[GpuStats]) -> tuple[str, str]:
    if len(stats) < EXPECTED_GPU_COUNT:
        raise GpuError(f"expected {EXPECTED_GPU_COUNT} GPUs, got {len(stats)}")
    ordered = sorted(stats, key=lambda s: s.index)
    return format_gpu_lcd_line(ordered[0]), format_gpu_lcd_line(ordered[1])


def _parse_nvidia_smi_output(text: str) -> list[GpuStats]:
    stats: list[GpuStats] = []
    for line in text.strip().splitlines():
        line = line.strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) != 4:
            raise GpuError(f"bad nvidia-smi line: {line!r}")
        try:
            stats.append(
                GpuStats(
                    index=int(parts[0]),
                    util_pct=int(float(parts[1])),
                    mem_used_mb=int(float(parts[2])),
                    mem_total_mb=int(float(parts[3])),
                )
            )
        except (TypeError, ValueError) as exc:
            raise GpuError(f"bad nvidia-smi values: {line!r}") from exc
    if len(stats) < EXPECTED_GPU_COUNT:
        raise GpuError(f"expected {EXPECTED_GPU_COUNT} GPUs, got {len(stats)}")
    return stats


class GpuClient:
    """通过 SSH 查询远程 GPU 占用率，带 TTL 缓存。"""

    def __init__(
        self,
        host: str = "5090",
        user: str = "js",
        timeout_sec: float = 5.0,
        cache_ttl_sec: float = 3.0,
    ) -> None:
        self._host = str(host)
        self._user = str(user)
        self._timeout = max(1.0, float(timeout_sec))
        self._ttl = max(0.0, float(cache_ttl_sec))
        self._lock = threading.Lock()
        self._cache: tuple[float, list[GpuStats]] | None = None

    def _ssh_target(self) -> str:
        return f"{self._user}@{self._host}"

    def _fetch_remote(self) -> list[GpuStats]:
        cmd = [
            "ssh",
            "-o", "BatchMode=yes",
            "-o", f"ConnectTimeout={int(self._timeout)}",
            "-o", "StrictHostKeyChecking=accept-new",
            self._ssh_target(),
            NVIDIA_SMI_QUERY,
        ]
        try:
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=self._timeout + 2.0,
                check=False,
            )
        except subprocess.TimeoutExpired as exc:
            raise GpuError("SSH timeout") from exc
        except OSError as exc:
            raise GpuError("SSH failed") from exc

        if proc.returncode != 0:
            detail = (proc.stderr or proc.stdout or "").strip()
            if "timed out" in detail.lower() or "timeout" in detail.lower():
                raise GpuError("SSH timeout")
            raise GpuError("SSH failed")

        try:
            return _parse_nvidia_smi_output(proc.stdout)
        except GpuError:
            raise
        except Exception as exc:
            raise GpuError("Bad data") from exc

    def fetch(self) -> list[GpuStats]:
        now = time.time()
        with self._lock:
            if self._cache is not None and now - self._cache[0] < self._ttl:
                return list(self._cache[1])

        stats = self._fetch_remote()
        with self._lock:
            self._cache = (now, stats)
        return list(stats)

    def fetch_async(
        self,
        on_ok: Callable[[list[GpuStats]], None],
        on_err: Callable[[GpuError], None],
    ) -> None:
        def _worker() -> None:
            try:
                stats = self.fetch()
            except GpuError as exc:
                on_err(exc)
            else:
                on_ok(stats)

        threading.Thread(target=_worker, daemon=True).start()
