from __future__ import annotations

import base64
import ctypes
import ipaddress
import json
import os
import queue
import secrets
import socket
import sys
import threading
import time
import tkinter as tk
import traceback
import winreg
from ctypes import wintypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from socketserver import TCPServer
from tkinter import messagebox, ttk
from urllib.parse import parse_qs, urlparse

import comtypes
import qrcode
import pystray
import win32clipboard
from comtypes.client import CreateObject, GetModule
from PIL import Image, ImageTk

if not getattr(sys, "frozen", False):
    GetModule("UIAutomationCore.dll")

from comtypes.gen.UIAutomationClient import (  # noqa: E402
    CUIAutomation,
    IUIAutomation,
    IUIAutomationValuePattern,
    UIA_DocumentControlTypeId,
    UIA_EditControlTypeId,
    UIA_TextPatternId,
    UIA_ValuePatternId,
)


APP_NAME = "声桥"
DEFAULT_PORT = 8765
MAX_TEXT_BYTES = 100_000
CONFIG_FILE = Path(os.environ.get("LOCALAPPDATA", Path.home())) / "VoiceBridge" / "config.json"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_VALUE = "VoiceBridge"
INSTANCE_MUTEX_NAME = r"Local\VoiceBridge.SingleInstance"
ACTIVATION_EVENT_NAME = r"Local\VoiceBridge.ActivateExisting"
ACTIVATION_ACK_EVENT_NAME = r"Local\VoiceBridge.ActivateExisting.Acknowledged"
CONFIG_LOCK = threading.RLock()
CRASH_LOG_FILE = CONFIG_FILE.with_name("crash.log")
DEFAULT_SETTINGS = {
    "close_action": "ask",
    "send_mode": "ctrl_enter",
    "show_char_count": True,
    "theme": "light",
    "character_count": 0,
    "confirm_enter": True,
    "confirm_send": False,
    "show_desktop_to_phone": True,
}
THEMES = {
    "light": {
        "root": "#F3F6FB", "title": "#F3F6FB", "card": "#FFFFFF", "ink": "#132238",
        "muted": "#64748B", "input": "#FFFFFF", "line": "#DBE4F0",
        "secondary": "#EDF2F8", "secondary_hover": "#E5ECF5",
        "secondary_pressed": "#DCE5F0",
        "primary": "#2563EB", "primary_hover": "#2F6CF0", "primary_pressed": "#1D4ED8",
        "teal": "#0F766E", "teal_hover": "#12847B", "teal_pressed": "#0F5F59",
        "danger": "#FFF1F2", "danger_hover": "#FFE8EB", "danger_pressed": "#FFDADF",
        "danger_ink": "#BE123C",
        "status": "#E8F7F0", "status_ink": "#176B4D",
    },
    "dark": {
        "root": "#0F172A", "title": "#0A0A0A", "card": "#182235", "ink": "#E5EDF8",
        "muted": "#9CAFC5", "input": "#111827", "line": "#334155",
        "secondary": "#263449", "secondary_hover": "#304158",
        "secondary_pressed": "#3A4D67",
        "primary": "#2563EB", "primary_hover": "#3473F2", "primary_pressed": "#1D4ED8",
        "teal": "#0F766E", "teal_hover": "#14877D", "teal_pressed": "#0F5F59",
        "danger": "#3B202B", "danger_hover": "#4A2633", "danger_pressed": "#592B3B",
        "danger_ink": "#FDA4AF",
        "status": "#15352F", "status_ink": "#6EE7B7",
    },
}


def resource_path(relative_path: str) -> Path:
    base = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
    return base / relative_path


WEB_PAGE = resource_path("mobile.html")
ICON_PNG = resource_path("assets/voice-bridge.png")
ICON_ICO = resource_path("assets/voice-bridge.ico")


def startup_command(
    frozen: bool | None = None,
    executable: Path | None = None,
    script: Path | None = None,
) -> str:
    frozen = getattr(sys, "frozen", False) if frozen is None else frozen
    executable = Path(sys.executable) if executable is None else executable
    script = Path(__file__).resolve() if script is None else script
    if frozen:
        return f'"{executable}"'
    pythonw = executable.with_name("pythonw.exe")
    return f'"{pythonw}" "{script}"'


def autostart_enabled() -> bool:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            value, _kind = winreg.QueryValueEx(key, RUN_VALUE)
        return value == startup_command()
    except FileNotFoundError:
        return False


def set_autostart(enabled: bool) -> None:
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
        if enabled:
            winreg.SetValueEx(key, RUN_VALUE, 0, winreg.REG_SZ, startup_command())
        else:
            try:
                winreg.DeleteValue(key, RUN_VALUE)
            except FileNotFoundError:
                pass


def _read_config(config_file: Path) -> dict:
    try:
        data = json.loads(config_file.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, TypeError, json.JSONDecodeError):
        return {}


def _write_config(config_file: Path, data: dict) -> None:
    config_file.parent.mkdir(parents=True, exist_ok=True)
    temporary = config_file.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    temporary.replace(config_file)


def load_settings(config_file: Path = CONFIG_FILE) -> dict:
    data = _read_config(config_file)
    settings = DEFAULT_SETTINGS.copy()
    if data.get("close_action") in {"ask", "tray", "exit"}:
        settings["close_action"] = data["close_action"]
    if data.get("send_mode") in {"ctrl_enter", "enter"}:
        settings["send_mode"] = data["send_mode"]
    if isinstance(data.get("show_char_count"), bool):
        settings["show_char_count"] = data["show_char_count"]
    if data.get("theme") in THEMES:
        settings["theme"] = data["theme"]
    for key in ("confirm_enter", "confirm_send"):
        if isinstance(data.get(key), bool):
            settings[key] = data[key]
    if isinstance(data.get("show_desktop_to_phone"), bool):
        settings["show_desktop_to_phone"] = data["show_desktop_to_phone"]
    count = data.get("character_count")
    if isinstance(count, int) and not isinstance(count, bool) and count >= 0:
        settings["character_count"] = count
    return settings


def update_config(values: dict, config_file: Path = CONFIG_FILE) -> dict:
    with CONFIG_LOCK:
        data = _read_config(config_file)
        data.update(values)
        _write_config(config_file, data)
        return data


def add_character_count(amount: int, config_file: Path = CONFIG_FILE) -> int:
    with CONFIG_LOCK:
        data = _read_config(config_file)
        count = data.get("character_count", 0)
        if not isinstance(count, int) or isinstance(count, bool) or count < 0:
            count = 0
        count += amount
        data["character_count"] = count
        _write_config(config_file, data)
        return count


def load_or_create_token(config_file: Path = CONFIG_FILE, rotate: bool = False) -> str:
    with CONFIG_LOCK:
        data = _read_config(config_file)
        token = data.get("token")
        if not rotate and isinstance(token, str) and len(token) >= 32:
            return token
        token = secrets.token_urlsafe(32)
        data["token"] = token
        _write_config(config_file, data)
    return token


def enable_dpi_awareness() -> None:
    try:
        if ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
            return
    except (AttributeError, OSError):
        pass
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except (AttributeError, OSError):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            pass


def windows_colorref(color: str) -> int:
    red = int(color[1:3], 16)
    green = int(color[3:5], 16)
    blue = int(color[5:7], 16)
    return red | (green << 8) | (blue << 16)


def acquire_single_instance() -> tuple[int, int, int] | None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateMutexW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR)
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    kernel32.CreateEventW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR)
    kernel32.CreateEventW.restype = wintypes.HANDLE
    kernel32.OpenEventW.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR)
    kernel32.OpenEventW.restype = wintypes.HANDLE
    kernel32.SetEvent.argtypes = (wintypes.HANDLE,)
    kernel32.ResetEvent.argtypes = (wintypes.HANDLE,)
    kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)

    mutex = kernel32.CreateMutexW(None, False, INSTANCE_MUTEX_NAME)
    if not mutex:
        raise ctypes.WinError(ctypes.get_last_error())
    if ctypes.get_last_error() == 183:
        event = kernel32.OpenEventW(0x0002, False, ACTIVATION_EVENT_NAME)
        acknowledged = False
        if event:
            acknowledgement = kernel32.OpenEventW(
                0x00100002, False, ACTIVATION_ACK_EVENT_NAME
            )
            if acknowledgement:
                kernel32.ResetEvent(acknowledgement)
            kernel32.SetEvent(event)
            if acknowledgement:
                acknowledged = kernel32.WaitForSingleObject(
                    acknowledgement, 1500
                ) == 0
                kernel32.CloseHandle(acknowledgement)
            kernel32.CloseHandle(event)
        kernel32.CloseHandle(mutex)
        if not acknowledged:
            ctypes.windll.user32.MessageBoxW(
                None,
                "声桥已经在后台运行，可能是另一个版本。\n"
                "请从系统托盘打开它；若要切换版本，请先在托盘中彻底退出旧实例。",
                APP_NAME,
                0x40,
            )
        return None

    event = kernel32.CreateEventW(None, True, False, ACTIVATION_EVENT_NAME)
    if not event:
        kernel32.CloseHandle(mutex)
        raise ctypes.WinError(ctypes.get_last_error())
    acknowledgement = kernel32.CreateEventW(
        None, True, False, ACTIVATION_ACK_EVENT_NAME
    )
    if not acknowledgement:
        kernel32.CloseHandle(event)
        kernel32.CloseHandle(mutex)
        raise ctypes.WinError(ctypes.get_last_error())
    return mutex, event, acknowledgement


def release_single_instance(handles: tuple[int, ...]) -> None:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    for handle in reversed(handles):
        kernel32.CloseHandle(handle)


def report_fatal_error(
    error: Exception, log_file: Path = CRASH_LOG_FILE
) -> None:
    try:
        log_file.parent.mkdir(parents=True, exist_ok=True)
        with log_file.open("a", encoding="utf-8") as stream:
            stream.write(
                f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] "
                f"{type(error).__name__}: {error}\n{traceback.format_exc()}\n"
            )
    except OSError:
        pass
    try:
        ctypes.windll.user32.MessageBoxW(
            None,
            f"声桥发生错误，无法继续运行：\n{error}\n\n"
            f"诊断记录：{log_file}",
            "声桥启动失败",
            0x10,
        )
    except (AttributeError, OSError):
        pass


class NoTextInputFocusError(OSError):
    pass


