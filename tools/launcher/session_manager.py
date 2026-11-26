"""session_manager.py -- Historical run session manager & telemetry analyzer.

Standard library only. Discovers, parses, and surfaces historical run summaries,
digests, stdout/stderr logs, startup JSONL events, and frame captures.
"""
from __future__ import annotations

import glob
import os
import re
from typing import Any

from .image_utils import ppm_to_png_bytes


class SessionManager:
    """Discovers and parses past Leonida boot sessions."""

    def __init__(self, runs_dirs: list[str]) -> None:
        self.runs_dirs = [d for d in runs_dirs if os.path.isdir(d)]
        self._cache_dir = os.path.join(os.path.expanduser("~"), ".leonida_launcher_cache")
        try:
            os.makedirs(self._cache_dir, exist_ok=True)
        except OSError:
            pass

    def _find_run_files(self, run_name: str) -> dict[str, str]:
        """Finds all associated files for a given run across all configured search paths."""
        found: dict[str, str] = {}
        for root in self.runs_dirs:
            candidates = {
                "summary": os.path.join(root, f"{run_name}.summary.txt"),
                "digest": os.path.join(root, f"{run_name}.digest.txt"),
                "log": os.path.join(root, f"{run_name}.log"),
                "stdout": os.path.join(root, f"{run_name}.stdout.log"),
                "jsonl": os.path.join(root, f"{run_name}.jsonl"),
                "cap_dir": os.path.join(root, f"cap_{run_name}"),
            }
            # Also check alternative naming like run68.err / run68.out in root
            if not os.path.isfile(candidates["log"]):
                alt_err = os.path.join(root, f"{run_name}.err")
                if os.path.isfile(alt_err):
                    candidates["log"] = alt_err
            if not os.path.isfile(candidates["stdout"]):
                alt_out = os.path.join(root, f"{run_name}.out")
                if os.path.isfile(alt_out):
                    candidates["stdout"] = alt_out

            for key, path in candidates.items():
                if key not in found:
                    if key == "cap_dir" and os.path.isdir(path):
                        found[key] = path
                    elif key != "cap_dir" and os.path.isfile(path):
                        found[key] = path
        return found

    def list_runs(self, limit: int = 100, query: str = "") -> list[dict[str, Any]]:
        """Scan all run directories and return sorted list of runs."""
        discovered: dict[str, dict[str, Any]] = {}

        for root in self.runs_dirs:
            try:
                entries = os.listdir(root)
            except OSError:
                continue

            for entry in entries:
                match = re.match(r"^(run[A-Za-z0-9_]+|boot-[0-9_-]+)", entry)
                if not match:
                    continue
                run_name = match.group(1)
                # Strip extensions
                if "." in run_name:
                    run_name = run_name.split(".")[0]

                if query and query.lower() not in run_name.lower():
                    continue

                if run_name in discovered:
                    continue

                files = self._find_run_files(run_name)
                if not files:
                    continue

                # Determine timestamp from most recent file
                mtime = 0.0
                for f_path in files.values():
                    try:
                        mtime = max(mtime, os.path.getmtime(f_path))
                    except OSError:
                        pass

                # Parse digest if available
                digest_text = ""
                presents = 0
                duration_sec = 0
                status = "Unknown"
                shaders_count = 0
                captures_count = 0

                if "digest" in files:
                    try:
                        with open(files["digest"], "r", encoding="utf-8", errors="replace") as df:
                            digest_text = df.read()
                            # parse duration
                            dur_m = re.search(r"\((\d+)\s*s\)", digest_text)
                            if dur_m:
                                duration_sec = int(dur_m.group(1))
                            if "stopped" in digest_text:
                                status = "Stopped (Time Limit)"
                            elif "fault" in digest_text.lower():
                                status = "Device Fault"
                            else:
                                status = "Completed"

                            pres_m = re.search(r"PresentX ok\s+(\d+)", digest_text)
                            if pres_m:
                                presents = int(pres_m.group(1))

                            shad_m = re.search(r"(\d+)\s+translated shaders", digest_text)
                            if shad_m:
                                shaders_count = int(shad_m.group(1))

                            cap_m = re.search(r"(\d+)\s+captured", digest_text)
                            if cap_m:
                                captures_count = int(cap_m.group(1))
                    except OSError:
                        pass

                if "cap_dir" in files and captures_count == 0:
                    try:
                        captures_count = len([
                            f for f in os.listdir(files["cap_dir"])
                            if f.endswith(".ppm") or f.endswith(".png")
                        ])
                    except OSError:
                        pass

                log_size = 0
                if "log" in files:
                    try:
                        log_size = os.path.getsize(files["log"])
                    except OSError:
                        pass

                discovered[run_name] = {
                    "name": run_name,
                    "mtime": mtime,
                    "status": status,
                    "duration_sec": duration_sec,
                    "presents": presents,
                    "shaders_count": shaders_count,
                    "captures_count": captures_count,
                    "log_size": log_size,
                    "files": files,
                }

        sorted_runs = sorted(discovered.values(), key=lambda r: r["mtime"], reverse=True)
        return sorted_runs[:limit]

    def get_run_detail(self, run_name: str) -> dict[str, Any] | None:
        """Returns deep run details including summary, digest, and tail of logs."""
        files = self._find_run_files(run_name)
        if not files:
            return None

        summary_text = ""
        if "summary" in files:
            try:
                with open(files["summary"], "r", encoding="utf-8", errors="replace") as f:
                    summary_text = f.read()
            except OSError:
                pass

        digest_text = ""
        if "digest" in files:
            try:
                with open(files["digest"], "r", encoding="utf-8", errors="replace") as f:
                    digest_text = f.read()
            except OSError:
                pass

        log_tail: list[str] = []
        if "log" in files:
            try:
                with open(files["log"], "r", encoding="utf-8", errors="replace") as f:
                    lines = f.readlines()
                    log_tail = [line.rstrip() for line in lines[-500:]]
            except OSError:
                pass

        captures = self.list_captures(run_name)

        return {
            "name": run_name,
            "files": files,
            "summary": summary_text,
            "digest": digest_text,
            "log_tail": log_tail,
            "captures": captures,
        }

    def get_full_log(self, run_name: str, stream: str = "log") -> str:
        """Fetch the full log file contents."""
        files = self._find_run_files(run_name)
        target = files.get(stream, "")
        if not target or not os.path.isfile(target):
            return ""
        try:
            with open(target, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def list_captures(self, run_name: str) -> list[dict[str, Any]]:
        """Returns list of captured frame files for a given run."""
        files = self._find_run_files(run_name)
        cap_dir = files.get("cap_dir")
        if not cap_dir or not os.path.isdir(cap_dir):
            return []

        results: list[dict[str, Any]] = []
        try:
            entries = sorted(os.listdir(cap_dir))
            for entry in entries:
                if entry.endswith(".ppm") or entry.endswith(".png"):
                    full_p = os.path.join(cap_dir, entry)
                    size = os.path.getsize(full_p)
                    results.append({
                        "filename": entry,
                        "size": size,
                        "is_ppm": entry.endswith(".ppm"),
                    })
        except OSError:
            pass
        return results

    def get_capture_png_bytes(self, run_name: str, filename: str, max_width: int = 0) -> bytes | None:
        """Fetch a capture frame converted to PNG bytes."""
        files = self._find_run_files(run_name)
        cap_dir = files.get("cap_dir")
        if not cap_dir:
            return None

        # Prevent path traversal
        clean_name = os.path.basename(filename)
        src_path = os.path.join(cap_dir, clean_name)
        if not os.path.isfile(src_path):
            return None

        if clean_name.endswith(".png"):
            with open(src_path, "rb") as f:
                return f.read()

        if clean_name.endswith(".ppm"):
            cache_file = os.path.join(self._cache_dir, f"{run_name}_{clean_name}_{max_width}.png")
            if os.path.isfile(cache_file):
                try:
                    with open(cache_file, "rb") as f:
                        return f.read()
                except OSError:
                    pass

            try:
                with open(src_path, "rb") as f:
                    ppm_data = f.read()
                png_bytes = ppm_to_png_bytes(ppm_data, max_width=max_width)
                try:
                    with open(cache_file, "wb") as f:
                        f.write(png_bytes)
                except OSError:
                    pass
                return png_bytes
            except Exception:
                return None

        return None
