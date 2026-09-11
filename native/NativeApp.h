#pragma once
#include "BridgeCore.h"
#undef GetCurrentTime
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include "vendor/qrcodegen.hpp"
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Hosting;
using namespace Windows::Foundation;
using namespace std::chrono_literals;
namespace windowing = Microsoft::UI::Windowing;
inline vb::fs::path config_path;
inline bool isolated = false;
inline int override_port = 0;
inline std::string override_bind;
inline HANDLE activation_event{}, activation_ack{};
inline void log_error(hstring const& message) {
    vb::fs::create_directories(config_path.parent_path());
    std::ofstream log(config_path.parent_path() / "crash.log", std::ios::binary | std::ios::app);
    log << to_string(message) << '\n';
}
inline HWND window_handle(Window const& window) { HWND hwnd{}; check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd)); return hwnd; }
inline SolidColorBrush brush(uint32_t rgb) { return SolidColorBrush(Windows::UI::Color{255, BYTE(rgb >> 16), BYTE(rgb >> 8), BYTE(rgb)}); }
inline UIElement xaml(std::wstring const& source) { return Markup::XamlReader::Load(source).as<UIElement>(); }
inline void accessible_name(DependencyObject const& object, wchar_t const* label) { Automation::AutomationProperties::SetName(object, label); }
inline void client_size(Window const& window, double width, double height) {
    double scale = GetDpiForWindow(window_handle(window)) / 96.0;
    window.AppWindow().ResizeClient({int32_t(std::lround(width * scale)), int32_t(std::lround(height * scale))});
}
inline void center(Window const& window, HWND owner = nullptr) {
    auto hwnd = window_handle(window); RECT rect{}, anchor{}; GetWindowRect(hwnd, &rect);
    if (owner) GetWindowRect(owner, &anchor);
    else { MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor); anchor = monitor.rcWork; }
    SetWindowPos(hwnd, nullptr, anchor.left + (anchor.right - anchor.left - rect.right + rect.left) / 2,
        anchor.top + (anchor.bottom - anchor.top - rect.bottom + rect.top) / 2, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}
