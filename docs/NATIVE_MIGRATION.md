# Windows Native Migration

## Recovery points

| Stage | Git recovery point | Notes |
|---|---|---|
| Committed Python source | `5e37383710ea1e7a26552ffd15c2e7d79080dac7` | Original repository commit. |
| Supplied working tree baseline | `47e6433` | Hash-identical copy of the dirty Python working tree; 61 tests passed. |

The Python implementation and root resources remain unchanged. Native sources
live only under `native/`.

## Verified build dependencies

- Visual Studio Community 2022 17.14.16
- MSBuild 17.14.23
- MSVC 14.44.35207 (x64 desktop workload)
- Windows SDK 10.0.26100.0, including `cppwinrt.exe`
- Microsoft Windows App SDK `1.8.260804001` (fixed NuGet version)
- Microsoft Windows C++/WinRT `2.0.250303.1` (fixed NuGet version)

The machine has no .NET SDK. The native C++ project does not require one.

## Functional correspondence

| Existing behavior | Python evidence | Native owner | Acceptance |
|---|---|---|---|
| Persistent compatible settings | `load_settings`, `update_config`, `%LOCALAPPDATA%/VoiceBridge/config.json` | `SettingsStore` | Existing JSON keys and values load without migration or loss. |
| Persistent/rotatable 256-bit token | `load_or_create_token` | `SettingsStore` | Reuse valid token; rotation invalidates old URL. |
| LAN adapter selection and saved address | `startup_lan_binding`, `current_lan_ip` | `NetworkManager` | Gateway-backed private IPv4 wins; saved address is not silently replaced. |
| Local HTTP page and token authorization | `BridgeServer`, `BridgeHandler` | `BridgeServer` | `/`, `/send`, `/enter`, `/receive`, `/ack`, `/settings` match status and JSON behavior. |
| 100 KB request limit and raw text preservation | `MAX_TEXT_BYTES`, server tests | `BridgeServer` | Reject empty/oversize/invalid payload; preserve UTF-8 text exactly. |
| Phone to active Windows input | `WindowsUnicodeInput` | `InputInjector` | Require a real editable caret, publish `CF_UNICODETEXT`, then send one Ctrl+V. |
| Remote Enter | `press_enter` | `InputInjector` | Require editable caret; one key-down and one key-up only. |
| Computer to phone queue | `queue_for_phone`, `messages_for_phone` | `BridgeServer` | In-memory maximum 50, long poll, session reset and acknowledgement removal. |
| Pause phone input | `toggle_receiving` | `BridgeServer`, `MainWindow` | Both `/send` and `/enter` reject while paused. |
| Desktop-to-phone editor and shortcut | `send_to_phone`, `_handle_enter` | `MainWindow` | Enter/Ctrl+Enter mode matches settings and clears on successful queue. |
| Single instance and activation acknowledgement | `acquire_single_instance` | `SingleInstance` | Second launch restores existing window or shows legacy-instance guidance. |
| Always-available tray icon | `_ensure_tray_icon` | `TrayIcon` | Show and Exit menu; icon remains while main window is visible or hidden. |
| Close choice | `request_close`, `show_close_dialog` | `MainWindow` | Ask, tray, and direct-exit settings all work. |
| Autostart | `set_autostart` | `SettingsStore` | HKCU Run value equals quoted native executable and is reversible. |
| Light/dark theme | `THEMES`, `toggle_theme` | XAML resources, Composition | Exact palette; persistent selection; transform/opacity/color animation. |
| Main, QR, settings, close interfaces | `_build_ui`, `toggle_qr`, `open_settings`, `show_close_dialog` | WinUI windows/dialogs | Text, controls, dimensions, spacing, colors, icons and states match references. |
| Transfer reveal animation | `toggle_transfer_panel` | Composition | 200 ms smoothstep visual transform/opacity; no per-frame control rebuild. |
| High DPI and title bar colors | `enable_dpi_awareness`, `_apply_window_chrome` | WinUI/AppWindow | Per-monitor DPI; light/dark caption and rounded system chrome. |
| Fatal diagnostics | `report_fatal_error` | `Diagnostics` | Append UTF-8 `%LOCALAPPDATA%/VoiceBridge/crash.log`; visible fatal dialog. |
| Mobile interface | `mobile.html` | copied web asset | Byte-identical page, including confirmations, theme, undo, copy and polling. |

## Visual baseline

The retained TEST10 reference captures are 648 px wide at the recorded test
scale. Closed windows are 540 px high and expanded windows are 763 px high.
Light/dark closed, expanded, opening and closing captures are retained in the
original workspace and are not modified. Final visual verification compares
native screenshots against these states at the same DPI.
