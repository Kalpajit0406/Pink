"""game_process.py -- Game process lifecycle, priority elevation & real-time telemetry extraction.

Standard library only (subprocess, threading, ctypes). Manages running gta6rt,
elevates process scheduling priority, maintains 1ms timer precision, and scrapes
instantaneous telemetry metrics (FPS, frametime, memory, ReBAR, audio).
"""
from __future__ import annotations

import collections
import ctypes
from ctypes import wintypes
import os
import queue
import re
import subprocess
import sys
import threading
import time
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# Win32 Process Priority Constants
REALTIME_PRIORITY_CLASS = 0x00000100
HIGH_PRIORITY_CLASS = 0x00000080
ABOVE_NORMAL_PRIORITY_CLASS = 0x00008000
NORMAL_PRIORITY_CLASS = 0x00000020

# Multimedia timer
try:
    _winmm = ctypes.windll.winmm
except (AttributeError, OSError):
    _winmm = None

try:
    _kernel32 = ctypes.windll.kernel32
except (AttributeError, OSError):
    _kernel32 = None


def set_multimedia_timer(enable: bool) -> None:
    """Toggle 1ms scheduler timer resolution via winmm timeBeginPeriod/timeEndPeriod."""
    if _winmm:
        try:
            if enable:
                _winmm.timeBeginPeriod(1)
            else:
                _winmm.timeEndPeriod(1)
        except Exception:
            pass


def elevate_process_priority(pid: int, priority_name: str = "realtime") -> bool:
    """Elevate priority of specified PID using Win32 SetPriorityClass."""
    if not _kernel32 or pid <= 0:
        return False

    desired_class = HIGH_PRIORITY_CLASS
    if priority_name.lower() == "realtime":
        desired_class = REALTIME_PRIORITY_CLASS
    elif priority_name.lower() == "above_normal":
        desired_class = ABOVE_NORMAL_PRIORITY_CLASS

    PROCESS_SET_INFORMATION = 0x0200
    PROCESS_QUERY_INFORMATION = 0x0400
    handle = _kernel32.OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, False, pid)
    if not handle:
        return False

    success = bool(_kernel32.SetPriorityClass(handle, desired_class))
    if not success and desired_class == REALTIME_PRIORITY_CLASS:
        # Fall back to HIGH if REALTIME requires elevated privilege
        success = bool(_kernel32.SetPriorityClass(handle, HIGH_PRIORITY_CLASS))

    _kernel32.CloseHandle(handle)
    return success


