"""server.py -- Unified Launcher local backend server.

Standard library only (http.server, urllib, json, mimetypes, subprocess).
Binds strictly to 127.0.0.1:8766 with DNS rebinding protection and provides
REST APIs and Server-Sent Events (SSE) for the launcher frontend.
"""
from __future__ import annotations

import argparse
import http.server
import json
import mimetypes
import os
import queue
import sys
import threading
import time
import urllib.parse
from typing import Any

# Ensure project root is in sys.path
HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)

from tools.launcher.config_manager import ConfigManager
from tools.launcher.game_process import GameProcessManager
from tools.launcher.input_probe import find_first_connected_gamepad, probe_gamepad
from tools.launcher.precompiler import ShaderPrecompiler
from tools.launcher.session_manager import SessionManager

_LOOPBACK_HOSTS = frozenset({"127.0.0.1", "localhost", "[::1]"})


def host_allowed(host_header: str | None) -> bool:
    """DNS rebinding defense: ensures Host header refers strictly to loopback."""
    if not host_header:
        return True
    host_only = host_header.split(":")[0].strip().lower()
    return host_only in _LOOPBACK_HOSTS


class LauncherRequestHandler(http.server.BaseHTTPRequestHandler):
    """HTTP and SSE Request Handler for the Unified Launcher."""

    server_version = "LeonidaLauncher/1.0"

    def do_OPTIONS(self) -> None:
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self) -> None:
        if not host_allowed(self.headers.get("Host")):
            self.send_error(403, "Host header not allowed (DNS rebinding protection)")
            return

        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path in ("/", "/index.html"):
            self._serve_static("index.html", "text/html; charset=utf-8")
        elif path.startswith("/static/"):
            rel_file = path[len("/static/") :]
            self._serve_static(rel_file)
        elif path == "/api/status":
            self._handle_api_status()
        elif path == "/api/config":
            self._handle_api_config()
        elif path == "/api/runs":
            self._handle_api_runs(parsed.query)
        elif path.startswith("/api/runs/"):
            self._handle_api_run_routes(path)
        elif path == "/api/precompile/status":
            self._handle_api_precompile_status()
        elif path == "/api/input/test":
            self._handle_api_input_test()
        elif path == "/api/stream":
            self._handle_api_sse_stream()
        else:
            self.send_error(404, "Endpoint Not Found")

    def do_POST(self) -> None:
        if not host_allowed(self.headers.get("Host")):
            self.send_error(403, "Host header not allowed (DNS rebinding protection)")
            return

        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        content_len = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_len) if content_len > 0 else b"{}"

        try:
            payload = json.loads(body.decode("utf-8")) if body else {}
        except Exception:
            payload = {}

        if path == "/api/config":
            self._handle_api_save_config(payload)
        elif path == "/api/launch":
            self._handle_api_launch(payload)
        elif path == "/api/stop":
            self._handle_api_stop(payload)
        elif path == "/api/precompile":
            self._handle_api_start_precompile(payload)
        elif path == "/api/precompile/stop":
            self._handle_api_stop_precompile()
        else:
            self.send_error(404, "Endpoint Not Found")

    def _send_json(self, data: Any, code: int = 200) -> None:
        body = json.dumps(data, indent=2).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _serve_static(self, rel_path: str, content_type: str = "") -> None:
        clean_rel = os.path.normpath(rel_path).lstrip("\\/")
        full_path = os.path.join(HERE, "web", clean_rel)
        if not os.path.isfile(full_path):
            self.send_error(404, f"Static file {rel_path} not found")
            return

        if not content_type:
            content_type, _ = mimetypes.guess_type(full_path)
            content_type = content_type or "application/octet-stream"

        try:
            with open(full_path, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)
        except OSError as e:
            self.send_error(500, f"Error reading file: {e}")

    def _handle_api_status(self) -> None:
        state = self.server.proc_mgr.get_state()
        state["gamepad"] = find_first_connected_gamepad()
        state["precompiler"] = self.server.precompiler.get_status()
        self._send_json(state)

    def _handle_api_config(self) -> None:
        self._send_json(self.server.cfg_mgr.get_config())

    def _handle_api_save_config(self, payload: dict) -> None:
        updated = self.server.cfg_mgr.update_config(payload)
        # Update runs search path in session manager if paths changed
        paths = updated.get("paths", {})
        self.server.session_mgr = SessionManager([paths.get("runs_dir", r"E:\Leonida\runs"), r"E:\Leonida"])
        self._send_json({"success": True, "config": updated})

    def _handle_api_launch(self, payload: dict) -> None:
        cfg = self.server.cfg_mgr.get_config()
        paths = cfg.get("paths", {})
        settings = cfg.get("settings", {})

        profile_id = payload.get("profile") or cfg.get("active_profile", "max_performance")
        env_vars = self.server.cfg_mgr.get_effective_env(profile_id)

        # Merge any user overrides sent in payload
        if "env_overrides" in payload and isinstance(payload["env_overrides"], dict):
            env_vars.update(payload["env_overrides"])

        run_name = payload.get("run_name", "")
        seconds_limit = int(payload.get("seconds_limit", settings.get("seconds_limit", 300)))
        input_script = (
            settings.get("input_script", "")
            if settings.get("enable_input_script", False)
            else ""
        )
        priority = env_vars.get("LEONIDA_PRIORITY", "realtime")

        ok, msg = self.server.proc_mgr.launch(
            env_vars=env_vars,
            runtime_path=paths.get("runtime_path", ""),
            game_dir=paths.get("game_dir", ""),
            exe_path=paths.get("exe_path", ""),
            shaders_dir=paths.get("shaders_dir", ""),
            runs_dir=paths.get("runs_dir", ""),
            seconds_limit=seconds_limit,
            run_name=run_name,
            input_script=input_script,
            priority=priority,
        )
        code = 200 if ok else 400
        self._send_json({"success": ok, "message": msg, "state": self.server.proc_mgr.get_state()}, code=code)

    def _handle_api_stop(self, payload: dict) -> None:
        force = bool(payload.get("force", False))
        ok = self.server.proc_mgr.stop(force=force)
        self._send_json({"success": ok, "state": self.server.proc_mgr.get_state()})

    def _handle_api_runs(self, query_str: str) -> None:
        query_params = urllib.parse.parse_qs(query_str)
        limit = int(query_params.get("limit", ["100"])[0])
        search = query_params.get("q", [""])[0]
        runs = self.server.session_mgr.list_runs(limit=limit, query=search)
        self._send_json({"runs": runs, "count": len(runs)})

    def _handle_api_run_routes(self, path: str) -> None:
        parts = path.strip("/").split("/")
        # /api/runs/<name>
        # /api/runs/<name>/log
        # /api/runs/<name>/captures
        # /api/runs/<name>/capture/<filename>
        if len(parts) == 3:
            run_name = parts[2]
            detail = self.server.session_mgr.get_run_detail(run_name)
            if detail is None:
                self.send_error(404, f"Run {run_name} not found")
                return
            self._send_json(detail)
        elif len(parts) == 4 and parts[3] == "log":
            run_name = parts[2]
            log_text = self.server.session_mgr.get_full_log(run_name, stream="log")
            self._send_json({"name": run_name, "log": log_text})
        elif len(parts) == 4 and parts[3] == "captures":
            run_name = parts[2]
            caps = self.server.session_mgr.list_captures(run_name)
            self._send_json({"name": run_name, "captures": caps})
        elif len(parts) == 5 and parts[3] == "capture":
            run_name = parts[2]
            cap_file = parts[4]
            png_bytes = self.server.session_mgr.get_capture_png_bytes(run_name, cap_file)
            if png_bytes is None:
                self.send_error(404, f"Capture frame {cap_file} not found")
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Content-Length", str(len(png_bytes)))
            self.send_header("Cache-Control", "public, max-age=86400")
            self.end_headers()
            self.wfile.write(png_bytes)
        else:
            self.send_error(404, "Invalid Run Route")

    def _handle_api_precompile_status(self) -> None:
        status = self.server.precompiler.get_status()
        self._send_json(status)

    def _handle_api_start_precompile(self, payload: dict) -> None:
        threads = int(payload.get("threads", 8))
        ok = self.server.precompiler.start_precompilation(threads=threads)
        self._send_json({"success": ok, "status": self.server.precompiler.get_status()})

    def _handle_api_stop_precompile(self) -> None:
        self.server.precompiler.stop()
        self._send_json({"success": True, "status": self.server.precompiler.get_status()})

    def _handle_api_input_test(self) -> None:
        gp = find_first_connected_gamepad()
        self._send_json(gp)

    def _handle_api_sse_stream(self) -> None:
        """Handle real-time Server-Sent Events stream for logs and metrics."""
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

        sub_queue = self.server.proc_mgr.subscribe()

        try:
            # Initial heartbeat / ping
            init_msg = json.dumps({"type": "init", "state": self.server.proc_mgr.get_state()})
            self.wfile.write(f"data: {init_msg}\n\n".encode("utf-8"))
            self.wfile.flush()

            while True:
                try:
                    item = sub_queue.get(timeout=1.0)
                    line = f"data: {json.dumps(item)}\n\n".encode("utf-8")
                    self.wfile.write(line)
                    self.wfile.flush()
                except queue.Empty:
                    # Keep-alive heartbeat ping every second
                    ping = json.dumps({"type": "ping", "time": time.time()})
                    self.wfile.write(f"data: {ping}\n\n".encode("utf-8"))
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            self.server.proc_mgr.unsubscribe(sub_queue)


