#pragma once
#include "NativeApp.h"
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Graphics.Imaging.h>

inline IAsyncAction save_preview(FrameworkElement element, vb::fs::path destination) {
    Imaging::RenderTargetBitmap bitmap;
    co_await bitmap.RenderAsync(element);
    auto pixels = co_await bitmap.GetPixelsAsync();
    auto folder = co_await Windows::Storage::StorageFolder::GetFolderFromPathAsync(destination.parent_path().wstring());
    auto file = co_await folder.CreateFileAsync(destination.filename().wstring(), Windows::Storage::CreationCollisionOption::ReplaceExisting);
    auto stream = co_await file.OpenAsync(Windows::Storage::FileAccessMode::ReadWrite);
    auto encoder = co_await Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(), stream);
    auto reader = Windows::Storage::Streams::DataReader::FromBuffer(pixels);
    com_array<uint8_t> bytes(pixels.Length()); reader.ReadBytes(bytes);
    encoder.SetPixelData(Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8, Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied,
        bitmap.PixelWidth(), bitmap.PixelHeight(), 96, 96, bytes);
    co_await encoder.FlushAsync();
}
inline void invoke_button(Button const& button) {
    auto peer = Automation::Peers::ButtonAutomationPeer(button);
    peer.GetPattern(Automation::Peers::PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
}
inline Button find_button(DependencyObject const& parent, std::wstring const& caption) {
    if (auto button = parent.try_as<Button>()) {
        if (auto text = button.Content().try_as<IPropertyValue>(); text && text.Type() == PropertyType::String && text.GetString() == caption) return button;
    }
    for (int i = 0; i < VisualTreeHelper::GetChildrenCount(parent); ++i) {
        if (auto found = find_button(VisualTreeHelper::GetChild(parent, i), caption)) return found;
    }
    return nullptr;
}
inline fire_and_forget App::run_ui_tests() {
    auto lifetime = get_strong(); apartment_context ui;
    vb::json checks = vb::json::array();
    vb::json window_metrics = vb::json::object();
    auto output = [&] {
        auto failures = std::count_if(checks.begin(), checks.end(), [](auto const& item) { return item["passed"] == false; });
        ui_test_exit_code = failures ? 1 : 0;
        std::ofstream file(ui_test_report, std::ios::binary);
        file << vb::json{{"passed", checks.size() - failures}, {"failed", failures}, {"checks", checks}}.dump(2);
        std::ofstream metrics(ui_test_report.parent_path() / "native-metrics.json"); metrics << window_metrics.dump(2);
    };
    auto check = [&](bool passed, char const* intent) { checks.push_back({{"test", intent}, {"passed", passed}}); output(); };
    auto http = [&](std::string route, std::optional<vb::json> body = {}) -> IAsyncOperation<hstring> {
        auto port = bridge->port; auto token = config->token();
        co_await resume_background();
        httplib::Client client("127.0.0.1", port); client.set_read_timeout(3);
        auto path = route + "?token=" + token;
        auto result = body ? client.Post(path, body->dump(), "application/json") : client.Get(path);
        if (!result) throw std::runtime_error("UI integration HTTP failed");
        co_return to_hstring(vb::json{{"status", result->status}, {"body", vb::json::parse(result->body)}}.dump());
    };
    try {
        vb::fs::create_directories(ui_test_report.parent_path());
        for (int tries = 0; !bridge && tries < 100; ++tries) { co_await resume_after(100ms); co_await ui; }
        if (!bridge) throw std::runtime_error("Native service did not finish startup");
        co_await resume_after(300ms); co_await ui;
        root.UpdateLayout();
        check(IsWindowVisible(window_handle(window)) && root.ActualWidth() >= 520 && root.ActualHeight() >= 330, "actual WinUI window is visible and laid out");
        check(find<Button>(L"QrButton").IsEnabled() && find<TextBlock>(L"StatusText").Text() == L"等待手机连接", "window is ready only after its real native service starts");
        check(find<Grid>(L"TransferContent").Visibility() == Visibility::Collapsed, "startup preserves collapsed transfer panel");
        window_metrics = vb::json{{"dpi", ui_dpi_scale * 96}, {"window_client", {root.ActualWidth(), root.ActualHeight()}},
            {"connection_height", find<Border>(L"ConnectionCard").ActualHeight()}, {"transfer_height", find<Border>(L"TransferCard").ActualHeight()},
            {"status_height", find<Border>(L"StatusCard").ActualHeight()}};
        co_await save_preview(window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "light-closed.png");
        invoke_button(find<Button>(L"TransferButton"));
        co_await resume_after(400ms); co_await ui;
        root.UpdateLayout();
        auto input = find<TextBox>(L"DesktopText");
        check(expanded && !animating && input.ActualHeight() >= 80 && find<Grid>(L"TransferContent").IsHitTestVisible(), "expand animation ends with an editable input and enabled hit testing");
        auto footer = find<Border>(L"StatusCard"); auto footer_position = footer.TransformToVisual(root).TransformPoint({0, 0});
        check(footer_position.Y + footer.ActualHeight() <= root.ActualHeight(), "expanded footer stays inside the visible client area");
        input.Text(L"原生界面验收  😀𠮷 **Markdown**");
        invoke_button(find<Button>(L"SendButton"));
        co_await resume_after(100ms); co_await ui;
        auto response = vb::json::parse(to_string(co_await http("/receive")));
        check(input.Text().empty() && response["body"]["messages"].size() == 1 && response["body"]["messages"][0]["text"] == "原生界面验收  😀𠮷 **Markdown**", "send button delivers its real text through the phone HTTP protocol and clears only after queueing");
        auto session = response["body"]["session"];
        co_await http("/ack", vb::json{{"session", session}, {"through", response["body"]["messages"][0]["id"]}});
        invoke_button(find<Button>(L"PauseButton")); co_await resume_after(100ms); co_await ui;
        response = vb::json::parse(to_string(co_await http("/send", vb::json{{"text", "must not inject"}})));
        check(response["status"] == 423, "pause toolbar action blocks native HTTP injection");
        invoke_button(find<Button>(L"PauseButton")); co_await resume_after(100ms); co_await ui;
        check(bridge->accepting, "resume toolbar action restores receiving");
        // Let the launcher/approval surface finish activation before taking test focus.
        co_await resume_after(1500ms); co_await ui;
        auto focus_test_input = [&] {
            auto foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
            auto current_thread = GetCurrentThreadId();
            bool attached = foreground_thread && foreground_thread != current_thread && AttachThreadInput(current_thread, foreground_thread, TRUE);
            restore(); input.Focus(FocusState::Programmatic);
            if (attached) AttachThreadInput(current_thread, foreground_thread, FALSE);
        };
        input.Text(L""); focus_test_input();
        for (int attempt = 0; attempt < 5 && GetForegroundWindow() != window_handle(window); ++attempt) {
            co_await resume_after(250ms); co_await ui; focus_test_input();
        }
        co_await resume_after(150ms); co_await ui;
        response = vb::json::parse(to_string(co_await http("/send", vb::json{{"text", "手机实测  😀𠮷 **Markdown**"}})));
        co_await resume_after(150ms); co_await ui;
        check(response["status"] == 200 && input.Text() == L"手机实测  😀𠮷 **Markdown**", "phone HTTP send performs real clipboard paste into the guarded native test input");
        checks.back()["response"] = response; checks.back()["text"] = to_string(input.Text());
        checks.back()["foreground"] = reinterpret_cast<int64_t>(GetForegroundWindow());
        checks.back()["expected_window"] = reinterpret_cast<int64_t>(window_handle(window));
        response = vb::json::parse(to_string(co_await http("/enter", vb::json::object())));
        co_await resume_after(150ms); co_await ui;
        check(response["status"] == 200 && to_string(input.Text()).ends_with('\r'), "remote Enter produces a real newline in the guarded test input");
        checks.back()["response"] = response; checks.back()["text"] = to_string(input.Text());
        if (GetForegroundWindow() != window_handle(window)) throw std::runtime_error("Test window lost keyboard focus; no shortcut keys sent");
        vb::send_keys({{WORD(VK_CONTROL), false}, {WORD(VK_RETURN), false}, {WORD(VK_RETURN), true}, {WORD(VK_CONTROL), true}});
        co_await resume_after(150ms); co_await ui;
        check(input.Text().empty(), "Ctrl+Enter sends instead of adding a newline in the default shortcut mode");
        response = vb::json::parse(to_string(co_await http("/receive")));
        auto last_id = response["body"]["messages"].back()["id"];
        co_await http("/ack", vb::json{{"session", session}, {"through", last_id}});
        config->update({{"send_mode", "enter"}}); apply_settings(); input.Text(L"快捷键验收"); input.Select(input.Text().size(), 0);
        if (GetForegroundWindow() != window_handle(window)) throw std::runtime_error("Test window lost keyboard focus; no shortcut keys sent");
        vb::send_keys({{WORD(VK_CONTROL), false}, {WORD(VK_RETURN), false}, {WORD(VK_RETURN), true}, {WORD(VK_CONTROL), true}});
        co_await resume_after(150ms); co_await ui;
        check(input.Text() == L"快捷键验收\r", "Ctrl+Enter inserts a newline when Enter is configured to send");
        if (GetForegroundWindow() != window_handle(window)) throw std::runtime_error("Test window lost keyboard focus; no shortcut keys sent");
        vb::send_keys({{WORD(VK_RETURN), false}, {WORD(VK_RETURN), true}});
        co_await resume_after(150ms); co_await ui;
        check(input.Text().empty(), "Enter sends in the alternate shortcut mode");
        checks.back()["text"] = to_string(input.Text());
        response = vb::json::parse(to_string(co_await http("/receive")));
        co_await http("/ack", vb::json{{"session", session}, {"through", response["body"]["messages"].back()["id"]}});
        config->update({{"send_mode", "ctrl_enter"}}); apply_settings();
        input.Text(L""); find<Button>(L"ThemeButton").Focus(FocusState::Programmatic);
        response = vb::json::parse(to_string(co_await http("/send", vb::json{{"text", "must not paste on toolbar"}})));
        check(response["status"] == 409 && input.Text().empty(), "real UI Automation focus detection rejects a toolbar instead of an input");
        co_await save_preview(window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "light-open.png");
        invoke_button(find<Button>(L"ThemeButton")); co_await resume_after(350ms); co_await ui;
        check(dark && config->settings()["theme"] == "dark" && window.Content().as<FrameworkElement>().RequestedTheme() == ElementTheme::Dark, "theme toolbar updates and persists the active WinUI theme");
        co_await save_preview(window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "dark-open.png");
        client_size(window, 640, 660); co_await resume_after(150ms); co_await ui;
        auto resized_height = root.ActualHeight();
        invoke_button(find<Button>(L"TransferButton")); co_await resume_after(350ms); co_await ui;
        check(!expanded && !animating && std::abs(root.ActualHeight() - collapsed_height) < 2, "collapse restores its previous window height");
        co_await save_preview(window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "dark-closed.png");
        invoke_button(find<Button>(L"TransferButton")); co_await resume_after(350ms); co_await ui;
        check(std::abs(root.ActualHeight() - resized_height) < 2, "reopening preserves user-resized input height");
        invoke_button(find<Button>(L"SettingsButton")); co_await resume_after(250ms); co_await ui;
        check(settings_window && setting_switches.size() == 5 && IsWindowVisible(window_handle(settings_window)), "settings window opens with all five original switches");
        auto settings_root = settings_window.Content().as<FrameworkElement>();
        window_metrics["settings_client"] = {settings_root.ActualWidth() * ui_dpi_scale, settings_root.ActualHeight() * ui_dpi_scale};
        window_metrics["font_lines"] = {font_line(9), font_line(10), font_line(11, true), font_line(22, true)};
        auto save_bounds = find_button(settings_window.Content(), L"保存设置").TransformToVisual(settings_root).TransformPoint({0, 42});
        check(save_bounds.Y <= settings_root.ActualHeight(), "settings save action is visible without clipping or scrolling at the initial size");
        auto count_toggle = setting_switches.at("show_char_count");
        Automation::Peers::ToggleSwitchAutomationPeer(count_toggle).GetPattern(Automation::Peers::PatternInterface::Toggle).as<Automation::Provider::IToggleProvider>().Toggle();
        check(!count_toggle.IsOn() && count_toggle.ActualWidth() == 42 && count_toggle.ActualHeight() == 24, "original-sized switch keeps native toggle and accessibility behavior");
        co_await save_preview(settings_window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "settings-dark.png");
        setting_switches.at("confirm_send").IsOn(true); setting_switches.at("confirm_enter").IsOn(false);
        draft_send = "enter"; draft_close = "tray";
        auto save = find_button(settings_window.Content(), L"保存设置");
        if (!save) throw std::runtime_error("Save settings button missing");
        invoke_button(save); co_await resume_after(200ms); co_await ui;
        response = vb::json::parse(to_string(co_await http("/settings")));
        check(!settings_window && response["body"]["confirm_send"] == true && response["body"]["confirm_enter"] == false && config->settings()["send_mode"] == "enter", "save settings commits choices and synchronizes phone confirmations");
        check(find<TextBlock>(L"CountText").Visibility() == Visibility::Collapsed, "character counter visibility follows the saved switch");
        invoke_button(find<Button>(L"QrButton")); co_await resume_after(200ms); co_await ui;
        check(qr_window && qr_canvas.Children().Size() > 200 && to_string(qr_address.Text()) == bridge->url(), "pairing window draws an actual QR and the current authenticated address");
        auto pairing_root = qr_window.Content().as<FrameworkElement>();
        window_metrics["pairing_client"] = {pairing_root.ActualWidth() * ui_dpi_scale, pairing_root.ActualHeight() * ui_dpi_scale};
        auto rotate_bounds = find_button(qr_window.Content(), L"重新配对").TransformToVisual(pairing_root).TransformPoint({0, 42});
        check(rotate_bounds.Y <= pairing_root.ActualHeight(), "pairing actions stay inside the non-resizable window");
        co_await save_preview(qr_window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "pairing-dark.png");
        qr_window.Close();
        request_close(); co_await resume_after(200ms); co_await ui;
        check(!IsWindowVisible(window_handle(window)), "configured close-to-tray hides the window and keeps its server running");
        response = vb::json::parse(to_string(co_await http("/settings")));
        check(response["status"] == 200, "phone service remains available while minimized to tray");
        SetEvent(activation_event); co_await resume_after(300ms); co_await ui;
        check(IsWindowVisible(window_handle(window)) && WaitForSingleObject(activation_ack, 0) == WAIT_OBJECT_0, "single-instance activation restores and acknowledges the existing window");
        config->update({{"close_action", "ask"}}); request_close(); co_await resume_after(200ms); co_await ui;
        check(close_window && find_button(close_window.Content(), L"最小化到托盘") && find_button(close_window.Content(), L"直接关闭"), "close prompt offers both original actions");
        auto close_root = close_window.Content().as<FrameworkElement>();
        window_metrics["close_client"] = {close_root.ActualWidth() * ui_dpi_scale, close_root.ActualHeight() * ui_dpi_scale};
        check(!IsWindowEnabled(window_handle(window)), "close question is modal like the original and prevents conflicting main-window actions");
        co_await save_preview(close_window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "close-dark.png");
        close_window.Close();
    } catch (hresult_error const& error) { checks.push_back({{"test", "UI runtime exception"}, {"passed", false}, {"error", to_string(error.message())}}); }
      catch (std::exception const& error) { checks.push_back({{"test", "UI integration exception"}, {"passed", false}, {"error", error.what()}}); }
    output(); exit_app();
}