inline std::map<std::wstring, std::wstring> const icon_paths{
    {L"moon", L"M16 3 A8 8 0 1 0 21 15 A8 8 0 0 1 16 3"},
    {L"sun", L"M12 8 A4 4 0 1 0 12 16 A4 4 0 1 0 12 8 M12 2 L12 4 M12 20 L12 22 M2 12 L4 12 M20 12 L22 12 M5 5 L6.5 6.5 M17.5 17.5 L19 19 M5 19 L6.5 17.5 M17.5 6.5 L19 5"},
    {L"settings", L"M9 3 L15 3 L16 6 L19 6 L22 11 L20 13 L20 17 L15 20 L12 18 L9 20 L4 17 L4 13 L2 11 L5 6 L8 6 Z M12 8 A4 4 0 1 0 12 16 A4 4 0 1 0 12 8"},
    {L"pause", L"M8 5 L8 19 M16 5 L16 19"}, {L"play", L"M8 4 L19 12 L8 20 Z"},
    {L"qr", L"M3 3 L9 3 L9 9 L3 9 Z M15 3 L21 3 L21 9 L15 9 Z M3 15 L9 15 L9 21 L3 21 Z M15 15 L18 15 L18 18 L21 18 L21 21 M15 18 L15 21"},
    {L"down", L"M6 9 L12 15 L18 9"}, {L"up", L"M6 15 L12 9 L18 15"}
};
inline void icon(Button const& button, std::wstring const& key) {
    auto path = xaml(L"<Path xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Width='21' Height='21' Stretch='Uniform' StrokeThickness='1.8' StrokeStartLineCap='Round' StrokeEndLineCap='Round' StrokeLineJoin='Round' Data='" + icon_paths.at(key) + L"'/>").as<Shapes::Path>();
    path.Stroke(button.Foreground()); button.Content(path);
}
struct App : ApplicationT<App, Markup::IXamlMetadataProvider> {
    XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata{nullptr};
    Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const& type) {
        if (!metadata) metadata = XamlTypeInfo::XamlControlsXamlMetaDataProvider();
        return metadata.GetXamlType(type);
    }
    Markup::IXamlType GetXamlType(hstring const& type) {
        if (!metadata) metadata = XamlTypeInfo::XamlControlsXamlMetaDataProvider();
        return metadata.GetXamlType(type);
    }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        if (!metadata) metadata = XamlTypeInfo::XamlControlsXamlMetaDataProvider();
        return metadata.GetXmlnsDefinitions();
    }
    Window window{nullptr}, settings_window{nullptr}, qr_window{nullptr}, close_window{nullptr};
    Grid root{nullptr};
    std::unique_ptr<vb::Config> config;
    std::unique_ptr<vb::Bridge> bridge;
    Microsoft::UI::Dispatching::DispatcherQueue dispatcher{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer poll{nullptr};
    std::thread initialization;
    bool closing = false, expanded = false, animating = false, dark = false;
    double collapsed_height = 360, expanded_height = 540;
    std::chrono::steady_clock::time_point animation_start;
    double animation_from{}, animation_to{};
    event_token rendering{};
    NOTIFYICONDATAW tray{};
    HICON app_icon{};
    TextBox qr_address{nullptr};
    Canvas qr_canvas{nullptr};
    std::map<std::string, ToggleSwitch> setting_switches;
    std::string draft_close, draft_send;
    App() {
        UnhandledException([](auto&&, UnhandledExceptionEventArgs const& args) { log_error(args.Message()); });
    }
    ~App() { if (initialization.joinable()) initialization.join(); }
    template<class T> T find(wchar_t const* id) { return root.FindName(id).as<T>(); }
    void safe(std::function<void()> const& action) {
        try { action(); }
        catch (hresult_error const& error) { status(to_string(error.message())); }
        catch (std::exception const& error) { status(error.what()); }
    }
    void status(std::string const& text) {
        if (!root || closing) return;
        find<TextBlock>(L"StatusText").Text(to_hstring(text));
        ToolTipService::SetToolTip(find<TextBlock>(L"StatusText"), box_value(to_hstring(text)));
        refresh_count();
    }
    void refresh_count() {
        auto s = config->settings();
        find<TextBlock>(L"CountText").Text(L"累计输入 " + to_hstring(s["character_count"].dump()) + L" 个字符");
    }
    Brush palette(wchar_t const* key) {
        auto dictionary = root.Resources().ThemeDictionaries().Lookup(box_value(dark ? L"Dark" : L"Light")).as<ResourceDictionary>();
        return dictionary.Lookup(box_value(key)).as<Brush>();
    }
    void chrome(Window const& target) {
        if (!target) return;
        auto hwnd = window_handle(target); BOOL enabled = dark;
        DwmSetWindowAttribute(hwnd, 20, &enabled, sizeof(enabled));
        COLORREF color = dark ? RGB(10, 10, 10) : RGB(243, 246, 251);
        DwmSetWindowAttribute(hwnd, 35, &color, sizeof(color));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(app_icon));
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(app_icon));
        target.Content().as<FrameworkElement>().RequestedTheme(dark ? ElementTheme::Dark : ElementTheme::Light);
    }
    void OnLaunched(LaunchActivatedEventArgs const&);
    void update_pause();
    void apply_settings();
    void change_theme();
    void send();
    void toggle_transfer();
    void open_settings();
    void open_qr();
    void draw_qr();
    void request_close();
    void initialize_tray();
    void exit_app();
    void minimize_to_tray();
    void restore() { ShowWindow(window_handle(window), SW_RESTORE); window.Activate(); SetForegroundWindow(window_handle(window)); }
    Window secondary(wchar_t const* title, FrameworkElement const& content, double width, double height) {
        auto target = Window(); target.Title(title); target.Content(content); content.Resources(root.Resources());
        SetWindowLongPtrW(window_handle(target), GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(window_handle(window)));
        chrome(target); client_size(target, width, height); center(target, window_handle(window)); target.Activate(); ShowWindow(window_handle(target), SW_SHOW); return target;
    }
    TextBlock label(wchar_t const* text, wchar_t const* style = nullptr) {
        TextBlock result; result.Text(text);
        if (style) result.Style(root.Resources().Lookup(box_value(style)).as<Style>());
        return result;
    }
    Button button(wchar_t const* text, std::function<void()> action, uint32_t color = 0, double width = 112) {
        Button result; result.Content(box_value(text)); result.Width(width);
        if (color) { result.Background(brush(color)); result.Foreground(brush(0xffffff)); }
        result.Click([this, action](auto&&, auto&&) { safe(action); }); return result;
    }
    StackPanel vertical(double spacing = 0) { StackPanel panel; panel.Spacing(spacing); return panel; }
    StackPanel horizontal(double spacing = 8) { auto panel = vertical(spacing); panel.Orientation(Orientation::Horizontal); return panel; }
    Border card(UIElement const& child) { Border result; result.CornerRadius(CornerRadius(22)); result.Padding(Thickness(16)); result.Background(palette(L"CardBrush")); result.Child(child); return result; }
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
};

