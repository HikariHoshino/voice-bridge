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
#include <regex>
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
inline vb::fs::path ui_test_report;
inline int ui_test_exit_code = 0;
inline HANDLE activation_event{}, activation_ack{};
inline void log_error(hstring const& message) {
    vb::fs::create_directories(config_path.parent_path());
    std::ofstream log(config_path.parent_path() / "crash.log", std::ios::binary | std::ios::app);
    log << to_string(message) << '\n';
}
inline HWND window_handle(Window const& window) { HWND hwnd{}; check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd)); return hwnd; }
inline double ui_dpi_scale = 1;
inline double font_line(int points, bool bold = false) {
    auto dc = GetDC(nullptr);
    auto font = CreateFontW(-MulDiv(points, static_cast<int>(96 * ui_dpi_scale), 72), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    auto previous = SelectObject(dc, font); TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous); DeleteObject(font); ReleaseDC(nullptr, dc); return metrics.tmHeight;
}
inline std::wstring scaled_font_xaml(std::string const& source) {
    auto input = vb::wide(source);
    for (auto const& pattern : {std::wregex(L"(FontSize=['\"])([0-9.]+)(['\"])", std::regex::ECMAScript), std::wregex(L"(Property=\"FontSize\" Value=\")([0-9.]+)(\")", std::regex::ECMAScript)}) {
        std::wstring result; size_t position = 0;
        for (auto match = std::wsregex_iterator(input.begin(), input.end(), pattern); match != std::wsregex_iterator(); ++match) {
            result += input.substr(position, match->position() - position);
            result += (*match)[1].str() + std::to_wstring(std::stod((*match)[2].str()) * ui_dpi_scale) + (*match)[3].str();
            position = match->position() + match->length();
        }
        input = result + input.substr(position);
    }
    return input;
}
inline Grid pixel_host(FrameworkElement const& content, double width, double height) {
    // Match Tk's physical-pixel card geometry and separately DPI-scaled fonts.
    Grid host; Viewbox view; view.Stretch(Stretch::Fill); content.Width(width); content.Height(height); view.Child(content); host.Children().Append(view);
    host.SizeChanged([weak = make_weak(content)](auto const& sender, auto&&) {
        if (auto surface = weak.get()) {
            auto host = sender.template as<FrameworkElement>(); auto scale = host.XamlRoot() ? host.XamlRoot().RasterizationScale() : ui_dpi_scale;
            surface.Width(host.ActualWidth() * scale); surface.Height(host.ActualHeight() * scale);
        }
    });
    return host;
}
inline SolidColorBrush brush(uint32_t rgb) { return SolidColorBrush(Windows::UI::Color{255, BYTE(rgb >> 16), BYTE(rgb >> 8), BYTE(rgb)}); }
inline UIElement xaml(std::wstring const& source) { return Markup::XamlReader::Load(source).as<UIElement>(); }
inline void accessible_name(DependencyObject const& object, wchar_t const* label) { Automation::AutomationProperties::SetName(object, label); }
inline void client_size(Window const& window, double width, double height) {
    window.AppWindow().ResizeClient({int32_t(std::lround(width)), int32_t(std::lround(height))});
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
    {L"settings", L"M12 9 A3 3 0 1 0 12 15 A3 3 0 1 0 12 9 M12 5 A7 7 0 1 0 12 19 A7 7 0 1 0 12 5 M12 3 L12 5.52 M3 12 L5.52 12 M18.48 12 L21 12 M12 18.48 L12 21"},
    {L"pause", L"M8 6 L8 18 M16 6 L16 18"}, {L"play", L"M7 5 L19 12 L7 19 Z"},
    {L"qr", L"M4 4 L10 4 L10 10 L4 10 Z M14 4 L20 4 L20 10 L14 10 Z M4 14 L10 14 L10 20 L4 20 Z M15 15 L20 15 L20 20 M15 20 L17 20"},
    {L"down", L"M6 9 L12 15 L18 9"}, {L"up", L"M6 15 L12 9 L18 15"},
    {L"copy", L"M5 5 L16 5 L16 16 L5 16 Z M9 9 L20 9 L20 20 L9 20 Z"},
    {L"refresh", L"M17.73 7.98 A7 7 0 1 0 16.50 17.36 M16 5 L20 6 L19 10"}
};
inline void icon(Button const& button, std::wstring const& key) {
    auto path = xaml(L"<Path xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Width='24' Height='24' Stretch='None' StrokeThickness='2' StrokeStartLineCap='Round' StrokeEndLineCap='Round' StrokeLineJoin='Round' Data='" + icon_paths.at(key) + L"'/>").as<Shapes::Path>();
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
    ResourceDictionary theme_resources{nullptr};
    std::unique_ptr<vb::Config> config;
    std::unique_ptr<vb::Bridge> bridge;
    Microsoft::UI::Dispatching::DispatcherQueue dispatcher{nullptr};
    Microsoft::UI::Dispatching::DispatcherQueueTimer poll{nullptr};
    std::thread initialization;
    bool closing = false, expanded = false, animating = false, dark = false;
    double collapsed_height = 360, expanded_height = 540;
    double initial_width = 580;
    std::chrono::steady_clock::time_point animation_start;
    double animation_from{}, animation_to{};
    event_token rendering{};
    NOTIFYICONDATAW tray{};
    HICON app_icon{};
    std::wstring startup_stage;
    TextBox qr_address{nullptr};
    Canvas qr_canvas{nullptr};
    std::map<std::string, ToggleSwitch> setting_switches;
    std::string draft_close, draft_send;
    struct ThemeBinding { weak_ref<DependencyObject> object; DependencyProperty property; hstring key; };
    std::vector<ThemeBinding> theme_bindings;
    void bind_color(DependencyObject const& object, DependencyProperty const& property, wchar_t const* key) {
        object.SetValue(property, palette(key));
        for (auto& binding : theme_bindings) if (binding.object.get() == object && binding.property == property) { binding.key = key; return; }
        theme_bindings.push_back({make_weak(object), property, key});
    }
    void select_button(Button const& button, bool selected) {
        bind_color(button, Control::BackgroundProperty(), selected ? L"PrimaryBrush" : L"SecondaryBrush");
        bind_color(button, Control::ForegroundProperty(), selected ? L"WhiteBrush" : L"InkBrush");
    }
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
        auto dictionary = theme_resources.ThemeDictionaries().Lookup(box_value(dark ? L"Dark" : L"Light")).as<ResourceDictionary>();
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
    fire_and_forget run_ui_tests();
    void exit_app();
    void minimize_to_tray();
    void restore() { ShowWindow(window_handle(window), SW_RESTORE); window.Activate(); SetForegroundWindow(window_handle(window)); }
    Window secondary(wchar_t const* title, FrameworkElement const& content, double width, double height) {
        auto target = Window(); target.Title(title); target.Content(pixel_host(content, width, height));
        SetWindowLongPtrW(window_handle(target), GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(window_handle(window)));
        chrome(target); client_size(target, width, height); center(target, window_handle(window)); target.Activate(); ShowWindow(window_handle(target), SW_SHOW); return target;
    }
    TextBlock label(wchar_t const* text, wchar_t const* style = nullptr) {
        TextBlock result; result.Text(text);
        if (style) result.Style(theme_resources.Lookup(box_value(style)).as<Style>());
        return result;
    }
    Button button(wchar_t const* text, std::function<void()> action, uint32_t color = 0, double width = 112) {
        Button result; result.Content(box_value(text)); result.Width(width);
        if (color) { result.Background(brush(color)); result.Foreground(brush(0xffffff)); }
        result.Click([this, action](auto&&, auto&&) { safe(action); }); return result;
    }
    StackPanel vertical(double spacing = 0) { StackPanel panel; panel.Spacing(spacing); return panel; }
    StackPanel horizontal(double spacing = 8) { auto panel = vertical(spacing); panel.Orientation(Orientation::Horizontal); return panel; }
    Border card(UIElement const& child) { Border result; result.CornerRadius(CornerRadius(22)); result.Padding(Thickness(27)); bind_color(result, Border::BackgroundProperty(), L"CardBrush"); result.Child(child); return result; }
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
};

