#pragma once
#include "NativeApp.h"

inline LRESULT CALLBACK settings_background(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR color) {
    if (message == WM_ERASEBKGND) {
        RECT bounds{}; GetClientRect(hwnd, &bounds);
        auto background = CreateSolidBrush(static_cast<COLORREF>(color));
        FillRect(reinterpret_cast<HDC>(wparam), &bounds, background); DeleteObject(background);
        return 1;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, settings_background, id);
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

inline IAsyncAction fade_settings(Window target, bool opening) {
    auto hwnd = window_handle(target);
    auto frame = target.Content().as<FrameworkElement>();
    apartment_context ui;
    BYTE initial{}; DWORD flags{}; COLORREF color{};
    check_bool(GetLayeredWindowAttributes(hwnd, &color, &initial, &flags));
    auto started = std::chrono::steady_clock::now();
    while (IsWindow(hwnd) && frame.IsHitTestVisible() == opening) {
        auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        auto progress = std::min(1., elapsed / 180.);
        auto alpha = static_cast<BYTE>(std::lround(initial + ((opening ? 255. : 0.) - initial) * progress));
        check_bool(SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA));
        if (progress == 1.) co_return;
        co_await resume_after(16ms); co_await ui;
    }
}

inline fire_and_forget reveal_settings(Window target) {
    auto hwnd = window_handle(target);
    try {
        auto frame = target.Content().as<FrameworkElement>(); frame.UpdateLayout();
        Imaging::RenderTargetBitmap first_frame;
        co_await first_frame.RenderAsync(frame);
        co_await ElementCompositionPreview::GetElementVisual(frame).Compositor().RequestCommitAsync();
        if (!IsWindow(hwnd) || !frame.IsHitTestVisible()) co_return;
        DwmFlush();
        // Reveal the completed surface at zero opacity, then fade the whole window.
        BOOL cloak = FALSE; check_hresult(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)));
        target.Activate();
        co_await fade_settings(target, true);
    } catch (hresult_error const& error) {
        log_error(error.message());
        if (IsWindow(hwnd)) target.Close();
    }
}

inline fire_and_forget App::close_settings() {
    if (!settings_window) co_return;
    auto target = settings_window;
    auto frame = target.Content().as<FrameworkElement>();
    if (!frame.IsHitTestVisible()) co_return;
    auto lifetime = get_strong();
    frame.IsHitTestVisible(false);
    try { co_await fade_settings(target, false); }
    catch (hresult_error const& error) { log_error(error.message()); }
    if (settings_window == target) target.Close();
}