class WindowsUnicodeInput:
    """Insert Unicode text at the active cursor without replacing the control value."""

    INPUT_KEYBOARD = 1
    VK_CONTROL = 0x11
    VK_SHIFT = 0x10
    VK_RETURN = 0x0D
    VK_V = 0x56
    VK_BACK = 0x08
    VK_HOME = 0x24
    KEYEVENTF_EXTENDEDKEY = 0x0001
    KEYEVENTF_UNICODE = 0x0004
    KEYEVENTF_KEYUP = 0x0002
    EM_REPLACESEL = 0x00C2
    SMTO_ABORTIFHUNG = 0x0002
    CLEANUP_MARKER = ord("x")
    MAX_EVENTS_PER_BATCH = 96
    BATCH_DELAY_SECONDS = 0.01

    class GuiThreadInfo(ctypes.Structure):
        _fields_ = (
            ("size", wintypes.DWORD),
            ("flags", wintypes.DWORD),
            ("active", wintypes.HWND),
            ("focus", wintypes.HWND),
            ("capture", wintypes.HWND),
            ("menu_owner", wintypes.HWND),
            ("move_size", wintypes.HWND),
            ("caret", wintypes.HWND),
            ("caret_rect", wintypes.RECT),
        )

    class KeyboardInput(ctypes.Structure):
        _fields_ = (
            ("virtual_key", wintypes.WORD),
            ("scan_code", wintypes.WORD),
            ("flags", wintypes.DWORD),
            ("timestamp", wintypes.DWORD),
            ("extra_info", ctypes.c_size_t),
        )

    class MouseInput(ctypes.Structure):
        _fields_ = (
            ("x", wintypes.LONG),
            ("y", wintypes.LONG),
            ("mouse_data", wintypes.DWORD),
            ("flags", wintypes.DWORD),
            ("timestamp", wintypes.DWORD),
            ("extra_info", ctypes.c_size_t),
        )

    class HardwareInput(ctypes.Structure):
        _fields_ = (
            ("message", wintypes.DWORD),
            ("parameter_low", wintypes.WORD),
            ("parameter_high", wintypes.WORD),
        )

    class InputUnion(ctypes.Union):
        pass

    def __init__(self) -> None:
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.user32.SendInput.argtypes = (
            wintypes.UINT,
            ctypes.POINTER(self.Input),
            ctypes.c_int,
        )
        self.user32.SendInput.restype = wintypes.UINT
        self.user32.GetForegroundWindow.restype = wintypes.HWND
        self.user32.GetWindowThreadProcessId.argtypes = (
            wintypes.HWND,
            ctypes.POINTER(wintypes.DWORD),
        )
        self.user32.GetWindowThreadProcessId.restype = wintypes.DWORD
        self.user32.GetGUIThreadInfo.argtypes = (
            wintypes.DWORD,
            ctypes.POINTER(self.GuiThreadInfo),
        )
        self.user32.GetGUIThreadInfo.restype = wintypes.BOOL
        self.user32.GetClassNameW.argtypes = (
            wintypes.HWND,
            wintypes.LPWSTR,
            ctypes.c_int,
        )
        self.user32.SendMessageTimeoutW.argtypes = (
            wintypes.HWND,
            wintypes.UINT,
            wintypes.WPARAM,
            wintypes.LPARAM,
            wintypes.UINT,
            wintypes.UINT,
            ctypes.POINTER(ctypes.c_size_t),
        )
        self.user32.SendMessageTimeoutW.restype = ctypes.c_size_t

    def __call__(self, text: str) -> None:
        self._paste_with_clipboard(text)

    def press_enter(self) -> None:
        self._require_text_input_target()
        self._send_inputs(
            self._inputs_from_events(
                (
                    (self.VK_RETURN, 0, 0),
                    (self.VK_RETURN, 0, self.KEYEVENTF_KEYUP),
                )
            )
        )

    def _paste_with_clipboard(self, text: str) -> None:
        owner = 0
        try:
            target = self._require_text_input_target()

            create_window = self.user32.CreateWindowExW
            create_window.argtypes = (
                wintypes.DWORD,
                wintypes.LPCWSTR,
                wintypes.LPCWSTR,
                wintypes.DWORD,
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_int,
                wintypes.HWND,
                wintypes.HMENU,
                wintypes.HINSTANCE,
                ctypes.c_void_p,
            )
            create_window.restype = wintypes.HWND
            owner = create_window(
                0, "STATIC", "", 0, 0, 0, 0, 0, None, None, None, None
            )
            if not owner:
                raise ctypes.WinError(ctypes.get_last_error())

            self._set_clipboard_text(owner, text)
            if self._require_text_input_target() != target:
                raise NoTextInputFocusError(
                    "电脑端输入光标已变化，请重新点击输入框后再发送"
                )

            self._send_inputs(
                self._inputs_from_events(
                    (
                        (self.VK_CONTROL, 0, 0),
                        (self.VK_V, 0, 0),
                        (self.VK_V, 0, self.KEYEVENTF_KEYUP),
                        (self.VK_CONTROL, 0, self.KEYEVENTF_KEYUP),
                    )
                )
            )
        finally:
            if owner:
                self.user32.DestroyWindow(owner)

    def _text_input_target(self) -> tuple[int, ...] | None:
        foreground = self.user32.GetForegroundWindow()
        if not foreground:
            return None
        process_id = wintypes.DWORD()
        thread_id = self.user32.GetWindowThreadProcessId(
            foreground, ctypes.byref(process_id)
        )
        info = self.GuiThreadInfo(size=ctypes.sizeof(self.GuiThreadInfo))
        if thread_id and self.user32.GetGUIThreadInfo(
            thread_id, ctypes.byref(info)
        ) and info.focus:
            if info.caret:
                return int(foreground), int(info.focus)

            class_name = ctypes.create_unicode_buffer(256)
            if self.user32.GetClassNameW(info.focus, class_name, 256):
                normalized_class = class_name.value.casefold()
                if (
                    normalized_class == "edit"
                    or "richedit" in normalized_class
                    or ".edit." in normalized_class
                ):
                    return int(foreground), int(info.focus)

        runtime_id = self._uia_text_input_runtime_id(process_id.value)
        if runtime_id:
            return int(foreground), int(process_id.value), *runtime_id
        return None

    @staticmethod
    def _uia_text_input_runtime_id(process_id: int) -> tuple[int, ...] | None:
        if not process_id:
            return None
        initialized = False
        try:
            comtypes.CoInitialize()
            initialized = True
            automation = CreateObject(CUIAutomation, interface=IUIAutomation)
            element = automation.GetFocusedElement()
            if (
                not element
                or element.CurrentProcessId != process_id
                or not element.CurrentIsEnabled
                or element.CurrentControlType
                not in {UIA_EditControlTypeId, UIA_DocumentControlTypeId}
            ):
                return None

            value_pattern = element.GetCurrentPattern(UIA_ValuePatternId)
            if value_pattern:
                editable_value = value_pattern.QueryInterface(
                    IUIAutomationValuePattern
                )
                if editable_value.CurrentIsReadOnly:
                    return None
            else:
                aria_role = (element.CurrentAriaRole or "").casefold()
                if aria_role not in {"textbox", "searchbox"} or not element.GetCurrentPattern(
                    UIA_TextPatternId
                ):
                    return None

            runtime_id = element.GetRuntimeId()
            if not runtime_id:
                return None
            return tuple(int(value) for value in runtime_id)
        except Exception:
            return None
        finally:
            if initialized:
                comtypes.CoUninitialize()

    def _require_text_input_target(self) -> tuple[int, ...]:
        target = self._text_input_target()
        if target is None:
            raise NoTextInputFocusError(
                "电脑端未检测到输入光标，请先在电脑输入框中点击一下"
            )
        return target

    def _set_clipboard_text(self, owner: int, text: str) -> None:
        self._open_clipboard(owner)
        try:
            win32clipboard.EmptyClipboard()
            win32clipboard.SetClipboardText(text, win32clipboard.CF_UNICODETEXT)
        finally:
            win32clipboard.CloseClipboard()

    @staticmethod
    def _open_clipboard(owner: int) -> None:
        opened = False
        for attempt in range(20):
            try:
                win32clipboard.OpenClipboard(owner)
                opened = True
                break
            except Exception:
                if attempt == 19:
                    raise
                time.sleep(0.01)
        if not opened:
            raise OSError("无法打开剪贴板")

    def _send_inputs(self, inputs) -> None:
        sent = self.user32.SendInput(len(inputs), inputs, ctypes.sizeof(self.Input))
        if sent != len(inputs):
            error = ctypes.get_last_error()
            if error:
                raise ctypes.WinError(error)
            raise OSError("Windows 阻止了向当前光标输入文字")

    def _insert_into_native_edit(self, text: str) -> bool:
        foreground = self.user32.GetForegroundWindow()
        if not foreground:
            return False
        thread_id = self.user32.GetWindowThreadProcessId(foreground, None)
        info = self.GuiThreadInfo(size=ctypes.sizeof(self.GuiThreadInfo))
        if not thread_id or not self.user32.GetGUIThreadInfo(thread_id, ctypes.byref(info)):
            return False
        class_name = ctypes.create_unicode_buffer(256)
        if not info.focus or not self.user32.GetClassNameW(info.focus, class_name, 256):
            return False
        normalized_class = class_name.value.casefold()
        if not (
            normalized_class == "edit"
            or "richedit" in normalized_class
            or ".edit." in normalized_class
        ):
            return False
        if "\0" in text:
            return False

        native_text = text.replace("\r\n", "\n").replace("\r", "\n").replace("\n", "\r\n")
        buffer = ctypes.create_unicode_buffer(native_text)
        result = ctypes.c_size_t()
        return bool(
            self.user32.SendMessageTimeoutW(
                info.focus,
                self.EM_REPLACESEL,
                True,
                ctypes.cast(buffer, ctypes.c_void_p).value,
                self.SMTO_ABORTIFHUNG,
                1000,
                ctypes.byref(result),
            )
        )

    @classmethod
    def _event_groups(cls, text: str):
        groups = []
        normalized = text.replace("\r\n", "\n").replace("\r", "\n")
        for character in normalized:
            if character == "\n":
                groups.append(
                    [
                        (cls.VK_SHIFT, 0, 0),
                        (cls.VK_RETURN, 0, 0),
                        (cls.VK_RETURN, 0, cls.KEYEVENTF_KEYUP),
                        (cls.VK_SHIFT, 0, cls.KEYEVENTF_KEYUP),
                        (0, cls.CLEANUP_MARKER, cls.KEYEVENTF_UNICODE),
                        (
                            0,
                            cls.CLEANUP_MARKER,
                            cls.KEYEVENTF_UNICODE | cls.KEYEVENTF_KEYUP,
                        ),
                        (cls.VK_SHIFT, 0, 0),
                        (cls.VK_HOME, 0, cls.KEYEVENTF_EXTENDEDKEY),
                        (
                            cls.VK_HOME,
                            0,
                            cls.KEYEVENTF_EXTENDEDKEY | cls.KEYEVENTF_KEYUP,
                        ),
                        (cls.VK_HOME, 0, cls.KEYEVENTF_EXTENDEDKEY),
                        (
                            cls.VK_HOME,
                            0,
                            cls.KEYEVENTF_EXTENDEDKEY | cls.KEYEVENTF_KEYUP,
                        ),
                        (cls.VK_SHIFT, 0, cls.KEYEVENTF_KEYUP),
                        (cls.VK_BACK, 0, 0),
                        (cls.VK_BACK, 0, cls.KEYEVENTF_KEYUP),
                    ]
                )
                continue
            encoded = character.encode("utf-16-le", "surrogatepass")
            events = []
            for index in range(0, len(encoded), 2):
                code_unit = int.from_bytes(encoded[index:index + 2], "little")
                events.extend(
                    (
                        (0, code_unit, cls.KEYEVENTF_UNICODE),
                        (
                            0,
                            code_unit,
                            cls.KEYEVENTF_UNICODE | cls.KEYEVENTF_KEYUP,
                        ),
                    )
                )
            groups.append(events)
        return groups

    @classmethod
    def _inputs_from_events(cls, events):
        inputs = (cls.Input * len(events))()
        for index, (virtual_key, scan_code, flags) in enumerate(events):
            inputs[index].type = cls.INPUT_KEYBOARD
            inputs[index].keyboard.virtual_key = virtual_key
            inputs[index].keyboard.scan_code = scan_code
            inputs[index].keyboard.flags = flags
        return inputs

    @classmethod
    def build_inputs(cls, text: str):
        events = [event for group in cls._event_groups(text) for event in group]
        return cls._inputs_from_events(events)

    @classmethod
    def build_input_batches(cls, text: str):
        batches = []
        events = []
        for group in cls._event_groups(text):
            if events and len(events) + len(group) > cls.MAX_EVENTS_PER_BATCH:
                batches.append(cls._inputs_from_events(events))
                events = []
            events.extend(group)
        if events:
            batches.append(cls._inputs_from_events(events))
        return batches