inline void App::OnLaunched(LaunchActivatedEventArgs const&) try {
    Resources().MergedDictionaries().Append(XamlControlsResources());
    config = std::make_unique<vb::Config>(config_path); dark = config->settings()["theme"] == "dark";
    window = Window(); window.Title(L"声桥 · Windows 原生版（测试版）"); dispatcher = window.DispatcherQueue();
    app_icon = static_cast<HICON>(LoadImageW(nullptr, (vb::executable_dir() / "assets/voice-bridge.ico").c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    auto theme = Markup::XamlReader::Load(to_hstring(vb::read_file(vb::executable_dir() / "Theme.xaml"))).as<ResourceDictionary>();
    Resources().MergedDictionaries().Append(theme);
    root = xaml(vb::wide(vb::read_file(vb::executable_dir() / "Main.xaml"))).as<Grid>();
    root.Resources(theme); window.Content(root);
    find<Image>(L"Logo").Source(Imaging::BitmapImage(Uri(L"file:///" + (vb::executable_dir() / L"assets/voice-bridge.png").generic_wstring())));
    icon(find<Button>(L"SettingsButton"), L"settings"); icon(find<Button>(L"PauseButton"), L"pause");
    icon(find<Button>(L"QrButton"), L"qr"); icon(find<Button>(L"TransferButton"), L"down");
    find<Button>(L"SettingsButton").Click([this](auto&&, auto&&) { safe([&] { open_settings(); }); });
    find<Button>(L"ThemeButton").Click([this](auto&&, auto&&) { safe([&] { change_theme(); }); });
    find<Button>(L"PauseButton").Click([this](auto&&, auto&&) { if (bridge) { bridge->accepting = !bridge->accepting; update_pause(); status(bridge->accepting ? "已恢复接收手机文字" : "已暂停接收手机文字"); } });
    find<Button>(L"QrButton").Click([this](auto&&, auto&&) { if (bridge) safe([&] { open_qr(); }); });
    find<Button>(L"TransferButton").Click([this](auto&&, auto&&) { toggle_transfer(); });
    find<Button>(L"SendButton").Click([this](auto&&, auto&&) { safe([&] { send(); }); });
    Input::KeyboardAccelerator keyboard; keyboard.Key(Windows::System::VirtualKey::Enter); keyboard.Modifiers(Windows::System::VirtualKeyModifiers::Control);
    keyboard.Invoked([this](auto&&, Input::KeyboardAcceleratorInvokedEventArgs const& args) {
        args.Handled(true); safe([&] {
            if (config->settings()["send_mode"] == "ctrl_enter") send();
            else { auto text = find<TextBox>(L"DesktopText"); auto start = text.SelectionStart(); text.SelectedText(L"\r"); text.Select(start + 1, 0); }
        });
    });
    find<TextBox>(L"DesktopText").KeyboardAccelerators().Append(keyboard);
    find<TextBox>(L"DesktopText").KeyDown([this](auto&&, Input::KeyRoutedEventArgs const& args) {
        if (args.Key() == Windows::System::VirtualKey::Enter && !(GetKeyState(VK_CONTROL) & 0x8000) && config->settings()["send_mode"] == "enter") { args.Handled(true); safe([&] { send(); }); }
    });
    SetWindowSubclass(window_handle(window), &App::window_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    apply_settings(); chrome(window); client_size(window, 580, collapsed_height); center(window);
    window.Activate(); ShowWindow(window_handle(window), SW_SHOW); initialize_tray(); status("正在连接本机服务…");
    for (auto id : {L"QrButton", L"PauseButton", L"SendButton"}) find<Button>(id).IsEnabled(false);
    poll = dispatcher.CreateTimer(); poll.Interval(100ms);
    poll.Tick([this](auto&&, auto&&) {
        if (activation_event && WaitForSingleObject(activation_event, 0) == WAIT_OBJECT_0) { ResetEvent(activation_event); restore(); if (activation_ack) SetEvent(activation_ack); }
    }); poll.Start();
    initialization = std::thread([this] {
        try {
            auto next = std::make_shared<std::unique_ptr<vb::Bridge>>(std::make_unique<vb::Bridge>(*config, vb::executable_dir(), vb::paste_text, vb::press_enter,
                [this](auto const& message) { dispatcher.TryEnqueue([this, message] { status(message); }); }));
            if (!override_bind.empty()) { (*next)->displayed_ip = override_bind; (*next)->bind(override_bind, override_port ? override_port : 18765); }
            else { if (override_port) (*next)->port = override_port; (*next)->start(); }
            dispatcher.TryEnqueue([this, next] {
                if (closing) return; bridge = std::move(*next);
                for (auto id : {L"QrButton", L"PauseButton", L"SendButton"}) find<Button>(id).IsEnabled(true);
                status(bridge->displayed_ip != bridge->bound_ip ? "已保存地址当前不可用，请打开二维码并点击“刷新地址”" : "等待手机连接");
            });
        } catch (std::exception const& error) { auto message = std::string(error.what()); dispatcher.TryEnqueue([this, message] { status("启动失败：" + message); }); }
          catch (hresult_error const& error) { auto message = to_string(error.message()); dispatcher.TryEnqueue([this, message] { status("启动失败：" + message); }); }
    });
} catch (hresult_error const& error) { log_error(error.message()); MessageBoxW(nullptr, error.message().c_str(), L"声桥启动失败", MB_OK | MB_ICONERROR); Exit(); }
  catch (std::exception const& error) { log_error(to_hstring(error.what())); MessageBoxW(nullptr, to_hstring(error.what()).c_str(), L"声桥启动失败", MB_OK | MB_ICONERROR); Exit(); }
inline void App::update_pause() {
    auto b = find<Button>(L"PauseButton");
    b.Background(bridge->accepting ? palette(L"SecondaryBrush") : brush(0x2563EB));
    b.Foreground(bridge->accepting ? palette(L"InkBrush") : brush(0xffffff)); icon(b, bridge->accepting ? L"pause" : L"play");
    accessible_name(b, bridge->accepting ? L"暂停手机输入电脑" : L"恢复手机输入电脑");
}
inline void App::apply_settings() {
    auto s = config->settings();
    find<TextBlock>(L"ShortcutHint").Text(s["send_mode"] == "ctrl_enter" ? L"Ctrl + Enter 发送 · Enter 换行" : L"Enter 发送 · Ctrl + Enter 换行");
    find<TextBlock>(L"CountText").Visibility(s["show_char_count"].get<bool>() ? Visibility::Visible : Visibility::Collapsed);
    find<Border>(L"TransferCard").Visibility(s["show_desktop_to_phone"].get<bool>() ? Visibility::Visible : Visibility::Collapsed);
    if (!s["show_desktop_to_phone"].get<bool>() && expanded) { expanded = false; find<Grid>(L"TransferContent").Visibility(Visibility::Collapsed); }
    icon(find<Button>(L"TransferButton"), expanded ? L"up" : L"down");
    icon(find<Button>(L"ThemeButton"), dark ? L"sun" : L"moon"); refresh_count();
}
inline void App::change_theme() {
    dark = !dark; config->update({{"theme", dark ? "dark" : "light"}});
    for (auto const& w : {window, settings_window, qr_window, close_window}) if (w) chrome(w);
    icon(find<Button>(L"ThemeButton"), dark ? L"sun" : L"moon"); icon(find<Button>(L"SettingsButton"), L"settings");
    icon(find<Button>(L"QrButton"), L"qr"); icon(find<Button>(L"TransferButton"), expanded ? L"up" : L"down");
    if (bridge) update_pause();
    auto visual = ElementCompositionPreview::GetElementVisual(root);
    auto fade = visual.Compositor().CreateScalarKeyFrameAnimation(); fade.InsertKeyFrame(0, .78f); fade.InsertKeyFrame(1, 1.f); fade.Duration(224ms); visual.StartAnimation(L"Opacity", fade);
}
inline void App::send() {
    if (!bridge) { status("本机服务尚未就绪"); return; }
    auto box = find<TextBox>(L"DesktopText"); auto text = to_string(box.Text());
    if (text.empty()) { status("请先输入要发给手机的文字"); return; }
    bridge->queue(text); box.Text(L""); status("已发送 " + std::to_string(vb::character_count(text)) + " 个字符到手机");
}
inline void App::toggle_transfer() {
    if (animating || closing) return;
    RECT rect{}; GetClientRect(window_handle(window), &rect); auto scale = GetDpiForWindow(window_handle(window)) / 96.0;
    animation_from = rect.bottom / scale;
    if (!expanded) { collapsed_height = animation_from; animation_to = expanded_height = std::max(expanded_height, collapsed_height + 180); }
    else { expanded_height = animation_from; animation_to = collapsed_height; }
    expanded = !expanded; animating = true; animation_start = std::chrono::steady_clock::now();
    auto content = find<Grid>(L"TransferContent"); content.Visibility(Visibility::Visible); content.IsHitTestVisible(false);
    icon(find<Button>(L"TransferButton"), expanded ? L"up" : L"down"); accessible_name(find<Button>(L"TransferButton"), expanded ? L"收起电脑到手机" : L"展开电脑到手机");
    auto visual = ElementCompositionPreview::GetElementVisual(content); auto compositor = visual.Compositor();
    auto easing = compositor.CreateCubicBezierEasingFunction({.33f, 0.f}, {.67f, 1.f});
    auto opacity = compositor.CreateScalarKeyFrameAnimation(); opacity.InsertKeyFrame(0, expanded ? 0.f : 1.f); opacity.InsertKeyFrame(1, expanded ? 1.f : 0.f, easing); opacity.Duration(200ms); visual.StartAnimation(L"Opacity", opacity);
    ElementCompositionPreview::SetIsTranslationEnabled(content, true);
    auto offset = compositor.CreateVector3KeyFrameAnimation(); offset.InsertKeyFrame(0, {0, expanded ? -12.f : 0.f, 0}); offset.InsertKeyFrame(1, {0, expanded ? 0.f : -12.f, 0}, easing); offset.Duration(200ms); visual.StartAnimation(L"Translation", offset);
    rendering = CompositionTarget::Rendering([this](auto&&, auto&&) {
        auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - animation_start).count();
        auto t = std::min(1.0, elapsed / .2), progress = t * t * (3 - 2 * t);
        RECT rect{}; GetClientRect(window_handle(window), &rect); auto scale = GetDpiForWindow(window_handle(window)) / 96.0;
        client_size(window, rect.right / scale, animation_from + (animation_to - animation_from) * progress);
        if (t >= 1) {
            CompositionTarget::Rendering(rendering); animating = false;
            find<Grid>(L"TransferContent").Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
            find<Grid>(L"TransferContent").IsHitTestVisible(true);
        }
    });
}
