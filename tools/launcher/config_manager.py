"""config_manager.py -- Configuration & profile management for the Leonida Unified Launcher.

Standard library only. Handles loading, auto-detecting system paths, profile
definitions, and atomic JSON persistence.
"""
from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CONFIG_PATH = os.path.join(REPO_ROOT, "launcher_config.json")


def _read_side_file(path: str) -> str:
    if os.path.isfile(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                line = f.readline().strip()
                if line:
                    return line
        except OSError:
            pass
    return ""


def detect_default_paths() -> dict[str, str]:
    """Auto-detect game, runtime, shader, and run paths based on host environment."""
    runtime = ""
    for candidate in [
        os.environ.get("GTA6RT", ""),
        "E:\\Leonida\\gta6rt.exe",
        os.path.join(REPO_ROOT, "runtime", "build_msvc", "gta6rt.exe"),
        os.path.join(REPO_ROOT, "runtime", "build", "RelWithDebInfo", "gta6rt.exe"),
    ]:
        if candidate:
            try:
                if os.path.isfile(candidate):
                    runtime = candidate
                    break
            except OSError:
                runtime = candidate
                break

    runtime_dir = os.path.dirname(runtime) if runtime else ""

    side_game = _read_side_file(os.path.join(runtime_dir, "game_path.txt")) or _read_side_file("E:\\Leonida\\game_path.txt")
    game_dir = ""
    for candidate in [
        os.environ.get("LEONIDA_GAME_DIR", ""),
        side_game,
        r"E:\GTA VI_XBOX",
        r"C:\Users\DELL\Downloads\GTA6.Xbox.v1.0.2.1-CYBERLEEK",
    ]:
        if candidate:
            try:
                if os.path.isdir(candidate):
                    game_dir = candidate
                    break
            except OSError:
                game_dir = candidate
                break

    if not game_dir and side_game:
        game_dir = side_game

    exe_path = ""
    if game_dir:
        exe_path = os.path.join(game_dir, "game_scarlett_master.exe")

    side_shaders = _read_side_file(os.path.join(runtime_dir, "shader_path.txt")) or _read_side_file("E:\\Leonida\\shader_path.txt")
    shaders_dir = ""
    for candidate in [
        os.environ.get("LEONIDA_SHADERS", ""),
        side_shaders,
        r"E:\Leonida\work\shaders\spirv",
    ]:
        if candidate:
            try:
                if os.path.isdir(candidate):
                    shaders_dir = candidate
                    break
            except OSError:
                shaders_dir = candidate
                break
    if not shaders_dir and side_shaders:
        shaders_dir = side_shaders

    runs_dir = ""
    for candidate in [
        os.environ.get("LEONIDA_RUNS", ""),
        r"E:\Leonida\runs",
        os.path.join(os.path.expanduser("~"), "leonida-runs"),
    ]:
        if candidate:
            try:
                if not os.path.isdir(candidate):
                    os.makedirs(candidate, exist_ok=True)
                runs_dir = candidate
                break
            except OSError:
                runs_dir = candidate
                break

    return {
        "runtime_path": runtime or r"E:\Leonida\gta6rt.exe",
        "game_dir": game_dir or r"C:\Users\DELL\Downloads\GTA6.Xbox.v1.0.2.1-CYBERLEEK",
        "exe_path": exe_path or r"C:\Users\DELL\Downloads\GTA6.Xbox.v1.0.2.1-CYBERLEEK\game_scarlett_master.exe",
        "shaders_dir": shaders_dir or r"E:\Leonida\work\shaders\spirv",
        "runs_dir": runs_dir or r"E:\Leonida\runs",
    }


DEFAULT_PROFILES = {
    "max_performance": {
        "id": "max_performance",
        "name": "Max Performance (720p FSR, 6GB VRAM)",
        "badge": "Recommended",
        "description": "Optimized for 6GB GDDR6 VRAM and 24GB DDR5 host memory. Clamps internal rendering to 720p with FSR, sets Realtime priority, 1ms timer, and GPU-side ConsoleHeap.",
        "env": {
            "LEONIDA_D3D12": "vulkan",
            "LEONIDA_DEVICE_TYPE": "6",
            "LEONIDA_MIN_WIDTH": "1280",
            "LEONIDA_MIN_HEIGHT": "720",
            "LEONIDA_MAX_WIDTH": "1280",
            "LEONIDA_MAX_HEIGHT": "720",
            "LEONIDA_FSR": "1",
            "LEONIDA_MAX_VRAM_MB": "6144",
            "LEONIDA_TITLE_MEMORY_MB": "10240",
            "LEONIDA_MAX_HOST_RAM_MB": "16384",
            "LEONIDA_CONSOLE_HEAP_GPU": "1",
            "LEONIDA_GEOMETRY_ASYNC": "1",
            "LEONIDA_REFRESH_PER_FRAME": "1",
            "LEONIDA_POOL_ZERO_PAGES": "1",
            "LEONIDA_FAST_PIPELINES": "all",
            "LEONIDA_STUB_PIPELINES": "1",
            "LEONIDA_KEYBOARD_PAD": "1",
            "LEONIDA_PRIORITY": "realtime",
            "LEONIDA_CAPTURE": "0",
            "LEONIDA_PEEK_SECONDS": "5",
        },
    },
    "balanced": {
        "id": "balanced",
        "name": "Balanced (900p - 1080p FSR, 8GB VRAM)",
        "badge": "Stable",
        "description": "Target 1080p output with dynamic FSR reconstruction. Uses 8GB VRAM budget and High process priority for balanced visual fidelity and smooth frame pacing.",
        "env": {
            "LEONIDA_D3D12": "vulkan",
            "LEONIDA_DEVICE_TYPE": "6",
            "LEONIDA_MIN_WIDTH": "1280",
            "LEONIDA_MIN_HEIGHT": "720",
            "LEONIDA_MAX_WIDTH": "1920",
            "LEONIDA_MAX_HEIGHT": "1080",
            "LEONIDA_FSR": "1",
            "LEONIDA_MAX_VRAM_MB": "8192",
            "LEONIDA_TITLE_MEMORY_MB": "10240",
            "LEONIDA_MAX_HOST_RAM_MB": "16384",
            "LEONIDA_CONSOLE_HEAP_GPU": "1",
            "LEONIDA_GEOMETRY_ASYNC": "1",
            "LEONIDA_REFRESH_PER_FRAME": "1",
            "LEONIDA_POOL_ZERO_PAGES": "1",
            "LEONIDA_FAST_PIPELINES": "all",
            "LEONIDA_STUB_PIPELINES": "1",
            "LEONIDA_KEYBOARD_PAD": "1",
            "LEONIDA_PRIORITY": "high",
            "LEONIDA_CAPTURE": "0",
            "LEONIDA_PEEK_SECONDS": "5",
        },
    },
    "quality": {
        "id": "quality",
        "name": "Visual Quality (1080p Native)",
        "badge": "Fidelity",
        "description": "Native 1080p rendering (1920x1080) without upscaling. Maximum image clarity with 8GB VRAM budget.",
        "env": {
            "LEONIDA_D3D12": "vulkan",
            "LEONIDA_DEVICE_TYPE": "6",
            "LEONIDA_MIN_WIDTH": "1920",
            "LEONIDA_MIN_HEIGHT": "1080",
            "LEONIDA_MAX_WIDTH": "1920",
            "LEONIDA_MAX_HEIGHT": "1080",
            "LEONIDA_FSR": "0",
            "LEONIDA_MAX_VRAM_MB": "8192",
            "LEONIDA_TITLE_MEMORY_MB": "10240",
            "LEONIDA_MAX_HOST_RAM_MB": "16384",
            "LEONIDA_CONSOLE_HEAP_GPU": "1",
            "LEONIDA_GEOMETRY_ASYNC": "1",
            "LEONIDA_REFRESH_PER_FRAME": "1",
            "LEONIDA_POOL_ZERO_PAGES": "1",
            "LEONIDA_FAST_PIPELINES": "all",
            "LEONIDA_STUB_PIPELINES": "1",
            "LEONIDA_KEYBOARD_PAD": "1",
            "LEONIDA_PRIORITY": "high",
            "LEONIDA_CAPTURE": "0",
            "LEONIDA_PEEK_SECONDS": "5",
        },
    },
    "diagnostic": {
        "id": "diagnostic",
        "name": "Diagnostic & Deep Trace",
        "badge": "Debugging",
        "description": "Full diagnostic mode. Enables JSONL startup trace, [d3d12-trace] verbose logging, periodic thread stack walks, and automated swapchain frame captures every 60 frames.",
        "env": {
            "LEONIDA_D3D12": "vulkan",
            "LEONIDA_DEVICE_TYPE": "6",
            "LEONIDA_MIN_WIDTH": "1280",
            "LEONIDA_MIN_HEIGHT": "720",
            "LEONIDA_MAX_WIDTH": "1920",
            "LEONIDA_MAX_HEIGHT": "1080",
            "LEONIDA_FSR": "1",
            "LEONIDA_MAX_VRAM_MB": "8192",
            "LEONIDA_TITLE_MEMORY_MB": "10240",
            "LEONIDA_MAX_HOST_RAM_MB": "16384",
            "LEONIDA_CONSOLE_HEAP_GPU": "1",
            "LEONIDA_GEOMETRY_ASYNC": "1",
            "LEONIDA_REFRESH_PER_FRAME": "1",
            "LEONIDA_CAPTURE": "1",
            "LEONIDA_CAPTURE_EVERY": "60",
            "LEONIDA_CAPTURE_LIMIT": "40",
            "LEONIDA_STACKS": "30,60",
            "LEONIDA_DEVICE_FAULT": "1",
            "LEONIDA_KEYBOARD_PAD": "1",
            "LEONIDA_PRIORITY": "high",
            "LEONIDA_PEEK_SECONDS": "5",
        },
    },
    "custom": {
        "id": "custom",
        "name": "Custom Profile",
        "badge": "Manual",
        "description": "User-defined custom configuration.",
        "env": {},
    },
}


class ConfigManager:
    """Manages launcher configuration and user presets."""

    def __init__(self, config_path: str = CONFIG_PATH) -> None:
        self.config_path = config_path
        self.data: dict = self._load()

    def _load(self) -> dict:
        defaults = {
            "paths": detect_default_paths(),
            "active_profile": "max_performance",
            "profiles": DEFAULT_PROFILES,
            "settings": {
                "auto_precompile_check": True,
                "input_script": "a@15,a@25,a@35,a@45,a@60,a@75,a@90",
                "enable_input_script": False,
                "high_precision_timer": True,
                "xapu_audio_fix": True,
                "gamepad_auto_map": True,
                "seconds_limit": 300,
            },
        }

        if os.path.isfile(self.config_path):
            try:
                with open(self.config_path, "r", encoding="utf-8") as f:
                    saved = json.load(f)
                    # Merge deep paths and settings
                    if "paths" in saved:
                        defaults["paths"].update(saved["paths"])
                    if "active_profile" in saved:
                        defaults["active_profile"] = saved["active_profile"]
                    if "profiles" in saved:
                        for k, v in saved["profiles"].items():
                            if k in defaults["profiles"]:
                                defaults["profiles"][k].update(v)
                            else:
                                defaults["profiles"][k] = v
                    if "settings" in saved:
                        defaults["settings"].update(saved["settings"])
                    return defaults
            except Exception:
                pass
        return defaults

    def save(self) -> None:
        """Atomically persist current configuration to JSON file."""
        tmp_path = self.config_path + ".tmp"
        with open(tmp_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(self.data, f, indent=2)
        if os.path.exists(self.config_path):
            os.replace(tmp_path, self.config_path)
        else:
            os.rename(tmp_path, self.config_path)

    def get_config(self) -> dict:
        return self.data

    def update_config(self, patch: dict) -> dict:
        if "paths" in patch and isinstance(patch["paths"], dict):
            self.data["paths"].update(patch["paths"])
        if "active_profile" in patch and isinstance(patch["active_profile"], str):
            self.data["active_profile"] = patch["active_profile"]
        if "profiles" in patch and isinstance(patch["profiles"], dict):
            for k, v in patch["profiles"].items():
                if k in self.data["profiles"]:
                    self.data["profiles"][k].update(v)
                else:
                    self.data["profiles"][k] = v
        if "settings" in patch and isinstance(patch["settings"], dict):
            self.data["settings"].update(patch["settings"])
        self.save()
        return self.data

    def get_effective_env(self, profile_name: str | None = None) -> dict[str, str]:
        """Returns the dictionary of environment variables for the selected profile."""
        p_name = profile_name or self.data.get("active_profile", "max_performance")
        profile = self.data.get("profiles", {}).get(p_name, DEFAULT_PROFILES["max_performance"])
        env = dict(profile.get("env", {}))

        # Apply settings
        settings = self.data.get("settings", {})
        if settings.get("enable_input_script") and settings.get("input_script"):
            env["LEONIDA_INPUT_SCRIPT"] = settings["input_script"]
            env["LEONIDA_KEYBOARD_PAD"] = "always"

        return env