WindowsUnicodeInput.InputUnion._fields_ = (
    ("mouse", WindowsUnicodeInput.MouseInput),
    ("keyboard", WindowsUnicodeInput.KeyboardInput),
    ("hardware", WindowsUnicodeInput.HardwareInput),
)


class WindowsInput(ctypes.Structure):
    _anonymous_ = ("data",)
    _fields_ = (
        ("type", wintypes.DWORD),
        ("data", WindowsUnicodeInput.InputUnion),
    )


WindowsUnicodeInput.Input = WindowsInput


class BridgeServer(ThreadingHTTPServer):
    daemon_threads = True

    def server_bind(self) -> None:
        # HTTPServer normally performs a reverse-DNS lookup here. The bridge
        # only displays a numeric LAN address, so that lookup adds no value and
        # can block startup for several seconds on some routers.
        TCPServer.server_bind(self)
        host, port = self.server_address[:2]
        self.server_name = host
        self.server_port = port

    def __init__(
        self,
        address,
        token: str,
        input_callback,
        status_callback=None,
        enter_callback=None,
        config_file: Path = CONFIG_FILE,
    ):
        self.token = token
        self.input_callback = input_callback
        self.status_callback = status_callback or (lambda _message: None)
        self.enter_callback = enter_callback or (lambda: None)
        self.config_file = config_file
        page = WEB_PAGE.read_text(encoding="utf-8")
        icon = base64.b64encode(ICON_PNG.read_bytes()).decode("ascii")
        self.page = page.replace("{{APP_ICON_BASE64}}", icon).encode("utf-8")
        self.accepting_phone_text = True
        self.session_id = secrets.token_urlsafe(8)
        self._phone_messages: list[dict] = []
        self._next_message_id = 1
        self._phone_condition = threading.Condition()
        super().__init__(address, BridgeHandler)

    def queue_for_phone(self, text: str) -> int:
        with self._phone_condition:
            message_id = self._next_message_id
            self._next_message_id += 1
            self._phone_messages.append({"id": message_id, "text": text})
            self._phone_messages = self._phone_messages[-50:]
            self._phone_condition.notify_all()
            return message_id

    def acknowledge_phone_messages(self, through: int, client_session: str) -> int:
        with self._phone_condition:
            if client_session != self.session_id:
                return 0
            previous_count = len(self._phone_messages)
            self._phone_messages = [
                item for item in self._phone_messages if item["id"] > through
            ]
            return previous_count - len(self._phone_messages)

    def messages_for_phone(
        self, after: int, client_session: str, wait_seconds: float = 20
    ) -> list[dict]:
        if client_session != self.session_id:
            after = 0
        with self._phone_condition:
            messages = [item for item in self._phone_messages if item["id"] > after]
            if not messages and wait_seconds:
                self._phone_condition.wait(wait_seconds)
                messages = [item for item in self._phone_messages if item["id"] > after]
            return messages


class BridgeHandler(BaseHTTPRequestHandler):
    server: BridgeServer
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if not self._authorized(parsed.query):
            self._json_response(403, {"ok": False, "error": "配对链接无效"})
            return

        if parsed.path == "/settings":
            settings = load_settings(self.server.config_file)
            self._json_response(
                200,
                {
                    "ok": True,
                    "confirm_enter": settings["confirm_enter"],
                    "confirm_send": settings["confirm_send"],
                },
            )
            return

        if parsed.path == "/receive":
            query = parse_qs(parsed.query)
            try:
                after = max(0, int(query.get("after", ["0"])[0]))
            except ValueError:
                self._json_response(400, {"ok": False, "error": "消息序号无效"})
                return
            messages = self.server.messages_for_phone(
                after, query.get("session", [""])[0]
            )
            self._json_response(
                200,
                {
                    "ok": True,
                    "session": self.server.session_id,
                    "messages": messages,
                },
            )
            return

        if parsed.path != "/":
            self._json_response(404, {"ok": False, "error": "页面不存在"})
            return

        self.server.status_callback("手机已连接")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(self.server.page)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(self.server.page)

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path not in {
            "/send", "/enter", "/settings", "/ack"
        } or not self._authorized(parsed.query):
            self._json_response(403, {"ok": False, "error": "配对链接无效"})
            return
        if parsed.path == "/ack":
            self._acknowledge_phone_messages()
            return
        if parsed.path == "/settings":
            self._update_mobile_settings()
            return
        if not self.server.accepting_phone_text:
            self._json_response(423, {"ok": False, "error": "电脑已暂停接收"})
            return

        if parsed.path == "/enter":
            try:
                self.server.enter_callback()
            except NoTextInputFocusError as error:
                self.server.status_callback(str(error))
                self._json_response(409, {"ok": False, "error": str(error)})
                return
            except Exception as error:
                self.server.status_callback(f"发送回车失败：{error}")
                self._json_response(500, {"ok": False, "error": "电脑回车失败"})
                return
            self.server.status_callback("已按下回车")
            self._json_response(200, {"ok": True})
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length <= 0 or length > MAX_TEXT_BYTES:
            self._json_response(400, {"ok": False, "error": "内容为空或过长"})
            return

        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            text = payload.get("text")
            if not isinstance(text, str) or not text:
                raise ValueError
            self.server.input_callback(text)
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
            self._json_response(400, {"ok": False, "error": "文字内容无效"})
            return
        except NoTextInputFocusError as error:
            self.server.status_callback(str(error))
            self._json_response(409, {"ok": False, "error": str(error)})
            return
        except Exception as error:
            self.server.status_callback(f"发送失败：{error}")
            self._json_response(500, {"ok": False, "error": "电脑输入失败"})
            return

        self.server.status_callback(f"已输入 {len(text)} 个字符")
        self._json_response(200, {"ok": True})

    def _acknowledge_phone_messages(self) -> None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 1_024:
                raise ValueError
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            client_session = payload.get("session")
            through = payload.get("through")
            if (
                not isinstance(client_session, str)
                or not isinstance(through, int)
                or isinstance(through, bool)
                or through < 0
            ):
                raise ValueError
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError):
            self._json_response(400, {"ok": False, "error": "确认信息无效"})
            return

        acknowledged = self.server.acknowledge_phone_messages(
            through, client_session
        )
        if acknowledged:
            self.server.status_callback(
                f"手机已确认接收 {acknowledged} 条消息"
            )
        self._json_response(
            200,
            {
                "ok": True,
                "session": self.server.session_id,
                "acknowledged": acknowledged,
            },
        )

    def _update_mobile_settings(self) -> None:
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 1_024:
                raise ValueError
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            values = {}
            for key in ("confirm_enter", "confirm_send"):
                if key in payload:
                    if not isinstance(payload[key], bool):
                        raise ValueError
                    values[key] = payload[key]
            if not values:
                raise ValueError
        except (UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError):
            self._json_response(400, {"ok": False, "error": "设置内容无效"})
            return

        update_config(values, self.server.config_file)
        settings = load_settings(self.server.config_file)
        self._json_response(
            200,
            {
                "ok": True,
                "confirm_enter": settings["confirm_enter"],
                "confirm_send": settings["confirm_send"],
            },
        )

    def _authorized(self, query: str) -> bool:
        supplied = parse_qs(query).get("token", [""])[0]
        return secrets.compare_digest(supplied, self.server.token)

    def _json_response(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format: str, *_args) -> None:
        return


def choose_lan_ip(
    addresses: list[str], gateway_addresses: set[str] | None = None
) -> str | None:
    unique = list(dict.fromkeys(addresses))
    if gateway_addresses:
        routed = [address for address in unique if address in gateway_addresses]
        if routed:
            unique = routed
    preferred_networks = (
        ipaddress.ip_network("192.168.0.0/16"),
        ipaddress.ip_network("172.16.0.0/12"),
        ipaddress.ip_network("10.0.0.0/8"),
    )
    for network in preferred_networks:
        for address in unique:
            if ipaddress.ip_address(address) in network:
                return address
    return None


def _registry_strings(key, *names: str) -> list[str]:
    for name in names:
        try:
            value, _kind = winreg.QueryValueEx(key, name)
        except OSError:
            continue
        values = [value] if isinstance(value, str) else value
        if isinstance(values, (list, tuple)):
            cleaned = [item for item in values if isinstance(item, str) and item]
            if cleaned:
                return cleaned
    return []


def gateway_lan_addresses() -> set[str]:
    """Return IPv4 addresses assigned to adapters with a default gateway."""
    path = r"SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces"
    addresses = set()
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, path) as interfaces:
            count = winreg.QueryInfoKey(interfaces)[0]
            for index in range(count):
                try:
                    with winreg.OpenKey(interfaces, winreg.EnumKey(interfaces, index)) as adapter:
                        gateways = _registry_strings(
                            adapter, "DhcpDefaultGateway", "DefaultGateway"
                        )
                        if not any(item != "0.0.0.0" for item in gateways):
                            continue
                        for address in _registry_strings(
                            adapter, "DhcpIPAddress", "IPAddress"
                        ):
                            if address != "0.0.0.0":
                                addresses.add(address)
                except OSError:
                    continue
    except OSError:
        return set()
    return addresses


def local_ip() -> str:
    hostname_addresses = [
        item[4][0]
        for item in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
    ]
    preferred = choose_lan_ip(hostname_addresses, gateway_lan_addresses())
    if preferred:
        return preferred

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("10.255.255.255", 1))
        return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        sock.close()


def active_ipv4_addresses() -> set[str]:
    addresses = {"127.0.0.1"}
    try:
        addresses.update(
            item[4][0]
            for item in socket.getaddrinfo(
                socket.gethostname(), None, socket.AF_INET
            )
        )
    except OSError:
        pass
    addresses.update(gateway_lan_addresses())
    return addresses


def load_saved_lan_ip(config_file: Path = CONFIG_FILE) -> str | None:
    value = _read_config(config_file).get("lan_ip")
    if not isinstance(value, str):
        return None
    try:
        address = ipaddress.ip_address(value)
    except ValueError:
        return None
    if address.version != 4 or address.is_unspecified or address.is_multicast:
        return None
    return str(address)