inline void App::OnLaunched(LaunchActivatedEventArgs const&) try {
    startup_stage = L"controls resources";
    Resources().MergedDictionaries().Append(XamlControlsResources());
    config = std::make_unique<vb::Config>(config_path); dark = config->settings()["theme"] == "dark";
    window = Window(); window.Title(L"声桥 · Windows 原生版（测试版）"); dispatcher = window.DispatcherQueue();
    ui_dpi_scale = GetDpiForWindow(window_handle(window)) / 96.0;
    app_icon = static_cast<HICON>(LoadImageW(nullptr, (vb::executable_dir() / "assets/voice-bridge.ico").c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
    startup_stage = L"theme dictionary";
    theme_resources = Markup::XamlReader::Load(scaled_font_xaml(vb::read_file(vb::executable_dir() / "Theme.xaml"))).as<ResourceDictionary>();
    Resources().MergedDictionaries().Append(theme_resources);
    startup_stage = L"main XAML";
    root = xaml(scaled_font_xaml(vb::read_file(vb::executable_dir() / "Main.xaml"))).as<Grid>();
    startup_stage = L"attach content";
    const auto title_height = font_line(22, true) + 4;
    const auto subtitle_height = font_line(10) + 4;
    const auto card_title_height = font_line(11, true) + 4;
    const auto muted_height = font_line(9) + 4;
    const auto header_height = title_height + subtitle_height + 2;
    const auto card_height = card_title_height + muted_height + 2 + 54;
    const auto status_height = std::max(muted_height, font_line(10) + 2) + 30;
    initial_width = std::max(580., std::floor(96 * ui_dpi_scale * 10 / 2.54) + 60);
    collapsed_height = std::max(360., 36 + header_height + 14 + 2 * (card_height + 12) + status_height + 20);
    expanded_height = collapsed_height + 12 + (5 * font_line(10) + 24) + 10 + 42;
    find<TextBlock>(L"MainTitle").Height(title_height); find<TextBlock>(L"Subtitle").Height(subtitle_height);
    for (auto id : {L"ConnectionTitle", L"TransferTitle"}) find<TextBlock>(id).Height(card_title_height);
    for (auto id : {L"ConnectionSubtitle", L"TransferSubtitle"}) find<TextBlock>(id).Height(muted_height);
    auto rows = find<Grid>(L"Layout").RowDefinitions();
    rows.GetAt(0).Height({header_height + 14, GridUnitType::Pixel}); rows.GetAt(1).Height({card_height + 12, GridUnitType::Pixel}); rows.GetAt(3).Height({status_height, GridUnitType::Pixel});
    find<Grid>(L"TransferLayout").RowDefinitions().GetAt(0).Height({card_height - 54, GridUnitType::Pixel});
    find<Border>(L"TransferCard").MinHeight(card_height);
    window.Content(pixel_host(root, initial_width, collapsed_height));
    startup_stage = L"logo image";
    find<Image>(L"Logo").Source(Imaging::BitmapImage(Uri(L"file:///" + (vb::executable_dir() / L"assets/voice-bridge.png").generic_wstring())));
    startup_stage = L"button icons";
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
    find<TextBox>(L"DesktopText").PreviewKeyDown([this](auto&&, Input::KeyRoutedEventArgs const& args) {
        if (args.Key() == Windows::System::VirtualKey::Enter && !(GetKeyState(VK_CONTROL) & 0x8000) && config->settings()["send_mode"] == "enter") { args.Handled(true); safe([&] { send(); }); }
    });
    SetWindowSubclass(window_handle(window), &App::window_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    startup_stage = L"apply settings";
    apply_settings(); chrome(window);
    startup_stage = L"window dimensions";
    client_size(window, initial_width, collapsed_height); center(window);
    startup_stage = L"activate window";
    window.Activate(); ShowWindow(window_handle(window), SW_SHOW); initialize_tray(); status("正在连接本机服务…");
    for (auto id : {L"QrButton", L"PauseButton", L"SendButton"}) find<Button>(id).IsEnabled(false);
    poll = dispatcher.CreateTimer(); poll.Interval(100ms);
    poll.Tick([this](auto&&, auto&&) {
        if (activation_event && WaitForSingleObject(activation_event, 0) == WAIT_OBJECT_0) { ResetEvent(activation_event); restore(); if (activation_ack) SetEvent(activation_ack); }
    }); poll.Start();
    auto input_test_window = ui_test_report.empty() ? nullptr : window_handle(window);
    initialization = std::thread([this, input_test_window] {
        try {
            auto next = std::make_shared<std::unique_ptr<vb::Bridge>>(std::make_unique<vb::Bridge>(*config, vb::executable_dir(),
                [input_test_window](auto const& text) { vb::paste_text_to(text, input_test_window); }, [input_test_window] { vb::press_enter_to(input_test_window); },
                [this](auto const& message) { dispatcher.TryEnqueue([this, message] { status(message); }); }));
            if (!override_bind.empty()) { (*next)->displayed_ip = override_bind; (*next)->bind(override_bind, !ui_test_report.empty() ? 0 : override_port ? override_port : 18765); }
            else { if (override_port) (*next)->port = override_port; (*next)->start(); }
            dispatcher.TryEnqueue([this, next] {
                if (closing) return; bridge = std::move(*next);
                for (auto id : {L"QrButton", L"PauseButton", L"SendButton"}) find<Button>(id).IsEnabled(true);
                status(bridge->displayed_ip != bridge->bound_ip ? "已保存地址当前不可用，请打开二维码并点击“刷新地址”" : "等待手机连接");
            });
        } catch (std::exception const& error) { auto message = std::string(error.what()); dispatcher.TryEnqueue([this, message] { status("启动失败：" + message); }); }
          catch (hresult_error const& error) { auto message = to_string(error.message()); dispatcher.TryEnqueue([this, message] { status("启动失败：" + message); }); }
    });
    if (!ui_test_report.empty()) run_ui_tests();
} catch (hresult_error const& error) { auto message = hstring(startup_stage) + L": " + error.message(); log_error(message); if (!isolated) MessageBoxW(nullptr, message.c_str(), L"声桥启动失败", MB_OK | MB_ICONERROR); Exit(); }
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
    find<Grid>(L"Layout").RowDefinitions().GetAt(2).Height(s["show_desktop_to_phone"].get<bool>() ? GridLength{1, GridUnitType::Star} : GridLength{0, GridUnitType::Pixel});
    if (!s["show_desktop_to_phone"].get<bool>() && expanded) { expanded = false; find<Grid>(L"TransferContent").Visibility(Visibility::Collapsed); }
    icon(find<Button>(L"TransferButton"), expanded ? L"up" : L"down");
    icon(find<Button>(L"ThemeButton"), dark ? L"sun" : L"moon"); refresh_count();
}
inline void App::change_theme() {
    dark = !dark; config->update({{"theme", dark ? "dark" : "light"}});
    for (auto const& w : {window, settings_window, qr_window, close_window}) if (w) chrome(w);
    std::erase_if(theme_bindings, [this](auto const& binding) {
        if (auto object = binding.object.get()) { object.SetValue(binding.property, palette(binding.key.c_str())); return false; }
        return true;
    });
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
    RECT rect{}; GetClientRect(window_handle(window), &rect);
    animation_from = rect.bottom;
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
        RECT rect{}; GetClientRect(window_handle(window), &rect);
        client_size(window, rect.right, animation_from + (animation_to - animation_from) * progress);
        if (t >= 1) {
            CompositionTarget::Rendering(rendering); animating = false;
            find<Grid>(L"TransferContent").Visibility(expanded ? Visibility::Visible : Visibility::Collapsed);
            find<Grid>(L"TransferContent").IsHitTestVisible(true);
        }
    });
}