inline void App::open_settings() {
    if (settings_window) { if (settings_window.Content().as<FrameworkElement>().IsHitTestVisible()) settings_window.Activate(); return; }
    auto s = config->settings(); draft_close = s["close_action"]; draft_send = s["send_mode"];
    const auto settings_width = std::max(500., std::floor(96 * ui_dpi_scale * 10 / 2.54) + 36);
    const auto settings_content_width = settings_width - 36;
    auto content = vertical(); content.Name(L"SettingsContent"); content.Width(settings_content_width); content.HorizontalAlignment(HorizontalAlignment::Left);
    auto title = label(L"设置", L"TitleStyle"); title.Height(font_line(22, true) + 4); content.Children().Append(title);
    auto subtitle = label(L"偏好保存在电脑，手机端打开页面时同步确认设置"); subtitle.Height(font_line(10) + 4); subtitle.Margin({0, 2, 0, 14}); bind_color(subtitle, TextBlock::ForegroundProperty(), L"MutedBrush"); content.Children().Append(subtitle);
    auto interface_panel = vertical(); auto interface_title = label(L"界面与启动", L"CardTitleStyle"); interface_title.Height(font_line(11, true) + 4); interface_title.Margin({0, 0, 0, 4}); interface_panel.Children().Append(interface_title);
    auto add_switch = [&](StackPanel const& panel, std::string const& key, wchar_t const* text, bool enabled) {
        Grid row; row.MinHeight(24); row.Margin({0, 7, 0, 7});
        ColumnDefinition caption_column; ColumnDefinition control_column; control_column.Width({1, GridUnitType::Auto});
        row.ColumnDefinitions().Append(caption_column); row.ColumnDefinitions().Append(control_column);
        auto caption = label(text); caption.TextWrapping(TextWrapping::Wrap); caption.Margin({0, 0, 12, 0}); caption.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(caption);
        ToggleSwitch toggle; toggle.IsOn(enabled); toggle.OnContent(box_value(L"")); toggle.OffContent(box_value(L"")); toggle.HorizontalAlignment(HorizontalAlignment::Right); accessible_name(toggle, text);
        toggle.KeyDown([](auto const& sender, Input::KeyRoutedEventArgs const& args) { if (args.Key() == Windows::System::VirtualKey::Enter) { auto control = sender.template as<ToggleSwitch>(); control.IsOn(!control.IsOn()); args.Handled(true); } });
        Grid::SetColumn(toggle, 1); row.Children().Append(toggle); panel.Children().Append(row); setting_switches[key] = toggle;
    };
    add_switch(interface_panel, "autostart", L"开机后自动启动", vb::autostart_enabled());
    setting_switches["autostart"].IsEnabled(!isolated);
    add_switch(interface_panel, "show_desktop_to_phone", L"显示电脑 → 手机", s["show_desktop_to_phone"]);
    add_switch(interface_panel, "show_char_count", L"显示累计输入字符数", s["show_char_count"]);
    Grid count_row; count_row.MinHeight(34); count_row.Margin({0, 6, 0, 0});
    ColumnDefinition count_column; ColumnDefinition clear_column; clear_column.Width({1, GridUnitType::Auto});
    count_row.ColumnDefinitions().Append(count_column); count_row.ColumnDefinitions().Append(clear_column);
    auto count = label((L"累计字符    当前 " + to_hstring(s["character_count"].dump()) + L" 个").c_str()); count.TextWrapping(TextWrapping::Wrap); count.Margin({0, 0, 12, 0}); count.VerticalAlignment(VerticalAlignment::Center); count_row.Children().Append(count);
    auto clear = button(L"清零", [this, count] { config->update({{"character_count", 0}}); count.Text(L"累计字符    当前 0 个"); refresh_count(); }, 0, 70);
    clear.Height(34); clear.CornerRadius({13, 13, 13, 13}); clear.HorizontalAlignment(HorizontalAlignment::Right); bind_color(clear, Control::BackgroundProperty(), L"DangerBrush"); bind_color(clear, Control::ForegroundProperty(), L"DangerInkBrush"); Grid::SetColumn(clear, 1); count_row.Children().Append(clear); interface_panel.Children().Append(count_row);
    auto interface_card = card(interface_panel); interface_card.Name(L"SettingsInterfaceCard"); interface_card.Width(settings_content_width); interface_card.HorizontalAlignment(HorizontalAlignment::Left); interface_card.Padding({16, 16, 16, 16}); interface_card.CornerRadius({22, 22, 22, 22}); interface_card.Margin({0, 0, 0, 12}); content.Children().Append(interface_card);
    auto behavior = vertical(); auto behavior_title = label(L"操作方式", L"CardTitleStyle"); behavior_title.Height(font_line(11, true) + 4); behavior_title.Margin({0, 0, 0, 4}); behavior.Children().Append(behavior_title);
    add_switch(behavior, "confirm_enter", L"按下回车二次确认", s["confirm_enter"]);
    add_switch(behavior, "confirm_send", L"发送到电脑二次确认", s["confirm_send"]);
    auto choices = [&](wchar_t const* title, std::string* draft, std::vector<std::pair<std::wstring, std::string>> const& items) {
        auto heading = label(title, L"CardTitleStyle"); heading.Height(font_line(11, true) + 4); heading.Margin({0, draft == &draft_close ? 12. : 14., 0, 0}); behavior.Children().Append(heading);
        Grid row; row.ColumnSpacing(8); row.Margin({0, 5, 0, 0});
        auto buttons = std::make_shared<std::vector<std::pair<weak_ref<Button>, std::string>>>();
        for (auto const& [caption, value] : items) {
            auto option = button(caption.c_str(), [this, draft, value, buttons] {
                *draft = value;
                for (auto const& [weak, v] : *buttons) if (auto b = weak.get()) select_button(b, v == value);
            });
            option.Width(std::numeric_limits<double>::quiet_NaN()); option.MinWidth(0); option.HorizontalAlignment(HorizontalAlignment::Stretch); option.Padding({8, 0, 8, 0});
            option.Height(38); option.CornerRadius({14, 14, 14, 14}); select_button(option, value == *draft);
            Grid::SetColumn(option, row.ColumnDefinitions().Size()); row.ColumnDefinitions().Append(ColumnDefinition());
            buttons->emplace_back(make_weak(option), value); row.Children().Append(option);
        }
        behavior.Children().Append(row);
    };
    choices(L"关闭主窗口时", &draft_close, {{L"每次询问", "ask"}, {L"最小化到托盘", "tray"}, {L"直接退出", "exit"}});
    choices(L"电脑发送快捷键", &draft_send, {{L"Ctrl + Enter 发送", "ctrl_enter"}, {L"Enter 发送", "enter"}});
    auto behavior_card = card(behavior); behavior_card.Name(L"SettingsBehaviorCard"); behavior_card.Width(settings_content_width); behavior_card.HorizontalAlignment(HorizontalAlignment::Left); behavior_card.Padding({16, 16, 16, 16}); behavior_card.CornerRadius({22, 22, 22, 22}); content.Children().Append(behavior_card);
    auto actions = horizontal(); actions.Name(L"SettingsActions"); actions.Height(42); actions.HorizontalAlignment(HorizontalAlignment::Right); actions.Margin({0, 14, 0, 0});
    actions.Children().Append(button(L"保存设置", [this] {
        if (!isolated) vb::set_autostart(setting_switches.at("autostart").IsOn());
        vb::json values{{"close_action", draft_close}, {"send_mode", draft_send}};
        for (auto const& [key, toggle] : setting_switches) if (key != "autostart") values[key] = toggle.IsOn();
        config->update(values); apply_settings(); close_settings(); status("设置已保存");
    }, 0x2563EB));
    actions.Children().Append(button(L"取消", [this] { close_settings(); }, 0, 94));
    ScrollViewer scroll; scroll.Name(L"SettingsScroll"); scroll.Content(content); scroll.Padding(Thickness(0)); scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled); scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Hidden);
    scroll.HorizontalContentAlignment(HorizontalAlignment::Left); scroll.VerticalContentAlignment(VerticalAlignment::Top);
    Grid layout; layout.Name(L"SettingsLayout"); layout.Padding({18, 18, 18, 18}); bind_color(layout, Panel::BackgroundProperty(), L"RootBrush");
    RowDefinition body_row; RowDefinition actions_row; actions_row.Height({1, GridUnitType::Auto});
    layout.RowDefinitions().Append(body_row); layout.RowDefinitions().Append(actions_row);
    layout.Children().Append(scroll); Grid::SetRow(actions, 1); layout.Children().Append(actions);
    settings_window = Window(); settings_window.Title(L"声桥设置");
    auto settings_hwnd = window_handle(settings_window);
    auto background = palette(L"RootBrush").as<SolidColorBrush>().Color();
    check_bool(SetWindowSubclass(settings_hwnd, settings_background, 2, RGB(background.R, background.G, background.B)));
    BOOL cloak = TRUE;
    check_hresult(DwmSetWindowAttribute(settings_hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak)));
    // A single explicit opacity animation owns both opening and closing.
    BOOL disable_transitions = TRUE;
    check_hresult(DwmSetWindowAttribute(settings_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable_transitions, sizeof(disable_transitions)));
    SetWindowLongPtrW(settings_hwnd, GWL_EXSTYLE, GetWindowLongPtrW(settings_hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
    check_bool(SetLayeredWindowAttributes(settings_hwnd, 0, 0, LWA_ALPHA));
    attach_frame(settings_window, layout, settings_width, 774, false);
    SetWindowLongPtrW(settings_hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(window_handle(window)));
    chrome(settings_window); client_size(settings_window, settings_width, 774);
    layout.UpdateLayout(); content.Measure({float(settings_content_width), std::numeric_limits<float>::infinity()});
    client_size(settings_window, settings_width, std::ceil(content.DesiredSize().Height) + 36 + 14 + 42);
    center(settings_window, window_handle(window));
    scroll.SizeChanged([weak = make_weak(content)](auto const& sender, auto&&) {
        if (auto panel = weak.get()) {
            auto width = sender.template as<FrameworkElement>().ActualWidth(); panel.Width(width);
            for (auto const& child : panel.as<StackPanel>().Children()) if (auto border = child.try_as<Border>()) border.Width(width);
        }
    });
    settings_window.Closed([this, settings_hwnd](auto&&, auto&&) {
        ShowWindow(settings_hwnd, SW_HIDE);
        settings_window = nullptr; setting_switches.clear();
    });
    settings_window.AppWindow().Closing([this](auto&&, windowing::AppWindowClosingEventArgs const& args) {
        if (!closing) { args.Cancel(true); close_settings(); }
    });
    auto loaded_token = std::make_shared<event_token>();
    *loaded_token = settings_window.Content().as<FrameworkElement>().Loaded([weak = make_weak(settings_window), loaded_token](auto const& sender, auto&&) {
        sender.template as<FrameworkElement>().Loaded(*loaded_token);
        if (auto target = weak.get()) reveal_settings(target);
    });
    settings_window.Activate();
}

inline void App::draw_qr() {
    auto url = bridge->url(); qr_address.Text(to_hstring(url)); qr_canvas.Children().Clear();
    auto qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    double unit = 240.0 / (qr.getSize() + 8);
    for (int y = 0; y < qr.getSize(); ++y) for (int x = 0; x < qr.getSize(); ++x) if (qr.getModule(x, y)) {
        Shapes::Rectangle module; module.Width(unit + .05); module.Height(unit + .05); module.Fill(brush(0));
        Canvas::SetLeft(module, (x + 4) * unit); Canvas::SetTop(module, (y + 4) * unit); qr_canvas.Children().Append(module);
    }
}
inline void App::open_qr() {
    if (qr_window) { qr_window.Close(); return; }
    auto content = vertical(); content.Margin(Thickness(22));
    auto title = label(L"扫描二维码连接声桥", L"TitleStyle"); title.Height(font_line(22, true) + 4); title.HorizontalAlignment(HorizontalAlignment::Center); content.Children().Append(title);
    auto subtitle = label(L"首次配对后请收藏手机页面"); subtitle.Height(font_line(10) + 4); subtitle.Margin({0, 2, 0, 12}); bind_color(subtitle, TextBlock::ForegroundProperty(), L"MutedBrush"); subtitle.HorizontalAlignment(HorizontalAlignment::Center); content.Children().Append(subtitle);
    qr_canvas = Canvas(); qr_canvas.Width(240); qr_canvas.Height(240); qr_canvas.Background(brush(0xffffff)); qr_canvas.HorizontalAlignment(HorizontalAlignment::Center); accessible_name(qr_canvas, L"手机配对二维码"); content.Children().Append(qr_canvas);
    qr_address = TextBox(); qr_address.IsReadOnly(true); qr_address.FontSize(12 * ui_dpi_scale); qr_address.MinHeight(0); qr_address.Height(font_line(9) + 6); qr_address.Padding({3, 0, 3, 0}); qr_address.Margin({0, 10, 0, 10}); qr_address.TextAlignment(TextAlignment::Center); accessible_name(qr_address, L"手机访问地址"); content.Children().Append(qr_address);
    auto actions = horizontal(); actions.HorizontalAlignment(HorizontalAlignment::Center);
    actions.Children().Append(button(L"复制访问地址", [this] { vb::clipboard(vb::wide(bridge->url())); status("手机访问地址已复制"); }, 0, 132));
    actions.Children().Append(button(L"刷新地址", [this] { bridge->refresh(); draw_qr(); }, 0x2563EB, 116));
    auto rotate = button(L"重新配对", [this] {
        auto result = MessageBoxW(window_handle(qr_window), L"旧的手机链接会立即失效，确定生成新链接吗？", L"重新配对", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (result == IDYES) { bridge->rotate(); draw_qr(); status("配对链接已更新，请重新扫码"); }
    }); bind_color(rotate, Control::BackgroundProperty(), L"DangerBrush"); bind_color(rotate, Control::ForegroundProperty(), L"DangerInkBrush"); actions.Children().Append(rotate); content.Children().Append(actions);
    Grid background; bind_color(background, Panel::BackgroundProperty(), L"RootBrush"); background.Children().Append(content);
    const auto pairing_width = std::max(480., std::round(420 * ui_dpi_scale));
    const auto pairing_height = 44 + font_line(22, true) + 4 + font_line(10) + 4 + 14 + 240 + font_line(9) + 6 + 20 + 42;
    draw_qr(); qr_window = secondary(L"手机配对", background, pairing_width, std::max(450., pairing_height), false);
    select_button(find<Button>(L"QrButton"), true); icon(find<Button>(L"QrButton"), L"qr");
    qr_window.Closed([this](auto&&, auto&&) { qr_window = nullptr; qr_canvas = nullptr; qr_address = nullptr; select_button(find<Button>(L"QrButton"), false); icon(find<Button>(L"QrButton"), L"qr"); });
}
inline void App::request_close() {
    auto action = config->settings()["close_action"];
    if (action == "tray") { minimize_to_tray(); return; }
    if (action == "exit") { exit_app(); return; }
    if (close_window) { close_window.Activate(); return; }
    auto content = vertical(); content.Margin(Thickness(24));
    auto title = label(L"关闭窗口后要做什么？", L"TitleStyle"); title.Height(font_line(22, true) + 4); content.Children().Append(title);
    auto subtitle = label(L"可在设置中修改以后关闭窗口的默认行为。"); subtitle.Height(font_line(10) + 4); subtitle.Margin({0, 3, 0, 18}); bind_color(subtitle, TextBlock::ForegroundProperty(), L"MutedBrush"); content.Children().Append(subtitle);
    auto actions = horizontal(10);
    actions.Children().Append(button(L"最小化到托盘", [this] { minimize_to_tray(); }, 0x2563EB, 142));
    auto exit = button(L"直接关闭", [this] { exit_app(); }); bind_color(exit, Control::BackgroundProperty(), L"DangerBrush"); bind_color(exit, Control::ForegroundProperty(), L"DangerInkBrush"); actions.Children().Append(exit);
    content.Children().Append(actions); Grid background; bind_color(background, Panel::BackgroundProperty(), L"RootBrush"); background.Children().Append(content);
    content.Measure({std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()});
    const auto close_height = 48 + font_line(22, true) + 4 + font_line(10) + 4 + 21 + 42;
    close_window = secondary(L"关闭声桥", background, std::max<double>(450., content.DesiredSize().Width), std::max(180., close_height), false);
    EnableWindow(window_handle(window), FALSE);
    close_window.Closed([this](auto&&, auto&&) { close_window = nullptr; EnableWindow(window_handle(window), TRUE); });
    actions.Children().GetAt(0).as<Button>().Focus(FocusState::Programmatic);
    content.KeyDown([this](auto&&, Input::KeyRoutedEventArgs const& args) {
        if (args.Key() == Windows::System::VirtualKey::Escape) { args.Handled(true); close_window.Close(); }
        if (args.Key() == Windows::System::VirtualKey::Enter) { args.Handled(true); minimize_to_tray(); }
    });
}
inline void App::minimize_to_tray() {
    if (qr_window) qr_window.Close(); if (settings_window) settings_window.Close(); if (close_window) close_window.Close();
    ShowWindow(window_handle(window), SW_HIDE);
}
inline void App::exit_app() {
    if (closing) return; closing = true;
    if (animating) { CompositionTarget::Rendering(rendering); animating = false; }
    if (poll) poll.Stop(); if (initialization.joinable()) initialization.join(); bridge.reset();
    Shell_NotifyIconW(NIM_DELETE, &tray);
    if (qr_window) qr_window.Close(); if (settings_window) settings_window.Close(); if (close_window) close_window.Close();
    RemoveWindowSubclass(window_handle(window), &App::window_proc, 1); window.Close();
    if (app_icon) { DestroyIcon(app_icon); app_icon = nullptr; }
    if (app_icon_small) { DestroyIcon(app_icon_small); app_icon_small = nullptr; }
    Exit();
}
inline void App::initialize_tray() {
    tray.cbSize = sizeof(tray); tray.hWnd = window_handle(window); tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; tray.uCallbackMessage = WM_APP + 1; tray.hIcon = app_icon_small;
    wcscpy_s(tray.szTip, L"声桥"); Shell_NotifyIconW(NIM_ADD, &tray);
    tray.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &tray);
}
inline LRESULT CALLBACK App::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto self = reinterpret_cast<App*>(data);
    if (message == WM_CLOSE) { self->safe([&] { self->request_close(); }); return 0; }
    if (message == WM_GETMINMAXINFO) {
        auto limits = reinterpret_cast<MINMAXINFO*>(lparam);
        RECT frame{0, 0, 520, LONG(self->expanded ? self->collapsed_height + 180 : self->collapsed_height)};
        if (self->animating) frame.bottom = 330;
        frame.bottom += caption_height(hwnd);
        frame.right += self->window_border_width;
        frame.bottom += self->window_border_height;
        limits->ptMinTrackSize = {frame.right - frame.left, frame.bottom - frame.top};
        if (!self->expanded && !self->animating) limits->ptMaxTrackSize.y = limits->ptMinTrackSize.y;
        return 0;
    }
    if (message == WM_APP + 1) {
        auto event = LOWORD(lparam);
        if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONDBLCLK) self->restore();
        if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            auto menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, 1, L"打开声桥"); AppendMenuW(menu, MF_STRING, 2, L"退出");
            SetMenuDefaultItem(menu, 1, FALSE); POINT point{}; GetCursorPos(&point); SetForegroundWindow(hwnd);
            auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr); DestroyMenu(menu);
            if (command == 1) self->restore(); if (command == 2) self->exit_app();
        }
        return 0;
    }
    static UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (message == taskbar_created) self->initialize_tray();
    return DefSubclassProc(hwnd, message, wparam, lparam);
}
