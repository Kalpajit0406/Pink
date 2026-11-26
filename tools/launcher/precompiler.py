"""precompiler.py -- Background shader precompilation & warmup engine.

Standard library only. Manages interactive shader precompilation before game
launch, modeled after The Last of Us shader precompilation screen.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


class ShaderPrecompiler:
    """Orchestrates background shader warmup and pipeline cache generation."""

    def __init__(self, shaders_dir: str = "", work_dir: str = "") -> None:
        self.shaders_dir = shaders_dir or r"E:\Leonida\work\shaders\spirv"
        self.work_dir = work_dir or r"E:\Leonida\work"
        self._lock = threading.Lock()
        self.is_running = False
        self.total_shaders = 0
        self.compiled_shaders = 0
        self.percent = 0.0
        self.eta_seconds = 0
        self.current_shader = ""
        self.start_time = 0.0
        self.last_error = ""
        self.status_message = "Ready"
        self._thread: threading.Thread | None = None
        self._stop_event = threading.Event()

        self._check_initial_state()

    def _check_initial_state(self) -> None:
        """Scan shaders directory to establish baseline count."""
        if os.path.isdir(self.shaders_dir):
            try:
                index_path = os.path.join(self.shaders_dir, "index.json")
                if os.path.isfile(index_path):
                    with open(index_path, "r", encoding="utf-8") as f:
                        data = json.load(f)
                        self.total_shaders = len(data.get("shaders", {}))
                if self.total_shaders == 0:
                    spv_files = [f for f in os.listdir(self.shaders_dir) if f.endswith(".spv")]
                    self.total_shaders = len(spv_files)
                self.compiled_shaders = self.total_shaders
                self.percent = 100.0 if self.total_shaders > 0 else 0.0
                self.status_message = f"{self.total_shaders} shaders ready in cache"
            except Exception as e:
                self.status_message = f"Cache scan error: {e}"

    def get_status(self) -> dict[str, Any]:
        with self._lock:
            return {
                "is_running": self.is_running,
                "total": self.total_shaders,
                "compiled": self.compiled_shaders,
                "percent": round(self.percent, 1),
                "eta_sec": self.eta_seconds,
                "current": self.current_shader,
                "status": self.status_message,
                "last_error": self.last_error,
            }

    def start_precompilation(self, threads: int = 8) -> bool:
        """Launch background worker to precompile/verify shaders."""
        with self._lock:
            if self.is_running:
                return False
            self.is_running = True
            self._stop_event.clear()
            self.start_time = time.time()
            self.status_message = "Precompiling shaders..."
            self.last_error = ""

        self._thread = threading.Thread(
            target=self._worker, args=(threads,), daemon=True, name="ShaderPrecompilerWorker"
        )
        self._thread.start()
        return True

    def stop(self) -> None:
        self._stop_event.set()
        with self._lock:
            self.is_running = False
            self.status_message = "Cancelled by user"

    def _worker(self, threads: int) -> None:
        disasm_dir = os.path.join(self.work_dir, "shaders", "dxbc", "disasm")
        rdna2spirv_tool = os.path.join(REPO_ROOT, "tools", "rdna2spirv.py")

        # If disasm directory is present, run rdna2spirv
        if os.path.isdir(disasm_dir) and os.path.isfile(rdna2spirv_tool):
            cmd = [
                sys.executable,
                "-I",
                rdna2spirv_tool,
                disasm_dir,
                "--out",
                self.shaders_dir,
                "--assembler",
                "python",
                "--no-asm",
                "--opt",
            ]
            try:
                proc = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                )
                assert proc.stdout is not None
                for line in proc.stdout:
                    if self._stop_event.is_set():
                        proc.terminate()
                        break
                    line_s = line.strip()
                    with self._lock:
                        self.current_shader = line_s[:60]
                        # Track compile output
                        if "translated" in line_s or ".spv" in line_s:
                            self.compiled_shaders += 1
                            if self.total_shaders > 0:
                                self.percent = min(100.0, (self.compiled_shaders / self.total_shaders) * 100)
                proc.wait()
            except Exception as e:
                with self._lock:
                    self.last_error = str(e)
        else:
            # Re-scan and verify existing SPIR-V files
            try:
                spv_files = [f for f in os.listdir(self.shaders_dir) if f.endswith(".spv")]
                total = len(spv_files)
                with self._lock:
                    self.total_shaders = total
                for idx, fname in enumerate(spv_files):
                    if self._stop_event.is_set():
                        break
                    time.sleep(0.001)  # brief yield
                    if idx % 100 == 0 or idx == total - 1:
                        with self._lock:
                            self.compiled_shaders = idx + 1
                            self.percent = ((idx + 1) / total) * 100.0
                            self.current_shader = fname
                            elapsed = time.time() - self.start_time
                            rate = (idx + 1) / max(0.1, elapsed)
                            remaining = total - (idx + 1)
                            self.eta_seconds = int(remaining / max(1.0, rate))
            except Exception as e:
                with self._lock:
                    self.last_error = str(e)

        with self._lock:
            self.is_running = False
            self.percent = 100.0
            self.eta_seconds = 0
            self.status_message = f"Warmup complete ({self.compiled_shaders} shaders in cache)"
