import ctypes
import inspect
import json
import queue
import re
import threading
import unittest
import win32clipboard
from pathlib import Path
from unittest.mock import Mock, patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from app import (
    BridgeApp,
    BridgeServer,
    THEMES,
    WEB_PAGE,
    WindowsUnicodeInput,
    add_character_count,
    choose_lan_ip,
    load_or_create_token,
    load_saved_lan_ip,
    load_settings,
    report_fatal_error,
    startup_lan_binding,
    startup_command,
    update_config,
    windows_colorref,
)


def replay_in_smart_markdown_editor(inputs, initial_text=""):
    output = list(initial_text)
    shift_pressed = False
    selection_start = None
    for item in inputs:
        key = item.keyboard
        released = bool(key.flags & WindowsUnicodeInput.KEYEVENTF_KEYUP)
        if key.flags & WindowsUnicodeInput.KEYEVENTF_UNICODE:
            if not released:
                output.append(chr(key.scan_code))
            continue
        if key.virtual_key == WindowsUnicodeInput.VK_SHIFT:
            shift_pressed = not released
            continue
        if released:
            continue
        if key.virtual_key == WindowsUnicodeInput.VK_RETURN and shift_pressed:
            line_start = "".join(output).rfind("\n") + 1
            current_line = "".join(output[line_start:])
            markdown_prefix = re.match(
                r"\s*(?:(?:\d+[.)]|[-+*>])\s+)", current_line
            )
            output.append("\n")
            if markdown_prefix:
                output.extend(" " * len(markdown_prefix.group(0)))
        elif key.virtual_key == WindowsUnicodeInput.VK_HOME and shift_pressed:
            selection_start = "".join(output).rfind("\n") + 1
        elif key.virtual_key == WindowsUnicodeInput.VK_BACK:
            if selection_start is not None:
                del output[selection_start:]
                selection_start = None
            elif output:
                output.pop()

    raw = "".join(output)
    return raw.encode("utf-16-le", "surrogatepass").decode("utf-16-le")


class ServerStartupTests(unittest.TestCase):
    def test_binding_skips_reverse_dns_because_only_numeric_ip_is_used(self):
        with patch("socket.getfqdn", side_effect=AssertionError("reverse DNS must not run")):
            server = BridgeServer(("127.0.0.1", 0), "test-token", lambda _text: None)
        server.server_close()