class GameProcessManager:
    """Coordinates game execution, log streaming, and telemetry gathering."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self.proc: subprocess.Popen | None = None
        self.pid: int = 0
        self.run_name: str = ""
        self.start_time: float = 0.0
        self.exit_code: int | None = None
        self.status: str = "Ready"  # Ready, Running, Stopping, Terminated, Crashed

        # Ring buffer of recent log lines for newly connected clients
        self.log_ring_buffer: collections.deque[dict[str, Any]] = collections.deque(maxlen=2500)
        # SSE listener queues
        self.subscribers: list[queue.Queue[dict[str, Any]]] = []

        # Real-time telemetry metrics
        self.metrics: dict[str, Any] = {
            "fps": 0.0,
            "frametime_ms": 0.0,
            "p50_ms": 0.0,
            "p95_ms": 0.0,
            "p99_ms": 0.0,
            "present_count": 0,
            "vram_used_mb": 0,
            "host_ram_used_mb": 0,
            "dstorage_mb_s": 0.0,
            "audio_underruns": 0,
            "faults": [],
        }
        self._recent_frametimes: collections.deque[float] = collections.deque(maxlen=100)
        self._last_present_time: float = 0.0
        self._out_dir: str = ""

    def is_running(self) -> bool:
        with self._lock:
            return self.proc is not None and self.proc.poll() is None

    def get_state(self) -> dict[str, Any]:
        with self._lock:
            running = self.proc is not None and self.proc.poll() is None
            elapsed = time.time() - self.start_time if running else 0.0
            return {
                "status": self.status,
                "is_running": running,
                "pid": self.pid,
                "run_name": self.run_name,
                "elapsed_sec": round(elapsed, 1),
                "exit_code": self.exit_code,
                "metrics": dict(self.metrics),
            }

    def subscribe(self) -> queue.Queue[dict[str, Any]]:
        """Register a new SSE client queue."""
        q: queue.Queue[dict[str, Any]] = queue.Queue(maxsize=1000)
        with self._lock:
            # Replay recent history
            for item in self.log_ring_buffer:
                q.put_nowait(item)
            self.subscribers.append(q)
        return q

    def unsubscribe(self, q: queue.Queue[dict[str, Any]]) -> None:
        with self._lock:
            if q in self.subscribers:
                self.subscribers.remove(q)

    def _broadcast(self, item: dict[str, Any]) -> None:
        with self._lock:
            self.log_ring_buffer.append(item)
            dead = []
            for q in self.subscribers:
                try:
                    q.put_nowait(item)
                except queue.Full:
                    dead.append(q)
            for d in dead:
                self.subscribers.remove(d)

    def launch(
        self,
        env_vars: dict[str, str],
        runtime_path: str,
        game_dir: str,
        exe_path: str,
        shaders_dir: str,
        runs_dir: str,
        seconds_limit: int = 300,
        run_name: str = "",
        input_script: str = "",
        priority: str = "realtime",
    ) -> tuple[bool, str]:
        """Launch the game via scripts/windows-boot/boot.ps1 harness."""
        with self._lock:
            if self.proc is not None and self.proc.poll() is None:
                return False, f"Game already running under PID {self.pid}"

        name = run_name or f"run_{time.strftime('%Y%m%d_%H%M%S')}"
        self.run_name = name
        self._out_dir = runs_dir
        self.exit_code = None
        self.status = "Starting..."
        self._recent_frametimes.clear()
        self._last_present_time = 0.0
        self.metrics = {
            "fps": 0.0,
            "frametime_ms": 0.0,
            "p50_ms": 0.0,
            "p95_ms": 0.0,
            "p99_ms": 0.0,
            "present_count": 0,
            "vram_used_mb": 0,
            "host_ram_used_mb": 0,
            "dstorage_mb_s": 0.0,
            "audio_underruns": 0,
            "faults": [],
        }

        # Build boot.ps1 command
        boot_script = os.path.join(REPO_ROOT, "scripts", "windows-boot", "boot.ps1")
        cmd = [
            "powershell",
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            boot_script,
            "-Name",
            name,
            "-Seconds",
            str(seconds_limit),
            "-Runtime",
            runtime_path,
            "-GameDir",
            game_dir,
            "-Exe",
            exe_path,
            "-Shaders",
            shaders_dir,
            "-OutDir",
            runs_dir,
        ]

        if input_script:
            cmd.extend(["-InputScript", input_script])
        else:
            cmd.append("-NoInput")

        # Pass custom environment
        merged_env = os.environ.copy()
        for k, v in env_vars.items():
            merged_env[k] = str(v)
            cmd.append(f"{k}={v}")

        # Enable multimedia timer
        set_multimedia_timer(True)

        try:
            self.proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                stdin=subprocess.DEVNULL,
                text=True,
                bufsize=1,
                cwd=REPO_ROOT,
                env=merged_env,
            )
            self.pid = self.proc.pid
            self.start_time = time.time()
            self.status = "Running"
        except Exception as e:
            set_multimedia_timer(False)
            self.status = "Failed"
            return False, f"Failed to spawn game process: {e}"

        # Elevate priority
        elevate_process_priority(self.pid, priority)

        # Start pipe listener threads
        t_out = threading.Thread(
            target=self._pipe_reader, args=(self.proc.stdout, "stdout"), daemon=True
        )
        t_err = threading.Thread(
            target=self._pipe_reader, args=(self.proc.stderr, "stderr"), daemon=True
        )
        t_mon = threading.Thread(
            target=self._monitor_proc, args=(priority,), daemon=True
        )
        t_out.start()
        t_err.start()
        t_mon.start()

        self._broadcast({
            "type": "event",
            "event": "started",
            "run_name": self.run_name,
            "pid": self.pid,
            "time": time.time(),
        })

        return True, f"Game launched successfully (PID {self.pid}, Run {name})"

    def _pipe_reader(self, pipe: Any, stream_name: str) -> None:
        if not pipe:
            return
        for line in pipe:
            line_s = line.rstrip()
            if not line_s:
                continue

            self._parse_telemetry_line(line_s)

            item = {
                "type": "log",
                "stream": stream_name,
                "line": line_s,
                "time": time.time(),
            }
            self._broadcast(item)

    def _parse_telemetry_line(self, line: str) -> None:
        now = time.time()

        # Check for Present lines
        if "PresentX ok" in line or "Present #" in line or "PresentX" in line:
            self.metrics["present_count"] += 1
            if self._last_present_time > 0:
                dt_ms = (now - self._last_present_time) * 1000.0
                if 2.0 < dt_ms < 500.0:
                    self._recent_frametimes.append(dt_ms)
                    self.metrics["frametime_ms"] = round(dt_ms, 2)
                    self.metrics["fps"] = round(1000.0 / dt_ms, 1)

                    # Compute percentiles
                    sorted_ft = sorted(self._recent_frametimes)
                    n = len(sorted_ft)
                    if n >= 5:
                        self.metrics["p50_ms"] = round(sorted_ft[int(n * 0.50)], 2)
                        self.metrics["p95_ms"] = round(sorted_ft[min(n - 1, int(n * 0.95))], 2)
                        self.metrics["p99_ms"] = round(sorted_ft[min(n - 1, int(n * 0.99))], 2)
            self._last_present_time = now

        # Check for DirectStorage decompression
        if "decompressed" in line and "MB/s" in line:
            m = re.search(r"(\d+(\.\d+)?)\s*MB/s", line)
            if m:
                self.metrics["dstorage_mb_s"] = float(m.group(1))

        # Check for Memory reports
        if "title memory" in line or "VRAM" in line:
            vm = re.search(r"VRAM[:\s]+(\d+)\s*MB", line, re.IGNORECASE)
            if vm:
                self.metrics["vram_used_mb"] = int(vm.group(1))
            rm = re.search(r"host[:\s]+(\d+)\s*MB", line, re.IGNORECASE)
            if rm:
                self.metrics["host_ram_used_mb"] = int(rm.group(1))

        # Check for Audio starvation / underrun
        if "audio underrun" in line.lower() or "starvation" in line.lower():
            self.metrics["audio_underruns"] += 1

        # Check for faults
        if "fault" in line.lower() or "exception" in line.lower() or "crash" in line.lower():
            if len(self.metrics["faults"]) < 10:
                self.metrics["faults"].append(line[:120])

    def _monitor_proc(self, priority: str) -> None:
        """Watches child process and handles child gta6rt.exe priority elevation."""
        if not self.proc:
            return

        # Poll periodically to elevate child gta6rt.exe if spawned under powershell
        elevated_child = False
        while self.proc.poll() is None:
            if not elevated_child:
                # Find gta6rt.exe child process
                try:
                    out = subprocess.check_output(
                        ["tasklist", "/FI", "IMAGENAME eq gta6rt.exe", "/FO", "CSV", "/NH"],
                        text=True,
                    )
                    for row in out.strip().splitlines():
                        parts = [p.strip('"') for p in row.split('","')]
                        if len(parts) >= 2 and parts[0].lower() == "gta6rt.exe":
                            c_pid = int(parts[1])
                            elevate_process_priority(c_pid, priority)
                            elevated_child = True
                            break
                except Exception:
                    pass

            # Emit periodic metrics ping
            self._broadcast({
                "type": "metrics",
                "metrics": dict(self.metrics),
                "elapsed": round(time.time() - self.start_time, 1),
            })
            time.sleep(0.5)

        ret = self.proc.poll()
        set_multimedia_timer(False)
        with self._lock:
            self.exit_code = ret
            if ret == 0:
                self.status = "Terminated (Exit 0)"
            elif ret is not None and ret < 0:
                self.status = f"Crashed (Exit {ret})"
            else:
                self.status = f"Stopped (Exit {ret})"

        # Run boot_trace.py post-run analysis if summary file missing
        summary_path = os.path.join(self._out_dir, f"{self.run_name}.summary.txt")
        jsonl_path = os.path.join(self._out_dir, f"{self.run_name}.jsonl")
        log_path = os.path.join(self._out_dir, f"{self.run_name}.log")

        if not os.path.isfile(summary_path) and os.path.isfile(jsonl_path):
            trace_tool = os.path.join(REPO_ROOT, "tools", "boot_trace.py")
            if os.path.isfile(trace_tool):
                try:
                    subprocess.run(
                        [sys.executable, "-I", trace_tool, jsonl_path, log_path, "--out", summary_path],
                        timeout=15,
                    )
                except Exception:
                    pass

        self._broadcast({
            "type": "event",
            "event": "stopped",
            "run_name": self.run_name,
            "exit_code": ret,
            "time": time.time(),
        })

    def stop(self, force: bool = False) -> bool:
        """Stop running game process."""
        with self._lock:
            if not self.proc or self.proc.poll() is not None:
                return True
            proc = self.proc

        self.status = "Stopping..."

        # Kill gta6rt.exe and powershell trees forcefully if force or timeout
        try:
            subprocess.run(
                ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass

        # Also ensure orphan gta6rt.exe is killed
        try:
            subprocess.run(
                ["taskkill", "/F", "/IM", "gta6rt.exe"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
        except Exception:
            pass

        set_multimedia_timer(False)
        return True
