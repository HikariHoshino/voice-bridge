#pragma once
#include "NativeApp.h"

inline void App::open_settings() {
    if (settings_window) { settings_window.Activate(); return; }
    auto s = config->settings(); draft_close = s["close_action"]; draft_send = s["send_mode"];
    auto content = vertical(12); content.Margin(Thickness(18));
    content.Children().Append(label(L"设置", L"TitleStyle"));
    content.Children().Append(label(L"偏好保存在电脑，手机端打开页面时同步确认设置", L"MutedStyle"));
    auto interface_panel = vertical(4); interface_panel.Children().Append(label(L"界面与启动", L"CardTitleStyle"));
    auto add_switch = [&](StackPanel const& panel, std::string const& key, wchar_t const* text, bool enabled) {
        Grid row; row.MinHeight(38); auto caption = label(text); caption.VerticalAlignment(VerticalAlignment::Center); row.Children().Append(caption);
        ToggleSwitch toggle; toggle.IsOn(enabled); toggle.OnContent(box_value(L"")); toggle.OffContent(box_value(L"")); toggle.MinWidth(44); toggle.HorizontalAlignment(HorizontalAlignment::Right); accessible_name(toggle, text);
        row.Children().Append(toggle); panel.Children().Append(row); setting_switches[key] = toggle;
    };
    add_switch(interface_panel, "autostart", L"开机后自动启动", vb::autostart_enabled());
    setting_switches["autostart"].IsEnabled(!isolated);
    add_switch(interface_panel, "show_desktop_to_phone", L"显示电脑 → 手机", s["show_desktop_to_phone"]);
    add_switch(interface_panel, "show_char_count", L"显示累计输入字符数", s["show_char_count"]);
    Grid count_row; count_row.Height(38); auto count = label((L"累计字符    当前 " + to_hstring(s["character_count"].dump()) + L" 个").c_str()); count.VerticalAlignment(VerticalAlignment::Center); count_row.Children().Append(count);
    auto clear = button(L"清零", [this, count] { config->update({{"character_count", 0}}); count.Text(L"累计字符    当前 0 个"); refresh_count(); }, 0, 70);
    clear.Height(34); clear.HorizontalAlignment(HorizontalAlignment::Right); clear.Background(palette(L"DangerBrush")); clear.Foreground(palette(L"DangerInkBrush")); count_row.Children().Append(clear); interface_panel.Children().Append(count_row);
    content.Children().Append(card(interface_panel));
    auto behavior = vertical(6); behavior.Children().Append(label(L"操作方式", L"CardTitleStyle"));
    add_switch(behavior, "confirm_enter", L"按下回车二次确认", s["confirm_enter"]);
    add_switch(behavior, "confirm_send", L"发送到电脑二次确认", s["confirm_send"]);
    auto choices = [&](wchar_t const* title, std::string* draft, std::vector<std::pair<std::wstring, std::string>> const& items, double width) {
        behavior.Children().Append(label(title, L"CardTitleStyle")); auto row = horizontal();
        auto buttons = std::make_shared<std::vector<std::pair<weak_ref<Button>, std::string>>>();
        for (auto const& [caption, value] : items) {
            auto option = button(caption.c_str(), [this, draft, value, buttons] {
                *draft = value;
                for (auto const& [weak, v] : *buttons) if (auto b = weak.get()) { b.Background(v == value ? brush(0x2563EB) : palette(L"SecondaryBrush")); b.Foreground(v == value ? brush(0xffffff) : palette(L"InkBrush")); }
            }, 0, width);
            option.Height(38); option.Background(value == *draft ? brush(0x2563EB) : palette(L"SecondaryBrush")); option.Foreground(value == *draft ? brush(0xffffff) : palette(L"InkBrush"));
            buttons->emplace_back(make_weak(option), value); row.Children().Append(option);
        }
        behavior.Children().Append(row);
    };
    choices(L"关闭主窗口时", &draft_close, {{L"每次询问", "ask"}, {L"最小化到托盘", "tray"}, {L"直接退出", "exit"}}, 112);
    choices(L"电脑发送快捷键", &draft_send, {{L"Ctrl + Enter 发送", "ctrl_enter"}, {L"Enter 发送", "enter"}}, 174);
    content.Children().Append(card(behavior));
    auto actions = horizontal(); actions.HorizontalAlignment(HorizontalAlignment::Right);
    actions.Children().Append(button(L"保存设置", [this] {
        if (!isolated) vb::set_autostart(setting_switches.at("autostart").IsOn());
        vb::json values{{"close_action", draft_close}, {"send_mode", draft_send}};
        for (auto const& [key, toggle] : setting_switches) if (key != "autostart") values[key] = toggle.IsOn();
        config->update(values); apply_settings(); settings_window.Close(); status("设置已保存");
    }, 0x2563EB));
    actions.Children().Append(button(L"取消", [this] { settings_window.Close(); }, 0, 94)); content.Children().Append(actions);
    ScrollViewer scroll; scroll.Content(content); scroll.Background(palette(L"RootBrush"));
    settings_window = secondary(L"声桥设置", scroll, 500, 660);
    settings_window.Closed([this](auto&&, auto&&) { settings_window = nullptr; setting_switches.clear(); });
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
    auto content = vertical(10); content.Margin(Thickness(22));
    auto title = label(L"扫描二维码连接声桥", L"TitleStyle"); title.HorizontalAlignment(HorizontalAlignment::Center); content.Children().Append(title);
    auto subtitle = label(L"首次配对后请收藏手机页面", L"MutedStyle"); subtitle.HorizontalAlignment(HorizontalAlignment::Center); content.Children().Append(subtitle);
    qr_canvas = Canvas(); qr_canvas.Width(240); qr_canvas.Height(240); qr_canvas.Background(brush(0xffffff)); qr_canvas.HorizontalAlignment(HorizontalAlignment::Center); accessible_name(qr_canvas, L"手机配对二维码"); content.Children().Append(qr_canvas);
    qr_address = TextBox(); qr_address.IsReadOnly(true); qr_address.FontSize(12); qr_address.TextAlignment(TextAlignment::Center); accessible_name(qr_address, L"手机访问地址"); content.Children().Append(qr_address);
    auto actions = horizontal(); actions.HorizontalAlignment(HorizontalAlignment::Center);
    actions.Children().Append(button(L"复制访问地址", [this] { vb::clipboard(vb::wide(bridge->url())); status("手机访问地址已复制"); }, 0, 132));
    actions.Children().Append(button(L"刷新地址", [this] { bridge->refresh(); draw_qr(); }, 0x2563EB, 116));
    auto rotate = button(L"重新配对", [this] {
        auto result = MessageBoxW(window_handle(qr_window), L"旧的手机链接会立即失效，确定生成新链接吗？", L"重新配对", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (result == IDYES) { bridge->rotate(); draw_qr(); status("配对链接已更新，请重新扫码"); }
    }); rotate.Background(palette(L"DangerBrush")); rotate.Foreground(palette(L"DangerInkBrush")); actions.Children().Append(rotate); content.Children().Append(actions);
    Grid background; background.Background(palette(L"RootBrush")); background.Children().Append(content);
    draw_qr(); qr_window = secondary(L"手机配对", background, 480, 450);
    qr_window.AppWindow().Presenter().as<windowing::OverlappedPresenter>().IsResizable(false);
    qr_window.Closed([this](auto&&, auto&&) { qr_window = nullptr; qr_canvas = nullptr; qr_address = nullptr; });
}
inline void App::request_close() {
    auto action = config->settings()["close_action"];
    if (action == "tray") { minimize_to_tray(); return; }
    if (action == "exit") { exit_app(); return; }
    if (close_window) { close_window.Activate(); return; }
    auto content = vertical(12); content.Margin(Thickness(24));
    content.Children().Append(label(L"关闭窗口后要做什么？", L"TitleStyle"));
    content.Children().Append(label(L"可在设置中修改以后关闭窗口的默认行为。", L"MutedStyle"));
    auto actions = horizontal(10);
    actions.Children().Append(button(L"最小化到托盘", [this] { minimize_to_tray(); }, 0x2563EB, 142));
    auto exit = button(L"直接关闭", [this] { exit_app(); }); exit.Background(palette(L"DangerBrush")); exit.Foreground(palette(L"DangerInkBrush")); actions.Children().Append(exit);
    content.Children().Append(actions); Grid background; background.Background(palette(L"RootBrush")); background.Children().Append(content);
    close_window = secondary(L"关闭声桥", background, 450, 180);
    close_window.AppWindow().Presenter().as<windowing::OverlappedPresenter>().IsResizable(false);
    close_window.Closed([this](auto&&, auto&&) { close_window = nullptr; });
    content.KeyDown([this](auto&&, Input::KeyRoutedEventArgs const& args) {
        if (args.Key() == Windows::System::VirtualKey::Escape) close_window.Close();
        if (args.Key() == Windows::System::VirtualKey::Enter) minimize_to_tray();
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
    Exit();
}
inline void App::initialize_tray() {
    tray.cbSize = sizeof(tray); tray.hWnd = window_handle(window); tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; tray.uCallbackMessage = WM_APP + 1; tray.hIcon = app_icon;
    wcscpy_s(tray.szTip, L"声桥"); Shell_NotifyIconW(NIM_ADD, &tray);
    tray.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &tray);
}
inline LRESULT CALLBACK App::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto self = reinterpret_cast<App*>(data);
    if (message == WM_CLOSE) { self->safe([&] { self->request_close(); }); return 0; }
    if (message == WM_GETMINMAXINFO) {
        auto limits = reinterpret_cast<MINMAXINFO*>(lparam); auto scale = GetDpiForWindow(hwnd) / 96.0;
        RECT frame{0, 0, LONG(520 * scale), LONG((self->expanded ? 540 : self->collapsed_height) * scale)};
        if (self->animating) frame.bottom = LONG(330 * scale);
        AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)), GetDpiForWindow(hwnd));
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
