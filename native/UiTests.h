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
inline Button caption_button(Window const& window, wchar_t const* name) { return window.Content().as<Grid>().FindName(name).as<Button>(); }
inline FrameworkElement find_named(DependencyObject const& parent, wchar_t const* name) {
    if (auto element = parent.try_as<FrameworkElement>(); element && element.Name() == name) return element;
    for (int i = 0; i < VisualTreeHelper::GetChildrenCount(parent); ++i)
        if (auto found = find_named(VisualTreeHelper::GetChild(parent, i), name)) return found;
    return nullptr;
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
        check(std::abs(root.ActualHeight() - collapsed_height) < 2 && std::abs(find<Border>(L"TransferCard").ActualHeight() - find<Border>(L"ConnectionCard").ActualHeight()) < 2, "custom title bar preserves the checkpoint content height and equal collapsed cards");
        check(!root.FindName(L"PauseButton") && bridge->accepting, "removed pause control leaves phone receiving enabled");
        auto presenter = window.AppWindow().Presenter().as<windowing::OverlappedPresenter>();
        check(!presenter.HasTitleBar() && caption_button(window, L"CaptionClose").ActualWidth() == 30, "title bar and caption buttons are drawn by Voice Bridge instead of the system");
        auto regions = Microsoft::UI::Input::InputNonClientPointerSource::GetForWindowId(window.AppWindow().Id()).GetRegionRects(Microsoft::UI::Input::NonClientRegionKind::Caption);
        check(regions.size() == 1 && regions[0].Height == caption_height(window_handle(window)) && regions[0].Width < window.AppWindow().ClientSize().Width - 100, "caption drag region is DPI-scaled and excludes clickable window controls");
        invoke_button(caption_button(window, L"CaptionMinimize")); co_await resume_after(150ms); co_await ui;
        check(IsIconic(window_handle(window)), "custom minimize button minimizes the real window");
        restore(); co_await resume_after(400ms); co_await ui;
        for (int attempt = 0; attempt < 20 && (IsIconic(window_handle(window)) || content_client_rect(window).bottom <= 0); ++attempt) {
            restore(); co_await resume_after(100ms); co_await ui;
        }
        if (IsIconic(window_handle(window)) || content_client_rect(window).bottom <= 0) throw std::runtime_error("Window did not finish restoring; cannot verify caption interactions");
        check(std::abs(root.ActualHeight() - collapsed_height) < 2, "minimize and restore cannot change the checkpoint content height");
        window_metrics = vb::json{{"dpi", ui_dpi_scale * 96}, {"window_client", {root.ActualWidth(), root.ActualHeight()}},
            {"connection_height", find<Border>(L"ConnectionCard").ActualHeight()}, {"transfer_height", find<Border>(L"TransferCard").ActualHeight()},
            {"status_height", find<Border>(L"StatusCard").ActualHeight()}};
        co_await save_preview(window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "light-closed.png");
        open_settings();
        DWORD settings_cloaked = 0;
        check_hresult(DwmGetWindowAttribute(window_handle(settings_window), DWMWA_CLOAKED, &settings_cloaked, sizeof(settings_cloaked)));
        check(settings_cloaked != 0, "settings is concealed while its first XAML frame is not ready");
        BYTE settings_alpha{}; DWORD alpha_flags{}; COLORREF alpha_color{};
        auto read_settings_alpha = [&] {
            check_bool(GetLayeredWindowAttributes(window_handle(settings_window), &alpha_color, &settings_alpha, &alpha_flags));
            return settings_alpha;
        };
        check(read_settings_alpha() == 0, "settings starts transparent to prevent an unrendered frame flashing");
        bool saw_fade_in = false, monotonic_fade_in = true; BYTE previous_alpha = 0;
        for (int sample = 0; sample < 60; ++sample) {
            co_await resume_after(16ms); co_await ui;
            auto alpha = read_settings_alpha();
            saw_fade_in |= alpha > 0 && alpha < 255;
            monotonic_fade_in &= alpha >= previous_alpha; previous_alpha = alpha;
            if (alpha == 255) break;
        }
        check(saw_fade_in && monotonic_fade_in && settings_alpha == 255, "settings fades progressively to full opacity instead of appearing instantly");
        check_hresult(DwmGetWindowAttribute(window_handle(settings_window), DWMWA_CLOAKED, &settings_cloaked, sizeof(settings_cloaked)));
        check(settings_cloaked == 0, "settings becomes visible after rendering and committing its first frame");
        check(IsWindowVisible(window_handle(settings_window)), "ready settings window remains visible after fading in");
        auto screen_dc = GetDC(nullptr); auto backing_dc = CreateCompatibleDC(screen_dc);
        auto backing_pixel = CreateCompatibleBitmap(screen_dc, 1, 1); auto previous_bitmap = SelectObject(backing_dc, backing_pixel);
        SendMessageW(window_handle(settings_window), WM_ERASEBKGND, reinterpret_cast<WPARAM>(backing_dc), 0);
        auto backing_color = GetPixel(backing_dc, 0, 0); auto expected_background = palette(L"RootBrush").as<SolidColorBrush>().Color();
        SelectObject(backing_dc, previous_bitmap); DeleteObject(backing_pixel); DeleteDC(backing_dc); ReleaseDC(nullptr, screen_dc);
        check(backing_color == RGB(expected_background.R, expected_background.G, expected_background.B), "native settings backing surface uses the theme color rather than black");
        auto settings_layout = find_named(settings_window.Content(), L"SettingsLayout");
        auto settings_actions = find_named(settings_window.Content(), L"SettingsActions");
        auto settings_scroll = find_named(settings_window.Content(), L"SettingsScroll").as<ScrollViewer>();
        auto first_card = find_named(settings_window.Content(), L"SettingsInterfaceCard");
        auto second_card = find_named(settings_window.Content(), L"SettingsBehaviorCard");
        auto actions_position = settings_actions.TransformToVisual(settings_layout).TransformPoint({0, 0});
        auto first_position = first_card.TransformToVisual(settings_layout).TransformPoint({0, 0});
        auto second_position = second_card.TransformToVisual(settings_layout).TransformPoint({0, 0});
        auto bottom_gap = settings_layout.ActualHeight() - actions_position.Y - settings_actions.ActualHeight();
        check(std::abs(bottom_gap - 18) < 1, "settings footer keeps the full 18 pixel bottom inset");
        check(settings_scroll.ScrollableHeight() < 2, "settings initial height fits its measured content without hidden options");
        check(std::abs(first_card.ActualWidth() - second_card.ActualWidth()) < 1 && first_card.ActualWidth() > 450, "settings cards share a full-width aligned layout");
        window_metrics["settings_layout"] = {{"body_height", settings_layout.ActualHeight()}, {"bottom_gap", bottom_gap}, {"scrollable_height", settings_scroll.ScrollableHeight()}, {"card_width", first_card.ActualWidth()}, {"first_left", first_position.X}, {"first_right", settings_layout.ActualWidth() - first_position.X - first_card.ActualWidth()}, {"second_left", second_position.X}, {"second_right", settings_layout.ActualWidth() - second_position.X - second_card.ActualWidth()}};
        co_await save_preview(settings_window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "settings-layout-light.png");
        check(std::abs(first_position.X - 18) < 1 && std::abs(settings_layout.ActualWidth() - first_position.X - first_card.ActualWidth() - 18) < 1, "settings cards preserve equal left and right insets");
        auto settings_hidden_on_close = std::make_shared<bool>(false);
        settings_window.Closed([settings_hidden_on_close, hwnd = window_handle(settings_window)](auto&&, auto&&) { *settings_hidden_on_close = !IsWindowVisible(hwnd); });
        settings_window.Close();
        check(*settings_hidden_on_close, "programmatic settings close hides the HWND before XAML destruction");
        for (int cycle = 0; cycle < 2; ++cycle) {
            change_theme(); open_settings(); co_await resume_after(400ms); co_await ui;
            check_hresult(DwmGetWindowAttribute(window_handle(settings_window), DWMWA_CLOAKED, &settings_cloaked, sizeof(settings_cloaked)));
            check(settings_cloaked == 0, "reopening settings in either theme reveals a completed frame");
            if (cycle == 0) co_await save_preview(settings_window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "settings-ready-dark.png");
            invoke_button(cycle == 0 ? find_button(settings_window.Content(), L"取消") : caption_button(settings_window, L"CaptionClose"));
            co_await resume_after(80ms); co_await ui;
            check(settings_window && IsWindowVisible(window_handle(settings_window)) && settings_window.Content() && read_settings_alpha() > 0 && settings_alpha < 255, "settings fades its live window before hiding or destroying the surface");
            co_await resume_after(250ms); co_await ui;
            check(!settings_window, "cancel and caption close both complete without leaving a hidden settings window");
        }
        open_settings();
        for (int sample = 0; sample < 60 && read_settings_alpha() == 0; ++sample) { co_await resume_after(16ms); co_await ui; }
        auto interrupted_alpha = read_settings_alpha();
        check(interrupted_alpha > 0 && interrupted_alpha < 255, "rapid close test interrupts an active fade-in");
        close_settings(); close_settings(); open_settings();
        co_await resume_after(60ms); co_await ui;
        check(settings_window && read_settings_alpha() <= interrupted_alpha, "closing during fade-in reverses from the current opacity without flashing or reopening");
        co_await resume_after(250ms); co_await ui;
        check(!settings_window, "repeated close requests finish without a stranded settings window");
        restore(); co_await resume_after(200ms); co_await ui;
        invoke_button(find<Button>(L"TransferButton"));
        co_await resume_after(400ms); co_await ui;
        root.UpdateLayout();
        auto input = find<TextBox>(L"DesktopText");
        check(expanded && !animating && input.ActualHeight() >= 80 && find<Grid>(L"TransferContent").IsHitTestVisible(), "expand animation ends with an editable input and enabled hit testing");
        auto before_maximize = root.ActualHeight();
        invoke_button(caption_button(window, L"CaptionMaximize")); co_await resume_after(200ms); co_await ui;
        check(IsZoomed(window_handle(window)) && window.Content().as<Grid>().FindName(L"RestoreGlyph").as<UIElement>().Visibility() == Visibility::Visible, "custom maximize button maximizes and shows the restore glyph");
        invoke_button(caption_button(window, L"CaptionMaximize")); co_await resume_after(200ms); co_await ui;
        check(!IsZoomed(window_handle(window)) && std::abs(root.ActualHeight() - before_maximize) < 2, "custom restore button returns to the original input panel size");
        auto footer = find<Border>(L"StatusCard"); auto footer_position = footer.TransformToVisual(root).TransformPoint({0, 0});
        check(footer_position.Y + footer.ActualHeight() <= root.ActualHeight(), "expanded footer stays inside the visible client area");
        input.Text(L"原生界面验收  😀𠮷 **Markdown**");
        invoke_button(find<Button>(L"SendButton"));
        co_await resume_after(100ms); co_await ui;
        auto response = vb::json::parse(to_string(co_await http("/receive")));
        check(input.Text().empty() && response["body"]["messages"].size() == 1 && response["body"]["messages"][0]["text"] == "原生界面验收  😀𠮷 **Markdown**", "send button delivers its real text through the phone HTTP protocol and clears only after queueing");
        auto session = response["body"]["session"];
        co_await http("/ack", vb::json{{"session", session}, {"through", response["body"]["messages"][0]["id"]}});
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
        checks.back()["actual"] = root.ActualHeight(); checks.back()["expected"] = collapsed_height; checks.back()["client"] = content_client_rect(window).bottom;
        co_await save_preview(window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "dark-closed.png");
        invoke_button(find<Button>(L"TransferButton")); co_await resume_after(350ms); co_await ui;
        check(std::abs(root.ActualHeight() - resized_height) < 2, "reopening preserves user-resized input height");
        checks.back()["actual"] = root.ActualHeight(); checks.back()["expected"] = resized_height; checks.back()["client"] = content_client_rect(window).bottom;
        invoke_button(find<Button>(L"SettingsButton")); co_await resume_after(250ms); co_await ui;
        check(settings_window && setting_switches.size() == 5 && IsWindowVisible(window_handle(settings_window)), "settings window opens with all five original switches");
        check(!settings_window.AppWindow().Presenter().as<windowing::OverlappedPresenter>().HasTitleBar(), "settings uses the same custom window frame");
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
        invoke_button(save); co_await resume_after(350ms); co_await ui;
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
        check(caption_button(qr_window, L"CaptionMaximize").Visibility() == Visibility::Collapsed, "fixed-size pairing window omits an unusable maximize action");
        invoke_button(caption_button(qr_window, L"CaptionClose")); co_await resume_after(150ms); co_await ui;
        check(!qr_window && IsWindowVisible(window_handle(window)), "pairing caption close dismisses only the pairing window");
        invoke_button(caption_button(window, L"CaptionClose")); co_await resume_after(200ms); co_await ui;
        check(!IsWindowVisible(window_handle(window)), "configured close-to-tray hides the window and keeps its server running");
        response = vb::json::parse(to_string(co_await http("/settings")));
        check(response["status"] == 200, "phone service remains available while minimized to tray");
        SetEvent(activation_event); co_await resume_after(300ms); co_await ui;
        check(IsWindowVisible(window_handle(window)) && WaitForSingleObject(activation_ack, 0) == WAIT_OBJECT_0, "single-instance activation restores and acknowledges the existing window");
        config->update({{"close_action", "ask"}}); invoke_button(caption_button(window, L"CaptionClose")); co_await resume_after(200ms); co_await ui;
        check(close_window && find_button(close_window.Content(), L"最小化到托盘") && find_button(close_window.Content(), L"直接关闭"), "close prompt offers both original actions");
        auto close_root = close_window.Content().as<FrameworkElement>();
        window_metrics["close_client"] = {close_root.ActualWidth() * ui_dpi_scale, close_root.ActualHeight() * ui_dpi_scale};
        check(!IsWindowEnabled(window_handle(window)), "close question is modal like the original and prevents conflicting main-window actions");
        co_await save_preview(close_window.Content().as<FrameworkElement>(), ui_test_report.parent_path() / "close-dark.png");
        invoke_button(caption_button(close_window, L"CaptionClose")); co_await resume_after(100ms); co_await ui;
        check(!close_window && IsWindowEnabled(window_handle(window)), "custom dialog close cancels the question and re-enables the main window");
    } catch (hresult_error const& error) { checks.push_back({{"test", "UI runtime exception"}, {"passed", false}, {"error", to_string(error.message())}}); }
      catch (std::exception const& error) { checks.push_back({{"test", "UI integration exception"}, {"passed", false}, {"error", error.what()}}); }
    output(); exit_app();
}
