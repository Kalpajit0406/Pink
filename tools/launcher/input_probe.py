"""input_probe.py -- XInput & Gamepad probe using Windows ctypes.

Standard library only. Tests and reads controller state without requiring
pygame, pygame-ce, or third-party packages.
"""
from __future__ import annotations

import ctypes
from ctypes import wintypes
from typing import Any

# XInput button masks
XINPUT_GAMEPAD_DPAD_UP = 0x0001
XINPUT_GAMEPAD_DPAD_DOWN = 0x0002
XINPUT_GAMEPAD_DPAD_LEFT = 0x0004
XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008
XINPUT_GAMEPAD_START = 0x0010
XINPUT_GAMEPAD_BACK = 0x0020
XINPUT_GAMEPAD_LEFT_THUMB = 0x0040
XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080
XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100
XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200
XINPUT_GAMEPAD_A = 0x1000
XINPUT_GAMEPAD_B = 0x2000
XINPUT_GAMEPAD_X = 0x4000
XINPUT_GAMEPAD_Y = 0x8000

ERROR_SUCCESS = 0
ERROR_DEVICE_NOT_CONNECTED = 1167


class XINPUT_GAMEPAD(ctypes.Structure):
    _fields_ = [
        ("wButtons", wintypes.WORD),
        ("bLeftTrigger", wintypes.BYTE),
        ("bRightTrigger", wintypes.BYTE),
        ("sThumbLX", wintypes.SHORT),
        ("sThumbLY", wintypes.SHORT),
        ("sThumbRX", wintypes.SHORT),
        ("sThumbRY", wintypes.SHORT),
    ]


class XINPUT_STATE(ctypes.Structure):
    _fields_ = [
        ("dwPacketNumber", wintypes.DWORD),
        ("Gamepad", XINPUT_GAMEPAD),
    ]


def _load_xinput() -> Any:
    for dll_name in ("xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"):
        try:
            dll = ctypes.windll.LoadLibrary(dll_name)
            if hasattr(dll, "XInputGetState"):
                return dll
        except OSError:
            continue
    return None


_XINPUT_DLL = _load_xinput()


def probe_gamepad(user_index: int = 0) -> dict[str, Any]:
    """Query state of connected XInput/Bluetooth gamepad in slot user_index."""
    if not _XINPUT_DLL:
        return {
            "supported": False,
            "connected": False,
            "error": "XInput DLL not found on this system",
        }

    state = XINPUT_STATE()
    res = _XINPUT_DLL.XInputGetState(user_index, ctypes.byref(state))

    if res == ERROR_DEVICE_NOT_CONNECTED:
        return {
            "supported": True,
            "connected": False,
            "index": user_index,
        }
    elif res != ERROR_SUCCESS:
        return {
            "supported": True,
            "connected": False,
            "index": user_index,
            "error_code": res,
        }

    gp = state.Gamepad
    buttons = gp.wButtons

    return {
        "supported": True,
        "connected": True,
        "index": user_index,
        "packet": state.dwPacketNumber,
        "buttons": {
            "A": bool(buttons & XINPUT_GAMEPAD_A),
            "B": bool(buttons & XINPUT_GAMEPAD_B),
            "X": bool(buttons & XINPUT_GAMEPAD_X),
            "Y": bool(buttons & XINPUT_GAMEPAD_Y),
            "DPad_Up": bool(buttons & XINPUT_GAMEPAD_DPAD_UP),
            "DPad_Down": bool(buttons & XINPUT_GAMEPAD_DPAD_DOWN),
            "DPad_Left": bool(buttons & XINPUT_GAMEPAD_DPAD_LEFT),
            "DPad_Right": bool(buttons & XINPUT_GAMEPAD_DPAD_RIGHT),
            "LB": bool(buttons & XINPUT_GAMEPAD_LEFT_SHOULDER),
            "RB": bool(buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER),
            "Start": bool(buttons & XINPUT_GAMEPAD_START),
            "Back": bool(buttons & XINPUT_GAMEPAD_BACK),
            "LeftThumb": bool(buttons & XINPUT_GAMEPAD_LEFT_THUMB),
            "RightThumb": bool(buttons & XINPUT_GAMEPAD_RIGHT_THUMB),
        },
        "triggers": {
            "left": round(gp.bLeftTrigger / 255.0, 3),
            "right": round(gp.bRightTrigger / 255.0, 3),
        },
        "thumbsticks": {
            "lx": round(gp.sThumbLX / 32768.0, 3),
            "ly": round(gp.sThumbLY / 32768.0, 3),
            "rx": round(gp.sThumbRX / 32768.0, 3),
            "ry": round(gp.sThumbRY / 32768.0, 3),
        },
    }


def find_first_connected_gamepad() -> dict[str, Any]:
    """Scan slots 0..3 and return the first connected controller."""
    for i in range(4):
        st = probe_gamepad(i)
        if st.get("connected"):
            return st
    return {"supported": bool(_XINPUT_DLL), "connected": False}