def startup_lan_binding(
    config_file: Path = CONFIG_FILE,
) -> tuple[str, str, bool]:
    """Return displayed IP, bind IP and whether the saved address is usable."""
    saved_ip = load_saved_lan_ip(config_file)
    if saved_ip is not None:
        available = saved_ip in active_ipv4_addresses()
        return saved_ip, saved_ip if available else "127.0.0.1", available

    selected_ip = local_ip()
    available = selected_ip != "127.0.0.1"
    if available:
        update_config({"lan_ip": selected_ip}, config_file)
    return selected_ip, selected_ip, available


class RoundedCard(tk.Canvas):
    def __init__(
        self,
        master,
        outer_color: str,
        fill_color: str,
        *,
        padding=18,
        radius: int = 16,
        content_style: str = "Card.TFrame",
    ) -> None:
        super().__init__(
            master,
            height=1,
            background=outer_color,
            borderwidth=0,
            highlightthickness=0,
        )
        self.fill_color = fill_color
        self.radius = radius
        self.inset = max(4, radius // 2)
        self.content = ttk.Frame(self, padding=padding, style=content_style)
        self.content_window = self.create_window(
            self.inset,
            self.inset,
            anchor="nw",
            window=self.content,
        )
        self.bind("<Configure>", self._resize)

    def fit_content(self) -> None:
        self.content.update_idletasks()
        self.configure(height=self.content.winfo_reqheight() + self.inset * 2)

    def set_colors(self, outer_color: str, fill_color: str) -> None:
        self.configure(background=outer_color)
        self.fill_color = fill_color
        self._draw_background(self.winfo_width(), self.winfo_height())

    def _resize(self, event) -> None:
        inset = self.inset
        self.coords(self.content_window, inset, inset)
        self.itemconfigure(
            self.content_window,
            width=max(1, event.width - inset * 2),
            height=max(1, event.height - inset * 2),
        )
        self._draw_background(event.width, event.height)

    def _draw_background(self, width: int, height: int) -> None:
        self.delete("rounded_background")
        radius = min(self.radius, width // 2, height // 2)
        if radius <= 0:
            return
        options = {
            "fill": self.fill_color,
            "outline": "",
            "tags": "rounded_background",
        }
        self.create_rectangle(radius, 0, width - radius, height, **options)
        self.create_rectangle(0, radius, width, height - radius, **options)
        diameter = radius * 2
        for x, y, start in (
            (0, 0, 90),
            (width - diameter, 0, 0),
            (width - diameter, height - diameter, 270),
            (0, height - diameter, 180),
        ):
            self.create_arc(
                x,
                y,
                x + diameter,
                y + diameter,
                start=start,
                extent=90,
                style="pieslice",
                **options,
            )
        self.tag_lower("rounded_background")


def blend_color(start: str, end: str, progress: float) -> str:
    first = tuple(int(start[index:index + 2], 16) for index in (1, 3, 5))
    second = tuple(int(end[index:index + 2], 16) for index in (1, 3, 5))
    mixed = tuple(round(a + (b - a) * progress) for a, b in zip(first, second))
    return "#" + "".join(f"{value:02X}" for value in mixed)


class AnimatedButton(tk.Canvas):
    def __init__(
        self,
        master,
        command,
        palette: dict,
        *,
        text: str = "",
        icon: str | None = None,
        width: int = 120,
        height: int = 42,
        radius: int | None = None,
        role: str = "secondary",
        outer_key: str = "card",
        font=("Microsoft YaHei UI", 9, "bold"),
    ) -> None:
        super().__init__(
            master,
            width=width,
            height=height,
            borderwidth=0,
            highlightthickness=0,
            takefocus=1,
            cursor="hand2",
        )
        self.command = command
        self.text = text
        self.icon = icon
        self.button_width = width
        self.button_height = height
        self.radius = radius if radius is not None else min(16, height // 2)
        self.base_role = role
        self.role = role
        self.outer_key = outer_key
        self.font = font
        self.state = "normal"
        self.enabled = True
        self.animation_job = None
        self.current_fill = "#000000"
        self.palette = palette
        self.set_palette(palette, immediate=True)
        self.bind("<Enter>", self._enter)
        self.bind("<Leave>", self._leave)
        self.bind("<ButtonPress-1>", self._press)
        self.bind("<ButtonRelease-1>", self._release)
        self.bind("<space>", self._keyboard_activate)
        self.bind("<Return>", self._keyboard_activate)

    def set_palette(self, palette: dict, immediate: bool = False) -> None:
        self.palette = palette
        self.configure(background=palette[self.outer_key])
        self.normal_fill, self.hover_fill, self.pressed_fill, self.foreground = (
            self._role_colors(self.role)
        )
        target = self._state_fill()
        if immediate:
            self.current_fill = target
            self._draw()
        else:
            self._animate_to(target)

    def set_role(self, role: str) -> None:
        self.role = role
        self.set_palette(self.palette)

    def set_selected(self, selected: bool) -> None:
        self.set_role("primary" if selected else self.base_role)

    def set_text(self, text: str) -> None:
        self.text = text
        self._draw()

    def set_icon(self, icon: str) -> None:
        self.icon = icon
        self._draw()

    def set_enabled(self, enabled: bool) -> None:
        self.enabled = enabled
        self.configure(cursor="hand2" if enabled else "arrow")
        self._draw()

    def _role_colors(self, role: str) -> tuple[str, str, str, str]:
        if role == "primary":
            return (
                self.palette["primary"],
                self.palette["primary_hover"],
                self.palette["primary_pressed"],
                "#FFFFFF",
            )
        if role == "teal":
            return (
                self.palette["teal"],
                self.palette["teal_hover"],
                self.palette["teal_pressed"],
                "#FFFFFF",
            )
        if role == "danger":
            return (
                self.palette["danger"],
                self.palette["danger_hover"],
                self.palette["danger_pressed"],
                self.palette["danger_ink"],
            )
        return (
            self.palette["secondary"],
            self.palette["secondary_hover"],
            self.palette["secondary_pressed"],
            self.palette["ink"],
        )

    def _state_fill(self) -> str:
        if self.state == "pressed":
            return self.pressed_fill
        if self.state == "hover":
            return self.hover_fill
        return self.normal_fill

    def _enter(self, _event=None) -> None:
        if self.enabled and self.state != "pressed":
            self.state = "hover"
            self._animate_to(self.hover_fill)

    def _leave(self, _event=None) -> None:
        if self.enabled:
            self.state = "normal"
            self._animate_to(self.normal_fill)

    def _press(self, _event=None) -> None:
        if self.enabled:
            self.focus_set()
            self.state = "pressed"
            self._animate_to(self.pressed_fill, steps=3)

    def _release(self, event=None) -> None:
        if not self.enabled:
            return
        inside = (
            event is not None
            and 0 <= event.x < self.button_width
            and 0 <= event.y < self.button_height
        )
        self.state = "hover" if inside else "normal"
        self._animate_to(self._state_fill())
        if inside:
            self.command()

    def _keyboard_activate(self, _event=None) -> str:
        if self.enabled:
            self.command()
        return "break"

    def _animate_to(self, target: str, steps: int = 6) -> None:
        if self.animation_job is not None:
            self.after_cancel(self.animation_job)
            self.animation_job = None
        start = self.current_fill

        def animate(step: int = 1) -> None:
            self.current_fill = blend_color(start, target, step / steps)
            self._draw()
            if step < steps:
                self.animation_job = self.after(14, animate, step + 1)
            else:
                self.animation_job = None

        animate()

    def _draw(self) -> None:
        self.delete("all")
        fill = self.current_fill
        if not self.enabled:
            fill = blend_color(fill, self.palette[self.outer_key], 0.45)
        self._rounded_rectangle(
            0,
            0,
            self.button_width,
            self.button_height,
            self.radius,
            fill,
        )
        color = self.foreground if self.enabled else self.palette["muted"]
        if self.icon and not self.text:
            self._draw_icon(self.icon, self.button_width / 2, self.button_height / 2, color)
        elif self.icon:
            icon_x = 19
            self._draw_icon(self.icon, icon_x, self.button_height / 2, color, size=15)
            self.create_text(
                self.button_width / 2 + 7,
                self.button_height / 2,
                text=self.text,
                fill=color,
                font=self.font,
            )
        else:
            self.create_text(
                self.button_width / 2,
                self.button_height / 2,
                text=self.text,
                fill=color,
                font=self.font,
            )

    def _rounded_rectangle(
        self, left: int, top: int, right: int, bottom: int, radius: int, fill: str
    ) -> None:
        radius = min(radius, (right - left) // 2, (bottom - top) // 2)
        self.create_rectangle(left + radius, top, right - radius, bottom, fill=fill, outline="")
        self.create_rectangle(left, top + radius, right, bottom - radius, fill=fill, outline="")
        diameter = radius * 2
        for x, y, start in (
            (left, top, 90),
            (right - diameter, top, 0),
            (right - diameter, bottom - diameter, 270),
            (left, bottom - diameter, 180),
        ):
            self.create_arc(
                x,
                y,
                x + diameter,
                y + diameter,
                start=start,
                extent=90,
                style="pieslice",
                fill=fill,
                outline="",
            )

    def _draw_icon(
        self, icon: str, x: float, y: float, color: str, *, size: int = 18
    ) -> None:
        half = size / 2
        line = {"fill": color, "width": 2, "capstyle": "round", "joinstyle": "round"}
        if icon == "settings":
            self.create_oval(x - 3, y - 3, x + 3, y + 3, outline=color, width=2)
            self.create_oval(x - 7, y - 7, x + 7, y + 7, outline=color, width=2)
            for dx, dy in ((0, -half), (half, 0), (0, half), (-half, 0)):
                self.create_line(x + dx * 0.72, y + dy * 0.72, x + dx, y + dy, **line)
        elif icon == "moon":
            self.create_arc(
                x - 7,
                y - 8,
                x + 7,
                y + 8,
                start=70,
                extent=230,
                style="arc",
                outline=color,
                width=2,
            )
            self.create_arc(
                x - 2,
                y - 8,
                x + 9,
                y + 6,
                start=95,
                extent=190,
                style="arc",
                outline=color,
                width=2,
            )
        elif icon == "sun":
            self.create_oval(x - 4, y - 4, x + 4, y + 4, outline=color, width=2)
            for dx, dy in (
                (0, -half), (half, 0), (0, half), (-half, 0),
                (-6, -6), (6, -6), (6, 6), (-6, 6),
            ):
                length = max(abs(dx), abs(dy))
                self.create_line(
                    x + dx * 0.72,
                    y + dy * 0.72,
                    x + dx / length * half,
                    y + dy / length * half,
                    **line,
                )
        elif icon == "pause":
            self.create_line(x - 4, y - 6, x - 4, y + 6, **line)
            self.create_line(x + 4, y - 6, x + 4, y + 6, **line)
        elif icon == "play":
            self.create_polygon(
                x - 5,
                y - 7,
                x + 7,
                y,
                x - 5,
                y + 7,
                fill="",
                outline=color,
                width=2,
            )
        elif icon == "qr":
            for left, top in ((x - 8, y - 8), (x + 2, y - 8), (x - 8, y + 2)):
                self.create_rectangle(left, top, left + 6, top + 6, outline=color, width=2)
            self.create_line(x + 3, y + 3, x + 8, y + 3, x + 8, y + 8, **line)
            self.create_line(x + 3, y + 8, x + 5, y + 8, **line)
        elif icon in {"chevron-down", "chevron-up"}:
            direction = 1 if icon == "chevron-down" else -1
            self.create_line(
                x - 6,
                y - 3 * direction,
                x,
                y + 3 * direction,
                x + 6,
                y - 3 * direction,
                **line,
            )
        elif icon == "copy":
            self.create_rectangle(x - 7, y - 7, x + 4, y + 4, outline=color, width=2)
            self.create_rectangle(x - 3, y - 3, x + 8, y + 8, outline=color, width=2)
        elif icon == "refresh":
            self.create_arc(
                x - 7, y - 7, x + 7, y + 7, start=35, extent=275,
                style="arc", outline=color, width=2,
            )
            self.create_line(x + 4, y - 7, x + 8, y - 6, x + 7, y - 2, **line)


class ToggleSwitch(tk.Canvas):
    def __init__(
        self,
        master,
        variable: tk.BooleanVar,
        palette: dict,
        *,
        outer_key: str = "card",
    ) -> None:
        super().__init__(
            master,
            width=42,
            height=24,
            borderwidth=0,
            highlightthickness=0,
            cursor="hand2",
            takefocus=1,
        )
        self.variable = variable
        self.palette = palette
        self.outer_key = outer_key
        self.hovered = False
        self.bind("<Button-1>", self._toggle)
        self.bind("<space>", self._toggle)
        self.bind("<Return>", self._toggle)
        self.bind("<Enter>", self._enter)
        self.bind("<Leave>", self._leave)
        self.variable.trace_add("write", lambda *_args: self._draw())
        self.set_palette(palette)

    def set_palette(self, palette: dict) -> None:
        self.palette = palette
        self.configure(background=palette[self.outer_key])
        self._draw()

    def _toggle(self, _event=None) -> str:
        self.variable.set(not self.variable.get())
        return "break"

    def _enter(self, _event=None) -> None:
        self.hovered = True
        self._draw()

    def _leave(self, _event=None) -> None:
        self.hovered = False
        self._draw()

    def _draw(self) -> None:
        self.delete("all")
        enabled = self.variable.get()
        fill = self.palette["primary"] if enabled else self.palette["secondary_pressed"]
        if self.hovered:
            fill = blend_color(fill, "#FFFFFF" if enabled else self.palette["ink"], 0.10)
        self.create_oval(0, 0, 24, 24, fill=fill, outline="")
        self.create_oval(18, 0, 42, 24, fill=fill, outline="")
        self.create_rectangle(12, 0, 30, 24, fill=fill, outline="")
        knob_x = 30 if enabled else 12
        self.create_oval(
            knob_x - 9,
            3,
            knob_x + 9,
            21,
            fill="#FFFFFF",
            outline="",
        )


class BridgeApp:
    def __init__(self, activation_event=None, activation_ack_event=None) -> None:
        self.settings = load_settings()
        self.theme_name = self.settings["theme"]
        self.palette = THEMES[self.theme_name].copy()
        self.root = tk.Tk()
        self.root.withdraw()
        self.root.title(APP_NAME)
        self.root.resizable(False, False)
        self.root.configure(background=self.palette["root"])
        self.root.tk.call("tk", "scaling", self.root.winfo_fpixels("1i") / 72.0)
        if ICON_ICO.is_file():
            self.root.iconbitmap(str(ICON_ICO))
        self.root.protocol("WM_DELETE_WINDOW", self.request_close)

        self.closing = False
        self.server = None
        self.thread = None
        self.tray_icon = None
        self.tray_actions = queue.Queue()
        self.qr_window = None
        self.settings_window = None
        self.close_dialog = None
        self.rounded_cards = []
        self.animated_buttons = []
        self.toggle_switches = []
        self.activation_event = activation_event
        self.activation_ack_event = activation_ack_event
        self.init_results = queue.Queue(maxsize=1)
        self._configure_styles()
        self._build_loading()
        self._show_centered_window(self.root)
        self.root.after(50, self._start_initialization)

    def _build_loading(self) -> None:
        self.loading_frame = ttk.Frame(self.root, padding=(54, 38), style="Root.TFrame")
        self.loading_frame.pack(fill="both", expand=True)
        if ICON_PNG.is_file():
            icon = Image.open(ICON_PNG).resize((76, 76), Image.Resampling.LANCZOS)
            self.loading_icon = ImageTk.PhotoImage(icon)
            ttk.Label(self.loading_frame, image=self.loading_icon, style="Root.TLabel").pack()
        ttk.Label(self.loading_frame, text="声桥", style="Title.TLabel").pack(pady=(12, 2))
        ttk.Label(self.loading_frame, text="正在连接本机服务…", style="Subtitle.TLabel").pack()
        self.loading_progress = ttk.Progressbar(
            self.loading_frame,
            mode="indeterminate",
            length=280,
            style="Loading.Horizontal.TProgressbar",
        )
        self.loading_progress.pack(pady=(22, 0))
        self.loading_progress.start(12)

    def _start_initialization(self) -> None:
        threading.Thread(target=self._initialize_bridge, daemon=True).start()
        self.root.after(30, self._poll_initialization)

    def _initialize_bridge(self) -> None:
        server = None
        try:
            lan_ip, bind_ip, network_address_available = startup_lan_binding()
            token = load_or_create_token()
            injector = WindowsUnicodeInput()
            try:
                server = BridgeServer(
                    (bind_ip, DEFAULT_PORT),
                    token,
                    lambda text: self._inject_and_count(injector, text),
                    self.set_status,
                    injector.press_enter,
                )
            except OSError:
                if bind_ip == "127.0.0.1":
                    raise
                bind_ip = "127.0.0.1"
                network_address_available = False
                server = BridgeServer(
                    (bind_ip, DEFAULT_PORT),
                    token,
                    lambda text: self._inject_and_count(injector, text),
                    self.set_status,
                    injector.press_enter,
                )
        except Exception as error:
            self.init_results.put((False, error))
            return

        if self.closing:
            server.server_close()
            return
        self.init_results.put(
            (
                True,
                (lan_ip, bind_ip, network_address_available, token, server),
            )
        )

    def _poll_initialization(self) -> None:
        try:
            succeeded, result = self.init_results.get_nowait()
        except queue.Empty:
            if not self.closing:
                self.root.after(30, self._poll_initialization)
            return

        if not succeeded:
            self.loading_progress.stop()
            messagebox.showerror("启动失败", f"声桥无法启动：{result}", parent=self.root)
            self.root.destroy()
            return

        (
            self.lan_ip,
            self.server_bound_ip,
            self.network_address_available,
            self.token,
            self.server,
        ) = result
        self.url = self._pairing_url()
        self.qr_visible = False
        self.root.withdraw()
        self.loading_progress.stop()
        self.loading_frame.destroy()
        self.root.resizable(True, True)
        self._build_ui()
        if not self.network_address_available:
            self.status.set(
                f"已保存地址 {self.lan_ip} 当前不可用，请打开二维码并点击“刷新地址”"
            )
        self.root.update_idletasks()
        width = max(self.main_frame.winfo_reqwidth() + 24, 580)
        height = max(self.main_frame.winfo_reqheight() + 20, 360)
        self.root.geometry(f"{width}x{height}")
        self.root.minsize(520, 330)
        self._show_centered_window(self.root)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.root.after(100, self._poll_tray_actions)

    def _inject_and_count(self, injector: WindowsUnicodeInput, text: str) -> None:
        injector(text)
        count = add_character_count(len(text))
        self.settings["character_count"] = count
        if not self.closing:
            self.root.after(0, self._update_character_count, count)

    def _center_window(self, window: tk.Toplevel | tk.Tk) -> None:
        window.update_idletasks()
        width = max(window.winfo_width(), window.winfo_reqwidth())
        height = max(window.winfo_height(), window.winfo_reqheight())
        try:
            class Rect(ctypes.Structure):
                _fields_ = (
                    ("left", ctypes.c_long),
                    ("top", ctypes.c_long),
                    ("right", ctypes.c_long),
                    ("bottom", ctypes.c_long),
                )

            frame = window.tk.call("wm", "frame", window._w)
            handle = int(str(frame), 0)
            window_rect = Rect()
            user32 = ctypes.windll.user32
            user32.GetWindowRect.argtypes = (wintypes.HWND, ctypes.POINTER(Rect))
            user32.GetWindowRect(handle, ctypes.byref(window_rect))
            width = window_rect.right - window_rect.left
            height = window_rect.bottom - window_rect.top
        except (AttributeError, OSError, tk.TclError, ValueError):
            pass
        x = max(0, (window.winfo_screenwidth() - width) // 2)
        y = max(0, (window.winfo_screenheight() - height) // 2)
        window.geometry(f"+{x}+{y}")

    def _show_centered_window(self, window: tk.Toplevel | tk.Tk) -> None:
        try:
            window.attributes("-alpha", 0.0)
        except tk.TclError:
            pass
        window.deiconify()
        window.update_idletasks()
        self._center_window(window)
        window.update_idletasks()
        self._apply_window_chrome(window)
        try:
            window.attributes("-alpha", 1.0)
        except tk.TclError:
            pass
        window.after_idle(self._apply_window_chrome, window)

    def _apply_window_chrome(self, window: tk.Toplevel | tk.Tk) -> None:
        try:
            window.update_idletasks()
            frame = window.tk.call("wm", "frame", window._w)
            handle = int(str(frame), 0)
            root_color = self.palette["title"]
            ink_color = self.palette["ink"]
            channels = [int(root_color[index:index + 2], 16) for index in (1, 3, 5)]
            use_dark = ctypes.c_int(int(sum(channels) < 384))
            set_attribute = ctypes.windll.dwmapi.DwmSetWindowAttribute
            set_attribute.argtypes = (
                wintypes.HWND,
                wintypes.DWORD,
                ctypes.c_void_p,
                wintypes.DWORD,
            )
            set_attribute.restype = ctypes.c_long
            corner_preference = ctypes.c_int(2)
            set_attribute(
                handle,
                33,
                ctypes.byref(corner_preference),
                ctypes.sizeof(corner_preference),
            )
            for attribute in (20, 19):
                result = set_attribute(
                    handle,
                    attribute,
                    ctypes.byref(use_dark),
                    ctypes.sizeof(use_dark),
                )
                if result == 0:
                    break
            for attribute, color in (
                (34, root_color),
                (35, root_color),
                (36, ink_color),
            ):
                value = ctypes.c_uint(windows_colorref(color))
                set_attribute(
                    handle,
                    attribute,
                    ctypes.byref(value),
                    ctypes.sizeof(value),
                )
        except (AttributeError, OSError, tk.TclError):
            pass

    def _button(self, master, command, **options) -> AnimatedButton:
        button = AnimatedButton(master, command, self.palette, **options)
        self.animated_buttons.append(button)
        return button

    def _switch(
        self, master, variable: tk.BooleanVar, *, outer_key: str = "card"
    ) -> ToggleSwitch:
        switch = ToggleSwitch(
            master,
            variable,
            self.palette,
            outer_key=outer_key,
        )
        self.toggle_switches.append(switch)
        return switch

    def _build_ui(self) -> None:
        frame = ttk.Frame(self.root, padding=18, style="Root.TFrame")
        self.main_frame = frame
        frame.pack(fill="both", expand=True)

        header = ttk.Frame(frame, style="Root.TFrame")
        header.pack(fill="x", pady=(0, 14))
        if ICON_PNG.is_file():
            icon = Image.open(ICON_PNG).resize((48, 48), Image.Resampling.LANCZOS)
            self.header_icon = ImageTk.PhotoImage(icon)
            ttk.Label(header, image=self.header_icon, style="Root.TLabel").pack(
                side="left", padx=(0, 13)
            )
        title_box = ttk.Frame(header, style="Root.TFrame")
        title_box.pack(side="left")
        ttk.Label(title_box, text="声桥", style="Title.TLabel").pack(anchor="w")
        ttk.Label(
            title_box,
            text="用手机说，在电脑上写",
            style="Subtitle.TLabel",
        ).pack(anchor="w", pady=(2, 0))
        header_actions = ttk.Frame(header, style="Root.TFrame")
        header_actions.pack(side="right")
        self.theme_button = self._button(
            header_actions,
            self.toggle_theme,
            icon="sun" if self.theme_name == "dark" else "moon",
            width=40,
            height=40,
            radius=20,
            outer_key="root",
        )
        self.theme_button.pack(side="left", padx=(0, 8))
        self.settings_button = self._button(
            header_actions,
            self.open_settings,
            icon="settings",
            width=40,
            height=40,
            radius=20,
            outer_key="root",
        )
        self.settings_button.pack(side="left")

        connection_shell = RoundedCard(
            frame,
            self.palette["root"],
            self.palette["card"],
            padding=16,
            radius=22,
        )
        self.rounded_cards.append(connection_shell)
        connection_shell.pack(fill="x", pady=(0, 12))
        connection_card = connection_shell.content
        connection_header = ttk.Frame(connection_card, style="Card.TFrame")
        connection_header.pack(fill="x")
        connection_title = ttk.Frame(connection_header, style="Card.TFrame")
        connection_title.pack(side="left")
        ttk.Label(connection_title, text="手机连接", style="CardTitle.TLabel").pack(anchor="w")
        ttk.Label(connection_title, text="已配对手机可直接打开收藏页面", style="Muted.TLabel").pack(anchor="w", pady=(2, 0))
        self.qr_toggle_button = self._button(
            connection_header,
            self.toggle_qr,
            icon="qr",
            width=40,
            height=40,
            radius=20,
        )
        self.qr_toggle_button.pack(side="right")
        self.pause_button = self._button(
            connection_header,
            self.toggle_receiving,
            icon="pause",
            width=40,
            height=40,
            radius=20,
        )
        self.pause_button.pack(side="right", padx=(0, 8))
        self.address_text = tk.StringVar(value=self.url)
        self.qr_window = None
        self.qr_label = None
        self.qr_photo = None
        connection_shell.fit_content()

        self.transfer_shell = RoundedCard(
            frame,
            self.palette["root"],
            self.palette["card"],
            padding=16,
            radius=22,
        )
        self.rounded_cards.append(self.transfer_shell)
        self.transfer_shell.pack(fill="x", pady=(0, 12))
        transfer_card = self.transfer_shell.content
        transfer_header = ttk.Frame(transfer_card, style="Card.TFrame")
        transfer_header.pack(fill="x")
        transfer_title = ttk.Frame(transfer_header, style="Card.TFrame")
        transfer_title.pack(side="left")
        ttk.Label(
            transfer_title,
            text="电脑 → 手机",
            style="CardTitle.TLabel",
        ).pack(anchor="w")
        ttk.Label(
            transfer_title,
            text="展开后输入文字并发送到手机当前光标",
            style="Muted.TLabel",
        ).pack(anchor="w", pady=(2, 0))
        self.transfer_toggle_button = self._button(
            transfer_header,
            self.toggle_transfer_panel,
            icon="chevron-down",
            width=40,
            height=40,
            radius=20,
        )
        self.transfer_toggle_button.pack(side="right")

        self.transfer_content = ttk.Frame(transfer_card, style="Card.TFrame")
        self.desktop_text = tk.Text(
            self.transfer_content,
            width=48,
            height=5,
            wrap="word",
            font=("Microsoft YaHei UI", 10),
            relief="flat",
            highlightthickness=1,
            background=self.palette["input"],
            foreground=self.palette["ink"],
            insertbackground=self.palette["ink"],
            highlightbackground=self.palette["line"],
            highlightcolor="#2563EB",
            padx=12,
            pady=10,
        )
        self.desktop_text.pack(fill="both", expand=True, pady=(12, 10))
        self.desktop_text.bind("<Return>", self._handle_enter)
        self.desktop_text.bind("<Control-Return>", self._handle_control_enter)
        send_row = ttk.Frame(self.transfer_content, style="Card.TFrame")
        send_row.pack(fill="x")
        self.shortcut_hint = tk.StringVar()
        self._update_shortcut_hint()
        ttk.Label(send_row, textvariable=self.shortcut_hint, style="Muted.TLabel").pack(side="left")
        self.send_phone_button = self._button(
            send_row,
            self.send_to_phone,
            text="发送到手机",
            width=122,
            height=42,
            radius=15,
            role="primary",
        )
        self.send_phone_button.pack(side="right")
        self.transfer_expanded = False
        self.transfer_shell.fit_content()

        self.status = tk.StringVar(value="等待手机连接")
        self.status_shell = RoundedCard(
            frame,
            self.palette["root"],
            self.palette["status"],
            padding=(12, 9),
            radius=12,
            content_style="Status.TFrame",
        )
        self.rounded_cards.append(self.status_shell)
        self.status_shell.pack(fill="x")
        status_row = self.status_shell.content
        ttk.Label(status_row, text="●", style="StatusDot.TLabel").pack(side="left")
        ttk.Label(status_row, textvariable=self.status, style="Status.TLabel").pack(side="left", padx=(6, 0))
        self.character_count_text = tk.StringVar()
        self.character_count_label = ttk.Label(
            status_row, textvariable=self.character_count_text, style="Status.TLabel"
        )
        self._update_character_count(self.settings["character_count"])
        if self.settings["show_char_count"]:
            self.character_count_label.pack(side="right")
        self.status_shell.fit_content()
        if not self.settings["show_desktop_to_phone"]:
            self.transfer_shell.pack_forget()

    def _configure_styles(self) -> None:
        palette = self.palette
        style = ttk.Style(self.root)
        style.theme_use("clam")
        style.configure("Root.TFrame", background=palette["root"])
        style.configure("Root.TLabel", background=palette["root"], foreground=palette["ink"])
        style.configure("Title.TLabel", background=palette["root"], foreground=palette["ink"], font=("Microsoft YaHei UI", 22, "bold"))
        style.configure("Subtitle.TLabel", background=palette["root"], foreground=palette["muted"], font=("Microsoft YaHei UI", 10))
        style.configure("Card.TFrame", background=palette["card"])
        style.configure("Card.TLabel", background=palette["card"], foreground=palette["ink"])
        style.configure("CardTitle.TLabel", background=palette["card"], foreground=palette["ink"], font=("Microsoft YaHei UI", 11, "bold"))
        style.configure("Muted.TLabel", background=palette["card"], foreground=palette["muted"], font=("Microsoft YaHei UI", 9))
        style.configure("Card.TCheckbutton", background=palette["card"], foreground=palette["ink"], font=("Microsoft YaHei UI", 9))
        style.map("Card.TCheckbutton", background=[("active", palette["card"])])
        style.configure("Card.TRadiobutton", background=palette["card"], foreground=palette["ink"], font=("Microsoft YaHei UI", 9))
        style.map("Card.TRadiobutton", background=[("active", palette["card"])])
        style.configure("Accent.TButton", background=palette["primary"], foreground="#FFFFFF", borderwidth=0, padding=(18, 9), font=("Microsoft YaHei UI", 9, "bold"))
        style.map("Accent.TButton", background=[("active", palette["primary_hover"]), ("pressed", palette["primary_pressed"])])
        style.configure("Secondary.TButton", background=palette["secondary"], foreground=palette["ink"], borderwidth=0, padding=(12, 8), font=("Microsoft YaHei UI", 9))
        style.map("Secondary.TButton", background=[("active", palette["secondary_hover"])])
        style.configure("Danger.TButton", background=palette["danger"], foreground=palette["danger_ink"], borderwidth=0, padding=(12, 8), font=("Microsoft YaHei UI", 9))
        style.map("Danger.TButton", background=[("active", palette["danger_hover"])])
        style.configure("Status.TFrame", background=palette["status"])
        style.configure("Status.TLabel", background=palette["status"], foreground=palette["status_ink"], font=("Microsoft YaHei UI", 9))
        style.configure("StatusDot.TLabel", background=palette["status"], foreground="#10B981")
        style.configure(
            "Loading.Horizontal.TProgressbar",
            background="#2563EB",
            troughcolor="#DCE8F8",
            bordercolor="#DCE8F8",
            lightcolor="#2563EB",
            darkcolor="#2563EB",
        )

    def _pairing_url(self) -> str:
        return f"http://{self.lan_ip}:{self.server.server_port}/?token={self.token}"

    @staticmethod
    def _shutdown_replaced_server(server, thread) -> None:
        server.shutdown()
        server.server_close()
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=1)

    def _refresh_network_binding(self) -> bool:
        if self.closing or self.server is None:
            return False
        try:
            current_ip = local_ip()
        except Exception as error:
            self.set_status(f"检测当前局域网失败：{error}")
            return False
        if current_ip == "127.0.0.1":
            self.set_status("当前未找到可用局域网，地址保持不变")
            return False
        if current_ip == self.lan_ip and current_ip == self.server_bound_ip:
            self.set_status(f"当前地址未变化：{current_ip}")
            return False

        old_server = self.server
        old_thread = self.thread
        replacement = None
        try:
            replacement = BridgeServer(
                (current_ip, DEFAULT_PORT),
                self.token,
                old_server.input_callback,
                old_server.status_callback,
                old_server.enter_callback,
                old_server.config_file,
            )
            replacement.accepting_phone_text = old_server.accepting_phone_text
            with old_server._phone_condition:
                replacement._phone_messages = [
                    item.copy() for item in old_server._phone_messages
                ]
                replacement._next_message_id = old_server._next_message_id
            new_thread = threading.Thread(
                target=replacement.serve_forever, daemon=True
            )
            new_thread.start()
            update_config({"lan_ip": current_ip}, old_server.config_file)
        except Exception as error:
            if replacement is not None:
                replacement.shutdown()
                replacement.server_close()
            self.set_status(f"刷新当前局域网地址失败：{error}")
            return False

        self.lan_ip = current_ip
        self.server_bound_ip = current_ip
        self.network_address_available = True
        self.server = replacement
        self.thread = new_thread
        self.refresh_pairing()
        self.set_status(f"地址已刷新并保存：{current_ip}")
        threading.Thread(
            target=self._shutdown_replaced_server,
            args=(old_server, old_thread),
            daemon=True,
        ).start()
        return True

    def refresh_pairing(self) -> None:
        self.url = self._pairing_url()
        self.address_text.set(self.url)
        if self.qr_label is not None:
            qr_image = qrcode.make(self.url).resize((240, 240))
            self.qr_photo = ImageTk.PhotoImage(qr_image)
            self.qr_label.configure(image=self.qr_photo)

    def toggle_qr(self) -> None:
        if self.qr_window is not None and self.qr_window.winfo_exists():
            self.close_qr()
            return
        self.qr_visible = True
        self.qr_window = tk.Toplevel(self.root)
        self.qr_window.withdraw()
        self.qr_window.title("手机配对")
        self.qr_window.resizable(False, False)
        self.qr_window.configure(background=self.palette["root"])
        if ICON_ICO.is_file():
            self.qr_window.iconbitmap(str(ICON_ICO))
        self.qr_window.protocol("WM_DELETE_WINDOW", self.close_qr)

        content = ttk.Frame(self.qr_window, padding=22, style="Root.TFrame")
        content.pack()
        ttk.Label(content, text="扫描二维码连接声桥", style="Title.TLabel").pack()
        ttk.Label(content, text="首次配对后请收藏手机页面", style="Subtitle.TLabel").pack(pady=(2, 12))
        qr_image = qrcode.make(self.url).resize((240, 240))
        self.qr_photo = ImageTk.PhotoImage(qr_image)
        self.qr_label = ttk.Label(content, image=self.qr_photo, style="Root.TLabel")
        self.qr_label.pack()
        ttk.Entry(
            content,
            width=58,
            justify="center",
            state="readonly",
            textvariable=self.address_text,
        ).pack(fill="x", pady=(10, 10))
        actions = ttk.Frame(content, style="Root.TFrame")
        actions.pack()
        self._button(
            actions,
            self.copy_url,
            text="复制访问地址",
            icon="copy",
            width=132,
            height=42,
            radius=15,
            outer_key="root",
        ).pack(side="left", padx=4)
        self._button(
            actions,
            self._refresh_network_binding,
            text="刷新地址",
            icon="refresh",
            width=116,
            height=42,
            radius=15,
            role="primary",
            outer_key="root",
        ).pack(side="left", padx=4)
        self._button(
            actions,
            self.rotate_pairing,
            text="重新配对",
            width=112,
            height=42,
            radius=15,
            role="danger",
            outer_key="root",
        ).pack(side="left", padx=4)
        self.qr_toggle_button.set_selected(True)
        self._show_centered_window(self.qr_window)

    def close_qr(self) -> None:
        if self.qr_window is not None and self.qr_window.winfo_exists():
            self.qr_window.destroy()
        self.qr_window = None
        self.qr_label = None
        self.qr_photo = None
        self.qr_visible = False
        self.qr_toggle_button.set_selected(False)

    def copy_url(self) -> None:
        self.root.clipboard_clear()
        self.root.clipboard_append(self.url)
        self.set_status("手机访问地址已复制")

    def send_to_phone(self, _event=None) -> str:
        text = self.desktop_text.get("1.0", "end-1c")
        if not text:
            self.set_status("请先输入要发给手机的文字")
            return "break"
        self.server.queue_for_phone(text)
        self.desktop_text.delete("1.0", "end")
        self.set_status(f"已发送 {len(text)} 个字符到手机")
        return "break"

    def toggle_transfer_panel(self) -> None:
        self.transfer_expanded = not self.transfer_expanded
        if self.transfer_expanded:
            self.transfer_content.pack(fill="both", expand=True)
            self.transfer_toggle_button.set_icon("chevron-up")
            self.transfer_shell.pack_configure(fill="both", expand=True)
            minimum_height = 520
        else:
            self.transfer_content.pack_forget()
            self.transfer_toggle_button.set_icon("chevron-down")
            self.transfer_shell.pack_configure(fill="x", expand=False)
            minimum_height = 330
        self.transfer_shell.fit_content()
        self.root.update_idletasks()
        self.root.minsize(520, minimum_height)
        current_width = self.root.winfo_width()
        current_height = self.root.winfo_height()
        if self.transfer_expanded and current_height < minimum_height:
            self.root.geometry(f"{current_width}x{minimum_height}")
        elif not self.transfer_expanded and current_height > 430:
            compact_height = max(self.main_frame.winfo_reqheight() + 20, 360)
            self.root.geometry(f"{current_width}x{compact_height}")

    def _apply_desktop_to_phone_visibility(self) -> None:
        visible = self.settings["show_desktop_to_phone"]
        if visible:
            if not self.transfer_shell.winfo_manager():
                self.transfer_shell.pack(
                    fill="both" if self.transfer_expanded else "x",
                    expand=self.transfer_expanded,
                    pady=(0, 12),
                    before=self.status_shell,
                )
        else:
            self.transfer_shell.pack_forget()
            if self.transfer_expanded:
                self.transfer_expanded = False
                self.transfer_content.pack_forget()
                self.transfer_toggle_button.set_icon("chevron-down")
        minimum_height = 520 if visible and self.transfer_expanded else 300
        self.root.minsize(520, minimum_height)
        self.root.update_idletasks()

    def _handle_enter(self, _event=None):
        if self.settings["send_mode"] == "enter":
            return self.send_to_phone()
        return None

    def _handle_control_enter(self, _event=None):
        if self.settings["send_mode"] == "ctrl_enter":
            return self.send_to_phone()
        self.desktop_text.insert("insert", "\n")
        return "break"

    def _update_shortcut_hint(self) -> None:
        if self.settings["send_mode"] == "ctrl_enter":
            self.shortcut_hint.set("Ctrl + Enter 发送 · Enter 换行")
        else:
            self.shortcut_hint.set("Enter 发送 · Ctrl + Enter 换行")

    def _update_character_count(self, count: int) -> None:
        if hasattr(self, "character_count_text"):
            self.character_count_text.set(f"累计输入 {count} 个字符")

    def _settings_switch_row(
        self,
        parent,
        text: str,
        variable: tk.BooleanVar,
    ) -> None:
        row = ttk.Frame(parent, style="Card.TFrame")
        row.pack(fill="x", pady=7)
        ttk.Label(row, text=text, style="Card.TLabel").pack(side="left")
        self._switch(row, variable).pack(side="right")

    def _settings_choice_row(
        self,
        parent,
        variable: tk.StringVar,
        options: tuple[tuple[str, str], ...],
        *,
        width: int,
    ) -> None:
        row = ttk.Frame(parent, style="Card.TFrame")
        row.pack(fill="x", pady=(5, 0))
        buttons = []

        def choose(value: str) -> None:
            variable.set(value)
            for button, option_value in buttons:
                button.set_selected(option_value == value)

        for index, (label, value) in enumerate(options):
            button = self._button(
                row,
                lambda selected=value: choose(selected),
                text=label,
                width=width,
                height=38,
                radius=14,
            )
            button.pack(side="left", padx=(0, 8) if index < len(options) - 1 else 0)
            buttons.append((button, value))
        choose(variable.get())

    def open_settings(self) -> None:
        if self.settings_window is not None and self.settings_window.winfo_exists():
            self.settings_window.lift()
            return
        window = tk.Toplevel(self.root)
        window.withdraw()
        self.settings_window = window
        window.title("声桥设置")
        window.resizable(True, True)
        window.configure(background=self.palette["root"])
        window.transient(self.root)
        if ICON_ICO.is_file():
            window.iconbitmap(str(ICON_ICO))
        window.protocol("WM_DELETE_WINDOW", window.destroy)

        content = ttk.Frame(window, padding=18, style="Root.TFrame")
        content.pack(fill="both", expand=True)
        ttk.Label(content, text="设置", style="Title.TLabel").pack(anchor="w")
        ttk.Label(
            content,
            text="偏好保存在电脑，手机端打开页面时同步确认设置",
            style="Subtitle.TLabel",
        ).pack(anchor="w", pady=(2, 14))

        interface_shell = RoundedCard(
            content,
            self.palette["root"],
            self.palette["card"],
            padding=16,
            radius=22,
        )
        self.rounded_cards.append(interface_shell)
        interface_shell.pack(fill="x", pady=(0, 12))
        interface_card = interface_shell.content
        self.settings_autostart = tk.BooleanVar(value=autostart_enabled())
        self.settings_close_action = tk.StringVar(value=self.settings["close_action"])
        self.settings_send_mode = tk.StringVar(value=self.settings["send_mode"])
        self.settings_show_count = tk.BooleanVar(value=self.settings["show_char_count"])
        self.settings_show_transfer = tk.BooleanVar(
            value=self.settings["show_desktop_to_phone"]
        )
        self.settings_confirm_enter = tk.BooleanVar(
            value=self.settings["confirm_enter"]
        )
        self.settings_confirm_send = tk.BooleanVar(
            value=self.settings["confirm_send"]
        )

        ttk.Label(
            interface_card,
            text="界面与启动",
            style="CardTitle.TLabel",
        ).pack(anchor="w", pady=(0, 4))
        self._settings_switch_row(
            interface_card,
            "开机后自动启动",
            self.settings_autostart,
        )
        self._settings_switch_row(
            interface_card,
            "显示电脑 → 手机",
            self.settings_show_transfer,
        )
        self._settings_switch_row(
            interface_card,
            "显示累计输入字符数",
            self.settings_show_count,
        )

        count_row = ttk.Frame(interface_card, style="Card.TFrame")
        count_row.pack(fill="x", pady=(6, 0))
        self.settings_count_text = tk.StringVar(value=f"当前 {self.settings['character_count']} 个")
        ttk.Label(
            count_row,
            text="累计字符",
            style="Card.TLabel",
        ).pack(side="left")
        ttk.Label(
            count_row,
            textvariable=self.settings_count_text,
            style="Muted.TLabel",
        ).pack(side="left", padx=(8, 0))
        self._button(
            count_row,
            self.clear_character_count,
            text="清零",
            width=70,
            height=34,
            radius=13,
            role="danger",
        ).pack(side="right")
        interface_shell.fit_content()

        behavior_shell = RoundedCard(
            content,
            self.palette["root"],
            self.palette["card"],
            padding=16,
            radius=22,
        )
        self.rounded_cards.append(behavior_shell)
        behavior_shell.pack(fill="x")
        behavior_card = behavior_shell.content
        ttk.Label(
            behavior_card,
            text="操作方式",
            style="CardTitle.TLabel",
        ).pack(anchor="w", pady=(0, 4))
        self._settings_switch_row(
            behavior_card,
            "按下回车二次确认",
            self.settings_confirm_enter,
        )
        self._settings_switch_row(
            behavior_card,
            "发送到电脑二次确认",
            self.settings_confirm_send,
        )

        ttk.Label(
            behavior_card,
            text="关闭主窗口时",
            style="CardTitle.TLabel",
        ).pack(anchor="w", pady=(12, 0))
        self._settings_choice_row(
            behavior_card,
            self.settings_close_action,
            (
                ("每次询问", "ask"),
                ("最小化到托盘", "tray"),
                ("直接退出", "exit"),
            ),
            width=112,
        )
        ttk.Label(
            behavior_card,
            text="电脑发送快捷键",
            style="CardTitle.TLabel",
        ).pack(anchor="w", pady=(14, 0))
        self._settings_choice_row(
            behavior_card,
            self.settings_send_mode,
            (
                ("Ctrl + Enter 发送", "ctrl_enter"),
                ("Enter 发送", "enter"),
            ),
            width=174,
        )
        behavior_shell.fit_content()

        actions = ttk.Frame(content, style="Root.TFrame")
        actions.pack(fill="x", pady=(14, 0))
        self._button(
            actions,
            window.destroy,
            text="取消",
            width=94,
            height=42,
            radius=15,
            outer_key="root",
        ).pack(side="right")
        self._button(
            actions,
            self.save_settings,
            text="保存设置",
            width=112,
            height=42,
            radius=15,
            role="primary",
            outer_key="root",
        ).pack(side="right", padx=(0, 8))
        window.minsize(500, 600)
        self._show_centered_window(window)

    def save_settings(self) -> None:
        try:
            set_autostart(self.settings_autostart.get())
        except OSError as error:
            messagebox.showerror("设置失败", f"无法修改开机自启：{error}", parent=self.settings_window)
            return
        values = {
            "close_action": self.settings_close_action.get(),
            "send_mode": self.settings_send_mode.get(),
            "show_char_count": self.settings_show_count.get(),
            "show_desktop_to_phone": self.settings_show_transfer.get(),
            "confirm_enter": self.settings_confirm_enter.get(),
            "confirm_send": self.settings_confirm_send.get(),
        }
        update_config(values)
        self.settings.update(values)
        self._update_shortcut_hint()
        if self.settings["show_char_count"]:
            if not self.character_count_label.winfo_manager():
                self.character_count_label.pack(side="right")
        else:
            self.character_count_label.pack_forget()
        self._apply_desktop_to_phone_visibility()
        self.settings_window.destroy()
        self.set_status("设置已保存")

    def clear_character_count(self) -> None:
        update_config({"character_count": 0})
        self.settings["character_count"] = 0
        self._update_character_count(0)
        self.settings_count_text.set("当前 0 个")

    def toggle_receiving(self) -> None:
        self.server.accepting_phone_text = not self.server.accepting_phone_text
        if self.server.accepting_phone_text:
            self.pause_button.set_icon("pause")
            self.pause_button.set_selected(False)
            self.set_status("已恢复接收手机文字")
        else:
            self.pause_button.set_icon("play")
            self.pause_button.set_selected(True)
            self.set_status("已暂停接收手机文字")

    def rotate_pairing(self) -> None:
        confirmed = messagebox.askyesno(
            "重新配对",
            "旧的手机链接会立即失效，确定生成新链接吗？",
            parent=self.root,
        )
        if not confirmed:
            return
        self.token = load_or_create_token(rotate=True)
        self.server.token = self.token
        self.refresh_pairing()
        self.set_status("配对链接已更新，请重新扫码")

    @staticmethod
    def _blend_color(start: str, end: str, progress: float) -> str:
        return blend_color(start, end, progress)

    def toggle_theme(self) -> None:
        if getattr(self, "theme_animating", False):
            return
        target_name = "dark" if self.theme_name == "light" else "light"
        start = self.palette.copy()
        target = THEMES[target_name]
        self.theme_animating = True

        def animate(step: int = 1) -> None:
            linear = step / 16
            progress = linear * linear * (3 - 2 * linear)
            self.palette = {
                key: self._blend_color(start[key], target[key], progress)
                for key in target
            }
            self._apply_palette(update_chrome=False)
            self.root.update_idletasks()
            if step < 16:
                self.root.after(14, animate, step + 1)
                return
            self.theme_name = target_name
            self.settings["theme"] = target_name
            update_config({"theme": target_name})
            self.theme_button.set_icon("sun" if target_name == "dark" else "moon")
            self._apply_window_chrome(self.root)
            for window in (self.qr_window, self.settings_window, self.close_dialog):
                if window is not None and window.winfo_exists():
                    self._apply_window_chrome(window)
            self.theme_animating = False

        animate()

    def _apply_palette(self, update_chrome: bool = True) -> None:
        self.root.configure(background=self.palette["root"])
        self._configure_styles()
        self.rounded_cards = [
            card for card in self.rounded_cards if card.winfo_exists()
        ]
        for card in self.rounded_cards:
            fill_color = (
                self.palette["status"]
                if card.content.cget("style") == "Status.TFrame"
                else self.palette["card"]
            )
            card.set_colors(self.palette["root"], fill_color)
        self.animated_buttons = [
            button for button in self.animated_buttons if button.winfo_exists()
        ]
        for button in self.animated_buttons:
            button.set_palette(self.palette, immediate=True)
        self.toggle_switches = [
            switch for switch in self.toggle_switches if switch.winfo_exists()
        ]
        for switch in self.toggle_switches:
            switch.set_palette(self.palette)
        if hasattr(self, "desktop_text"):
            self.desktop_text.configure(
                background=self.palette["input"],
                foreground=self.palette["ink"],
                insertbackground=self.palette["ink"],
                highlightbackground=self.palette["line"],
            )
        for window in (self.qr_window, self.settings_window, self.close_dialog):
            if window is not None and window.winfo_exists():
                window.configure(background=self.palette["root"])
        if update_chrome:
            self._apply_window_chrome(self.root)
            for window in (self.qr_window, self.settings_window, self.close_dialog):
                if window is not None and window.winfo_exists():
                    self._apply_window_chrome(window)

    def request_close(self) -> None:
        action = self.settings["close_action"]
        if action == "tray":
            self.minimize_to_tray()
        elif action == "exit":
            self.exit_app()
        else:
            self.show_close_dialog()

    def show_close_dialog(self) -> None:
        if self.close_dialog is not None and self.close_dialog.winfo_exists():
            self.close_dialog.lift()
            return
        dialog = tk.Toplevel(self.root)
        dialog.withdraw()
        self.close_dialog = dialog
        dialog.title("关闭声桥")
        dialog.resizable(False, False)
        dialog.configure(background=self.palette["root"])
        dialog.transient(self.root)
        if ICON_ICO.is_file():
            dialog.iconbitmap(str(ICON_ICO))
        dialog.protocol("WM_DELETE_WINDOW", dialog.destroy)
        content = ttk.Frame(dialog, padding=24, style="Root.TFrame")
        content.pack()
        ttk.Label(content, text="关闭窗口后要做什么？", style="Title.TLabel").pack(anchor="w")
        ttk.Label(content, text="可在设置中修改以后关闭窗口的默认行为。", style="Subtitle.TLabel").pack(anchor="w", pady=(3, 18))
        buttons = ttk.Frame(content, style="Root.TFrame")
        buttons.pack(fill="x")
        tray_button = self._button(
            buttons,
            self._choose_tray,
            text="最小化到托盘",
            width=142,
            height=42,
            radius=15,
            role="primary",
            outer_key="root",
        )
        tray_button.pack(side="left")
        self._button(
            buttons,
            self._choose_exit,
            text="直接关闭",
            width=112,
            height=42,
            radius=15,
            role="danger",
            outer_key="root",
        ).pack(side="left", padx=(10, 0))
        dialog.bind("<Return>", lambda _event: self._choose_tray())
        dialog.bind("<Escape>", lambda _event: dialog.destroy())
        self._show_centered_window(dialog)
        dialog.grab_set()
        tray_button.focus_set()

    def _choose_tray(self) -> None:
        self.close_dialog.destroy()
        self.close_dialog = None
        self.minimize_to_tray()

    def _choose_exit(self) -> None:
        self.close_dialog.destroy()
        self.close_dialog = None
        self.exit_app()

    def minimize_to_tray(self) -> None:
        if self.settings_window is not None and self.settings_window.winfo_exists():
            self.settings_window.destroy()
        if self.qr_window is not None and self.qr_window.winfo_exists():
            self.close_qr()
        self.root.withdraw()
        if self.tray_icon is None:
            menu = pystray.Menu(
                pystray.MenuItem("打开声桥", lambda _icon, _item: self.tray_actions.put("show"), default=True),
                pystray.MenuItem("退出", lambda _icon, _item: self.tray_actions.put("exit")),
            )
            self.tray_icon = pystray.Icon(
                "VoiceBridge",
                Image.open(ICON_PNG).copy(),
                APP_NAME,
                menu,
            )
            self.tray_icon.run_detached()

    def _poll_tray_actions(self) -> None:
        if self.activation_event:
            kernel32 = ctypes.windll.kernel32
            kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
            kernel32.ResetEvent.argtypes = (wintypes.HANDLE,)
            kernel32.SetEvent.argtypes = (wintypes.HANDLE,)
            if kernel32.WaitForSingleObject(self.activation_event, 0) == 0:
                kernel32.ResetEvent(self.activation_event)
                self.restore_from_tray()
                if self.activation_ack_event:
                    kernel32.SetEvent(self.activation_ack_event)
        try:
            action = self.tray_actions.get_nowait()
        except queue.Empty:
            action = None
        if action == "show":
            self.restore_from_tray()
        elif action == "exit":
            self.exit_app()
            return
        if not self.closing:
            self.root.after(100, self._poll_tray_actions)

    def restore_from_tray(self) -> None:
        self.root.deiconify()
        self.root.state("normal")
        self.root.lift()
        self.root.focus_force()
        try:
            handle = self.root.winfo_id()
            user32 = ctypes.windll.user32
            user32.ShowWindow.argtypes = (wintypes.HWND, ctypes.c_int)
            user32.SetForegroundWindow.argtypes = (wintypes.HWND,)
            user32.ShowWindow(handle, 9)
            user32.SetForegroundWindow(handle)
        except (AttributeError, OSError, tk.TclError):
            pass

    def set_status(self, message: str) -> None:
        if not self.closing and hasattr(self, "status"):
            self.root.after(0, self.status.set, message)

    def exit_app(self) -> None:
        if self.closing:
            return
        self.closing = True
        if self.tray_icon is not None:
            self.tray_icon.stop()
            self.tray_icon = None
        if self.server is not None:
            self.server.shutdown()
            self.server.server_close()
        self.root.destroy()

    def run(self) -> None:
        self.root.mainloop()


if __name__ == "__main__":
    instance_handles = None
    try:
        enable_dpi_awareness()
        instance_handles = acquire_single_instance()
        if instance_handles is not None:
            BridgeApp(instance_handles[1], instance_handles[2]).run()
    except Exception as error:
        report_fatal_error(error)
    finally:
        if instance_handles is not None:
            release_single_instance(instance_handles)