class BridgeServerTests(unittest.TestCase):
    def setUp(self):
        self.received = []
        self.enter_presses = 0
        self.config_file = Path(__file__).with_name("test-server-config.json")
        self.config_file.unlink(missing_ok=True)

        def press_enter():
            self.enter_presses += 1

        self.server = BridgeServer(
            ("127.0.0.1", 0),
            "test-token",
            self.received.append,
            enter_callback=press_enter,
            config_file=self.config_file,
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.config_file.unlink(missing_ok=True)

    def test_phone_page_requires_pairing_token(self):
        with self.assertRaises(HTTPError) as error:
            urlopen(self.base_url + "/", timeout=2)
        self.assertEqual(error.exception.code, 403)

    def test_phone_page_embeds_the_same_app_icon(self):
        page = self.server.page.decode("utf-8")
        self.assertIn("data:image/png;base64,", page)
        self.assertNotIn("{{APP_ICON_BASE64}}", page)

    def test_valid_text_is_forwarded_to_input_callback(self):
        payload = json.dumps({"text": "测试语音输入"}).encode("utf-8")
        request = Request(
            self.base_url + "/send?token=test-token",
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urlopen(request, timeout=2) as response:
            result = json.loads(response.read().decode("utf-8"))

        self.assertEqual(result, {"ok": True})
        self.assertEqual(self.received, ["测试语音输入"])

    def test_empty_text_is_rejected(self):
        payload = json.dumps({"text": ""}).encode("utf-8")
        request = Request(
            self.base_url + "/send?token=test-token", data=payload, method="POST"
        )
        with self.assertRaises(HTTPError) as error:
            urlopen(request, timeout=2)
        self.assertEqual(error.exception.code, 400)
        self.assertEqual(self.received, [])

    def test_confirmed_enter_request_presses_computer_enter_once(self):
        request = Request(
            self.base_url + "/enter?token=test-token", data=b"", method="POST"
        )
        with urlopen(request, timeout=2) as response:
            result = json.loads(response.read().decode("utf-8"))

        self.assertEqual(result, {"ok": True})
        self.assertEqual(self.enter_presses, 1)
        self.assertEqual(self.received, [])

    def test_paused_computer_rejects_phone_input(self):
        self.server.accepting_phone_text = False
        payload = json.dumps({"text": "不应输入"}).encode("utf-8")
        request = Request(
            self.base_url + "/send?token=test-token", data=payload, method="POST"
        )
        with self.assertRaises(HTTPError) as error:
            urlopen(request, timeout=2)
        self.assertEqual(error.exception.code, 423)
        self.assertEqual(self.received, [])

    def test_paused_computer_rejects_enter_request(self):
        self.server.accepting_phone_text = False
        request = Request(
            self.base_url + "/enter?token=test-token", data=b"", method="POST"
        )
        with self.assertRaises(HTTPError) as error:
            urlopen(request, timeout=2)

        self.assertEqual(error.exception.code, 423)
        self.assertEqual(self.enter_presses, 0)

    def test_computer_text_is_received_by_phone(self):
        message_id = self.server.queue_for_phone("电脑发给手机")
        url = (
            self.base_url
            + "/receive?token=test-token&after=0&session="
            + self.server.session_id
        )
        with urlopen(url, timeout=2) as response:
            result = json.loads(response.read().decode("utf-8"))

        self.assertEqual(
            result["messages"], [{"id": message_id, "text": "电脑发给手机"}]
        )

    def test_mobile_confirmation_settings_are_stored_by_computer(self):
        with urlopen(
            self.base_url + "/settings?token=test-token", timeout=2
        ) as response:
            defaults = json.loads(response.read().decode("utf-8"))
        self.assertTrue(defaults["confirm_enter"])
        self.assertFalse(defaults["confirm_send"])

        payload = json.dumps(
            {"confirm_enter": False, "confirm_send": True}
        ).encode("utf-8")
        request = Request(
            self.base_url + "/settings?token=test-token",
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urlopen(request, timeout=2) as response:
            saved = json.loads(response.read().decode("utf-8"))

        self.assertFalse(saved["confirm_enter"])
        self.assertTrue(saved["confirm_send"])
        persisted = load_settings(self.config_file)
        self.assertFalse(persisted["confirm_enter"])
        self.assertTrue(persisted["confirm_send"])


class NetworkSelectionTests(unittest.TestCase):
    def setUp(self):
        self.config_file = Path(__file__).with_name("test-network-config.json")
        self.config_file.unlink(missing_ok=True)

    def tearDown(self):
        self.config_file.unlink(missing_ok=True)

    def test_real_lan_address_is_preferred_over_virtual_adapters(self):
        addresses = ["198.18.0.1", "26.136.123.102", "192.168.10.104"]
        self.assertEqual(choose_lan_ip(addresses), "192.168.10.104")

    def test_default_gateway_adapter_wins_over_host_only_virtual_network(self):
        addresses = ["192.168.56.1", "192.168.10.104"]
        self.assertEqual(
            choose_lan_ip(addresses, {"192.168.10.104"}),
            "192.168.10.104",
        )

    def test_first_selected_address_is_persisted(self):
        with patch("app.local_ip", return_value="192.168.10.104"):
            displayed_ip, bind_ip, available = startup_lan_binding(self.config_file)

        self.assertEqual(displayed_ip, "192.168.10.104")
        self.assertEqual(bind_ip, "192.168.10.104")
        self.assertTrue(available)
        self.assertEqual(load_saved_lan_ip(self.config_file), "192.168.10.104")

    def test_restart_reuses_saved_address_without_selecting_another_one(self):
        update_config({"lan_ip": "192.168.10.104"}, self.config_file)
        with patch(
            "app.local_ip",
            side_effect=AssertionError("restart must not select a new address"),
        ), patch(
            "app.active_ipv4_addresses", return_value={"192.168.10.104"}
        ):
            result = startup_lan_binding(self.config_file)

        self.assertEqual(result, ("192.168.10.104", "192.168.10.104", True))

    def test_stale_saved_address_is_not_silently_replaced(self):
        update_config({"lan_ip": "192.168.53.26"}, self.config_file)
        with patch(
            "app.local_ip",
            side_effect=AssertionError("only manual refresh may select a new address"),
        ), patch(
            "app.active_ipv4_addresses", return_value={"192.168.10.104"}
        ):
            result = startup_lan_binding(self.config_file)

        self.assertEqual(result, ("192.168.53.26", "127.0.0.1", False))
        self.assertEqual(load_saved_lan_ip(self.config_file), "192.168.53.26")


class NetworkRebindTests(unittest.TestCase):
    @staticmethod
    def fake_server(ip):
        server = type("FakeServer", (), {})()
        server.server_port = 8765
        server.token = "test-token"
        server.input_callback = lambda _text: None
        server.status_callback = lambda _message: None
        server.enter_callback = lambda: None
        server.config_file = Path("test-config.json")
        server.accepting_phone_text = False
        server._phone_condition = threading.Condition()
        server._phone_messages = [{"id": 1, "text": "待发送"}]
        server._next_message_id = 2
        server.serve_forever = lambda: None
        server.shutdown = lambda: None
        server.server_close = lambda: None
        server.bound_ip = ip
        return server

    def test_manual_refresh_rebinds_and_persists_current_lan_ip(self):
        application = object.__new__(BridgeApp)
        application.closing = False
        application.lan_ip = "192.168.5.6"
        application.server_bound_ip = "192.168.5.6"
        application.token = "test-token"
        application.server = self.fake_server("192.168.5.6")
        application.thread = None
        application.refresh_pairing = Mock()
        application.set_status = Mock()
        replacement = self.fake_server("192.168.5.7")

        with patch("app.local_ip", return_value="192.168.5.7"), patch(
            "app.BridgeServer", return_value=replacement
        ) as create_server, patch("app.update_config") as save_config:
            changed = application._refresh_network_binding()

        self.assertTrue(changed)
        self.assertEqual(application.lan_ip, "192.168.5.7")
        self.assertIs(application.server, replacement)
        self.assertFalse(replacement.accepting_phone_text)
        self.assertEqual(replacement._phone_messages, [{"id": 1, "text": "待发送"}])
        create_server.assert_called_once()
        save_config.assert_called_once_with(
            {"lan_ip": "192.168.5.7"}, application.server.config_file
        )
        application.refresh_pairing.assert_called_once_with()

    def test_manual_refresh_keeps_server_when_lan_ip_is_unchanged(self):
        application = object.__new__(BridgeApp)
        application.closing = False
        application.lan_ip = "192.168.5.7"
        application.server_bound_ip = "192.168.5.7"
        application.server = self.fake_server("192.168.5.7")
        application.set_status = Mock()

        with patch("app.local_ip", return_value="192.168.5.7"), patch(
            "app.BridgeServer"
        ) as create_server:
            changed = application._refresh_network_binding()

        self.assertFalse(changed)
        create_server.assert_not_called()
        application.set_status.assert_called_once_with(
            "当前地址未变化：192.168.5.7"
        )

    def test_temporary_network_loss_does_not_replace_lan_server_with_loopback(self):
        application = object.__new__(BridgeApp)
        application.closing = False
        application.lan_ip = "192.168.10.104"
        application.server_bound_ip = "192.168.10.104"
        application.server = self.fake_server("192.168.10.104")
        application.set_status = Mock()

        with patch("app.local_ip", return_value="127.0.0.1"), patch(
            "app.BridgeServer"
        ) as create_server:
            changed = application._refresh_network_binding()

        self.assertFalse(changed)
        create_server.assert_not_called()
        application.set_status.assert_called_once_with(
            "当前未找到可用局域网，地址保持不变"
        )

    def test_opening_qr_or_restoring_window_cannot_refresh_address(self):
        self.assertNotIn(
            "self._refresh_network_binding()", inspect.getsource(BridgeApp.toggle_qr)
        )
        self.assertNotIn(
            "self._refresh_network_binding()",
            inspect.getsource(BridgeApp.restore_from_tray),
        )


class SingleInstanceTests(unittest.TestCase):
    def test_activation_is_acknowledged_after_existing_window_is_restored(self):
        application = object.__new__(BridgeApp)
        application.activation_event = 11
        application.activation_ack_event = 12
        application.tray_actions = queue.Queue()
        application.restore_from_tray = Mock()
        application.root = Mock()
        application.closing = False
        kernel32 = Mock()
        kernel32.WaitForSingleObject.return_value = 0

        with patch("app.ctypes.windll.kernel32", kernel32):
            application._poll_tray_actions()

        kernel32.ResetEvent.assert_called_once_with(11)
        application.restore_from_tray.assert_called_once_with()
        kernel32.SetEvent.assert_called_once_with(12)

    def test_restore_shows_window_before_leaving_tray_icon_available(self):
        application = object.__new__(BridgeApp)
        application.root = Mock()
        application.root.winfo_id.return_value = 123
        application.tray_icon = Mock()

        with patch("app.ctypes.windll.user32") as user32:
            application.restore_from_tray()

        application.root.deiconify.assert_called_once_with()
        application.root.state.assert_called_once_with("normal")
        application.tray_icon.stop.assert_not_called()
        user32.SetForegroundWindow.assert_called_once_with(123)

    def test_fatal_error_is_recorded_instead_of_silent_exit(self):
        log_file = Path(__file__).with_name("test-crash.log")
        log_file.unlink(missing_ok=True)
        try:
            with patch("app.ctypes.windll.user32") as user32:
                try:
                    raise RuntimeError("startup failed")
                except RuntimeError as error:
                    report_fatal_error(error, log_file)

            contents = log_file.read_text(encoding="utf-8")
            self.assertIn("RuntimeError: startup failed", contents)
            self.assertIn("Traceback", contents)
            user32.MessageBoxW.assert_called_once()
        finally:
            log_file.unlink(missing_ok=True)


class PairingTokenTests(unittest.TestCase):
    def test_token_is_reused_until_pairing_is_rotated(self):
        config_file = Path(__file__).with_name("test-token-config.json")
        try:
            first = load_or_create_token(config_file)
            second = load_or_create_token(config_file)
            rotated = load_or_create_token(config_file, rotate=True)
        finally:
            config_file.unlink(missing_ok=True)

        self.assertEqual(first, second)
        self.assertNotEqual(first, rotated)

    def test_settings_and_character_count_persist_without_replacing_token(self):
        config_file = Path(__file__).with_name("test-settings-config.json")
        try:
            token = load_or_create_token(config_file)
            update_config(
                {
                    "theme": "dark",
                    "character_count": 12,
                    "show_desktop_to_phone": False,
                },
                config_file,
            )
            self.assertEqual(add_character_count(5, config_file), 17)

            settings = load_settings(config_file)
            self.assertEqual(settings["theme"], "dark")
            self.assertEqual(settings["character_count"], 17)
            self.assertTrue(settings["confirm_enter"])
            self.assertFalse(settings["confirm_send"])
            self.assertFalse(settings["show_desktop_to_phone"])
            self.assertEqual(load_or_create_token(config_file), token)
        finally:
            config_file.unlink(missing_ok=True)


class DesktopFeatureTests(unittest.TestCase):
    def test_press_enter_sends_only_enter_down_and_up(self):
        class FakeSendInput:
            def __call__(self, count, inputs, _input_size):
                self.events = [
                    (
                        inputs[index].keyboard.virtual_key,
                        inputs[index].keyboard.flags,
                    )
                    for index in range(count)
                ]
                return count

        injector = object.__new__(WindowsUnicodeInput)
        injector.user32 = type("FakeUser32", (), {})()
        injector.user32.GetForegroundWindow = lambda: 1
        injector.user32.SendInput = FakeSendInput()

        injector.press_enter()

        self.assertEqual(
            injector.user32.SendInput.events,
            [
                (WindowsUnicodeInput.VK_RETURN, 0),
                (
                    WindowsUnicodeInput.VK_RETURN,
                    WindowsUnicodeInput.KEYEVENTF_KEYUP,
                ),
            ],
        )

    def test_all_text_uses_clipboard_path_without_replacing_control(self):
        injector = object.__new__(WindowsUnicodeInput)
        for source in ("一句话", "甲\n乙", "甲\r\n乙", "甲\r乙"):
            with self.subTest(source=repr(source)), patch.object(
                injector, "_paste_with_clipboard"
            ) as paste, patch.object(
                injector, "_insert_into_native_edit"
            ) as replace_selection:
                injector(source)
            paste.assert_called_once_with(source)
            replace_selection.assert_not_called()

    def test_clipboard_text_is_published_without_normalizing_formatting(self):
        injector = object.__new__(WindowsUnicodeInput)
        source = "甲  \r\n\n\t乙 🌟"
        with patch.object(injector, "_open_clipboard"), patch(
            "app.win32clipboard.EmptyClipboard"
        ), patch(
            "app.win32clipboard.SetClipboardText"
        ) as set_clipboard_text, patch(
            "app.win32clipboard.CloseClipboard"
        ):
            injector._set_clipboard_text(1, source)

        set_clipboard_text.assert_called_once_with(
            source, win32clipboard.CF_UNICODETEXT
        )

    def test_unicode_event_builder_preserves_utf16_units(self):
        inputs = WindowsUnicodeInput.build_inputs("A中🙂")

        self.assertEqual(ctypes.sizeof(WindowsUnicodeInput.Input), 40)
        self.assertEqual(
            [inputs[index].keyboard.scan_code for index in range(0, len(inputs), 2)],
            [0x0041, 0x4E2D, 0xD83D, 0xDE42],
        )
        for index in range(0, len(inputs), 2):
            self.assertEqual(inputs[index].keyboard.flags, 0x0004)
            self.assertEqual(inputs[index + 1].keyboard.flags, 0x0006)

    def test_unicode_input_preserves_line_breaks_spaces_and_tabs(self):
        source = "甲  \r\n\n\t乙"
        inputs = WindowsUnicodeInput.build_inputs(source)

        self.assertEqual(len(inputs), 38)
        for start in (6, 20):
            self.assertEqual(
                [inputs[index].keyboard.virtual_key for index in range(start, start + 14)],
                [0x10, 0x0D, 0x0D, 0x10, 0, 0, 0x10, 0x24, 0x24, 0x24, 0x24, 0x10, 0x08, 0x08],
            )
            self.assertEqual(
                [inputs[index].keyboard.flags for index in range(start, start + 14)],
                [0, 0, 0x0002, 0x0002, 0x0004, 0x0006, 0, 0x0001, 0x0003, 0x0001, 0x0003, 0x0002, 0, 0x0002],
            )
        self.assertEqual(replay_in_smart_markdown_editor(inputs), "甲  \n\n\t乙")

    def test_unicode_input_preserves_markdown_and_uncommon_unicode_exactly(self):
        cases = (
            "新的问题出现了：\n"
            "1. 急需新袜子架，原有太小。\n"
            "2. 课桌扩展空间（侧边袋）\n"
            "3. new手提袋（转移）\n"
            "4. 🌟🌟🌟文言文教辅！\n"
            "5. 买一堆书\n"
            "6. 买一堆笔记本（原）\n"
            "7. 错题本",
            "# 标题\n\n- [ ] 待办\n- [x] 完成\n  - 嵌套项\n\n"
            "> 引用第一行\n> 引用第二行\n\n"
            "```python\nprint('原样  保留')\n```\n\n"
            "| 列 A | 列 B |\n| --- | :---: |\n| `x` | **y** |",
            "前导与尾随空格  \n\tTab\t内容\n\n"
            "𠮷野家 · 龘 · e\u0301 · 👨‍👩‍👧‍👦 · \u00a0NBSP\u00a0 · 中文标点「」",
        )
        for source in cases:
            with self.subTest(source=source[:20]):
                batches = WindowsUnicodeInput.build_input_batches(source)
                self.assertTrue(
                    all(
                        len(batch) <= WindowsUnicodeInput.MAX_EVENTS_PER_BATCH
                        for batch in batches
                    )
                )
                events = [item for batch in batches for item in batch]
                actual = replay_in_smart_markdown_editor(events)
                self.assertEqual(actual, source)

    def test_windows_title_bar_color_uses_colorref_byte_order(self):
        self.assertEqual(windows_colorref("#112233"), 0x00332211)

    def test_packaged_autostart_uses_only_the_executable(self):
        command = startup_command(
            frozen=True,
            executable=Path(r"C:\Tools\VoiceBridge.exe"),
            script=Path(r"C:\ignored\app.py"),
        )
        self.assertEqual(command, '"C:\\Tools\\VoiceBridge.exe"')

    def test_source_autostart_uses_pythonw_and_script(self):
        command = startup_command(
            frozen=False,
            executable=Path(r"C:\Python\python.exe"),
            script=Path(r"C:\VoiceBridge\app.py"),
        )
        self.assertEqual(
            command,
            '"C:\\Python\\pythonw.exe" "C:\\VoiceBridge\\app.py"',
        )

    def test_mobile_page_contains_requested_actions_confirmation_and_themes(self):
        page = WEB_PAGE.read_text(encoding="utf-8")
        self.assertIn('class="primary-actions"', page)
        self.assertIn('id="send"', page)
        self.assertIn('id="enter"', page)
        self.assertIn('id="settings-toggle"', page)
        self.assertIn('id="confirm-enter"', page)
        self.assertIn('id="confirm-send"', page)
        self.assertIn("'/settings?token='", page)
        self.assertIn("confirmationSettings.confirm_send", page)
        self.assertIn("confirmationSettings.confirm_enter", page)
        self.assertIn("确认吗？", page)
        self.assertIn("'/enter?token='", page)
        self.assertEqual(page.count('class="icon-button"'), 3)
        self.assertIn('id="undo"', page)
        self.assertIn("clearedSnapshot", page)
        self.assertIn("sentSnapshot", page)
        self.assertIn("text.blur();", page)
        self.assertIn('<option value="system">跟随系统</option>', page)
        self.assertIn('<option value="light">浅色</option>', page)
        self.assertIn('<option value="dark">深色</option>', page)
        self.assertIn("prefers-color-scheme: dark", page)
        self.assertIn("{{APP_ICON_BASE64}}", page)

    def test_desktop_ui_uses_mobile_style_controls_and_collapsed_transfer_panel(self):
        source = Path(__file__).with_name("app.py").read_text(encoding="utf-8")
        self.assertIn("class AnimatedButton(tk.Canvas)", source)
        self.assertIn('self.state = "normal"', source)
        self.assertIn('self.state = "hover"', source)
        self.assertIn('self.state = "pressed"', source)
        self.assertIn("class ToggleSwitch(tk.Canvas)", source)
        self.assertIn("self.transfer_expanded = False", source)
        self.assertIn('"show_desktop_to_phone": True', source)
        self.assertNotIn("ttk.Button(", source)
        for palette in THEMES.values():
            for key in (
                "primary_hover",
                "primary_pressed",
                "secondary_hover",
                "secondary_pressed",
            ):
                self.assertIn(key, palette)


if __name__ == "__main__":
    unittest.main()