class LauncherServer(http.server.ThreadingHTTPServer):
    """Threaded HTTP Server holding global managers."""

    def __init__(self, server_address: tuple[str, int]) -> None:
        super().__init__(server_address, LauncherRequestHandler)
        self.cfg_mgr = ConfigManager()
        cfg = self.cfg_mgr.get_config()
        paths = cfg.get("paths", {})
        self.session_mgr = SessionManager([paths.get("runs_dir", r"E:\Leonida\runs"), r"E:\Leonida"])
        self.proc_mgr = GameProcessManager()
        self.precompiler = ShaderPrecompiler(
            shaders_dir=paths.get("shaders_dir", ""),
            work_dir=os.path.join(REPO_ROOT, "work"),
        )


def main() -> None:
    parser = argparse.ArgumentParser(description="Leonida Unified Launcher Server")
    parser.add_argument("--port", type=int, default=8766, help="Port to bind (default: 8766)")
    parser.add_argument("--open-edge", action="store_true", help="Automatically launch Microsoft Edge App Mode")
    args = parser.parse_args()

    server = LauncherServer(("127.0.0.1", args.port))
    url = f"http://127.0.0.1:{args.port}"
    print(f"[Leonida Launcher] Server active at {url}")

    if args.open_edge:
        def _launch_edge() -> None:
            time.sleep(0.5)
            edge_cmd = f'start "" msedge --app={url} --window-size=1280,860'
            os.system(edge_cmd)

        threading.Thread(target=_launch_edge, daemon=True).start()

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[Leonida Launcher] Shutting down...")
        server.proc_mgr.stop(force=True)
        server.server_close()


if __name__ == "__main__":
    main()
