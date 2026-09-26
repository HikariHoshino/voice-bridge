// Voice Bridge desktop shell: Win32 windows hosting the WebView2 UI pages in ui/.
// The HTTP service, input injection and settings stay in ../native/BridgeCore.h.
#include "BridgeCore.h"
#include "NativeTests.h"
#include "vendor/qrcodegen.hpp"
#include <shellapi.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <map>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT WM_BRIDGE_JSON = WM_APP + 1;   // wParam 0: lParam std::string* message; wParam 1: lParam vb::Bridge*
constexpr UINT WM_ACTIVATE_EXISTING = WM_APP + 2;
constexpr UINT WM_TRAY = WM_APP + 3;
constexpr UINT WM_SHELL_DESTROY = WM_APP + 4;   // deferred: never destroy a WebView inside its own event
constexpr UINT WM_APP_EXIT = WM_APP + 5;
constexpr UINT_PTR SHOT_TIMER = 2, PHONE_TIMER = 3;
constexpr UINT TRAY_OPEN = 1, TRAY_EXIT = 2;
constexpr UINT WM_RUN_RESIZE = WM_APP + 6;
constexpr wchar_t HOST[] = L"app.voicebridge";

struct Shell {
    std::string page;
    HWND hwnd{};
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    double width = 420, min_height = 200;
    bool resizable = false, ready = false;
    int resize_from = 0, resize_to = 0; double resize_seconds = 0.24, final_min_height = 200;
    LONGLONG resize_start = 0; bool resizing = false;
};

std::map<std::string, std::unique_ptr<Shell>> shells;   // keyed by page
ComPtr<ICoreWebView2Environment> environment;
std::unique_ptr<vb::Config> config;
std::unique_ptr<vb::Bridge> bridge;
std::thread starter;
vb::fs::path config_path, shot_path;
std::string shot_state, shot_page, override_bind, last_status = "正在连接本机服务…";
int override_port = 0;
bool isolated = false, devtools = false, dark = false, closing = false, phone_connected = false, tray_added = false;
HANDLE activation_event{}, activation_ack{}, activation_wait{};
HINSTANCE instance{};
HICON app_icon{}, tray_icon{};
UINT taskbar_created = 0;

Shell* shell(std::string const& page) { auto it = shells.find(page); return it == shells.end() ? nullptr : it->second.get(); }
HWND main_window() { auto s = shell("main"); return s ? s->hwnd : nullptr; }
double scale(HWND hwnd) { return GetDpiForWindow(hwnd) / 96.0; }
SIZE frame_extra(HWND hwnd) {
    RECT outer{}, client{}; GetWindowRect(hwnd, &outer); GetClientRect(hwnd, &client);
    return {outer.right - outer.left - client.right, outer.bottom - outer.top - client.bottom};
}
void set_client_height(HWND hwnd, int pixels) {
    RECT outer{}; GetWindowRect(hwnd, &outer); auto extra = frame_extra(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, outer.right - outer.left, pixels + extra.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
COLORREF surface() { return dark ? RGB(0x1E, 0x23, 0x2B) : RGB(0xE8, 0xEC, 0xF1); }

// Same curve as the page's CSS cubic-bezier(.4,0,.2,1), so the window edge and the panel move together.
double ease(double x) {
    auto bezier = [](double t, double a, double b) { double u = 1 - t; return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t; };
    double lo = 0, hi = 1, t = x;
    for (int i = 0; i < 30; ++i) { t = (lo + hi) / 2; (bezier(t, .4, .2) < x ? lo : hi) = t; }
    return bezier(t, 0, 1);
}
LONGLONG now_ticks() { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }

void post(Shell* target, vb::json const& message) {
    if (target && target->webview && target->ready) target->webview->PostWebMessageAsJson(vb::wide(message.dump()).c_str());
}
void broadcast(vb::json const& message) { for (auto& [page, s] : shells) post(s.get(), message); }
void post_from_any_thread(vb::json const& message) {
    auto text = new std::string(message.dump());
    if (!main_window() || !PostMessageW(main_window(), WM_BRIDGE_JSON, 0, reinterpret_cast<LPARAM>(text))) delete text;
}
vb::json settings_message() {
    auto s = config->settings();
    return {{"type", "settings"}, {"theme", s["theme"]}, {"sendMode", s["send_mode"]}, {"closeAction", s["close_action"]},
        {"showCount", s["show_char_count"]}, {"showDesktop", s["show_desktop_to_phone"]}, {"count", s["character_count"]},
        {"autostart", !isolated && vb::autostart_enabled()}, {"autostartAvailable", !isolated}};
}
vb::json bridge_message() {
    if (!bridge) return {{"type", "bridge"}, {"ready", false}};
    return {{"type", "bridge"}, {"ready", true}, {"ip", bridge->displayed_ip}, {"port", bridge->port},
        {"available", bridge->displayed_ip == bridge->bound_ip}};
}
vb::json qr_message() {
    if (!bridge) return {{"type", "qr"}, {"ready", false}};
    auto url = bridge->url();
    auto code = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    vb::json rows = vb::json::array();
    for (int y = 0; y < code.getSize(); ++y) { std::string row; for (int x = 0; x < code.getSize(); ++x) row += code.getModule(x, y) ? '1' : '0'; rows.push_back(row); }
    auto shown = url.substr(0, url.find("token=") + 6) + std::string(12, '*');
    return {{"type", "qr"}, {"ready", true}, {"rows", rows}, {"address", shown}, {"available", bridge->displayed_ip == bridge->bound_ip}};
}
void status(std::string const& text) {
    last_status = text;
    broadcast({{"type", "status"}, {"text", text}, {"count", config->settings()["character_count"]}});
}

void apply_theme(Shell* s) {
    BOOL use_dark = dark; DwmSetWindowAttribute(s->hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &use_dark, sizeof(use_dark));
    COLORREF border = surface(); DwmSetWindowAttribute(s->hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    if (ComPtr<ICoreWebView2Controller2> c2; s->controller && SUCCEEDED(s->controller.As(&c2)))
        c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, GetRValue(border), GetGValue(border), GetBValue(border)});
}

void tray(DWORD action) {
    auto hwnd = main_window(); if (!hwnd) return;
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = hwnd; data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP; data.uCallbackMessage = WM_TRAY; data.hIcon = tray_icon;
    wcscpy_s(data.szTip, L"声桥");
    if (action == NIM_ADD) { tray_added = Shell_NotifyIconW(NIM_ADD, &data); data.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &data); }
    else if (tray_added) { Shell_NotifyIconW(NIM_DELETE, &data); tray_added = false; }
}

void close_shell(std::string const& page) {
    auto s = shell(page); if (!s || page == "main") return;
    PostMessageW(s->hwnd, WM_SHELL_DESTROY, 0, 0);
}
void close_secondary() { for (auto page : {"settings", "qr", "close"}) close_shell(page); }

void exit_app() { if (main_window()) PostMessageW(main_window(), WM_APP_EXIT, 0, 0); else PostQuitMessage(0); }
void finish_exit() {
    if (closing) return; closing = true;
    if (activation_wait) { UnregisterWaitEx(activation_wait, INVALID_HANDLE_VALUE); activation_wait = nullptr; }
    if (starter.joinable()) starter.join();
    bridge.reset();
    tray(NIM_DELETE);
    for (auto page : {"settings", "qr", "close"}) if (auto s = shell(page)) DestroyWindow(s->hwnd);
    if (auto s = shell("main")) DestroyWindow(s->hwnd);
}
void restore_main() {
    auto hwnd = main_window(); if (!hwnd) return;
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(hwnd);
}
void minimize_to_tray() { close_secondary(); ShowWindow(main_window(), SW_HIDE); status("声桥已最小化到托盘，服务保持运行"); }

void open_shell(std::string const& page, double width, double min_height, bool resizable);
void request_close() {
    auto action = config->settings()["close_action"].get<std::string>();
    if (action == "exit") exit_app();
    else if (action == "tray") minimize_to_tray();
    else if (auto s = shell("close")) SetForegroundWindow(s->hwnd);
    else { open_shell("close", 400, 180, false); EnableWindow(main_window(), FALSE); }
}

void capture_and_exit(Shell* s) {
    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(shot_path.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream))) { exit_app(); return; }
    s->webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([stream](HRESULT) -> HRESULT { stream->Commit(STGC_DEFAULT); exit_app(); return S_OK; }).Get());
}

bool one_of(vb::json const& value, std::initializer_list<char const*> allowed) {
    return value.is_string() && std::any_of(allowed.begin(), allowed.end(), [&](char const* a) { return value.get<std::string>() == a; });
}

void handle_web_message(Shell* s, vb::json const& message) {
    auto cmd = message.value("cmd", std::string());
    if (cmd == "ready") {
        s->ready = true;
        post(s, settings_message()); post(s, bridge_message()); post(s, {{"type", "phone"}, {"connected", phone_connected}});
        post(s, {{"type", "status"}, {"text", last_status}, {"count", config->settings()["character_count"]}});
        if (s->page == "qr") post(s, qr_message());
        if (!shot_path.empty()) {
            auto target = shot_page.empty() ? std::string("main") : shot_page;
            if (s->page == "main" && target != "main") { if (target == "close") request_close(); else open_shell(target, 420, 200, false); }
            if (s->page == target) { post(s, {{"type", "shot"}, {"state", shot_state}}); SetTimer(s->hwnd, SHOT_TIMER, 900, nullptr); }
        }
    } else if (cmd == "layout") {
        auto min = message.value("min", s->min_height);
        s->resize_to = static_cast<int>(std::lround(message.value("height", min) * scale(s->hwnd)));
        RECT client{}; GetClientRect(s->hwnd, &client); s->resize_from = client.bottom;
        if (message.value("animate", false) && s->resize_from != s->resize_to) {
            // The minimum only applies once the animation lands; applying it first would snap the window.
            s->final_min_height = min; s->min_height = std::min(min, client.bottom / scale(s->hwnd));
            s->resize_seconds = message.value("duration", 240) / 1000.0; s->resize_start = now_ticks();
            if (!s->resizing) PostMessageW(s->hwnd, WM_RUN_RESIZE, 0, 0);
        } else { s->min_height = min; set_client_height(s->hwnd, s->resize_to); }
        if (s->page != "main" && !IsWindowVisible(s->hwnd)) {
            if (auto owner = GetWindow(s->hwnd, GW_OWNER)) {
                RECT o{}, r{}; GetWindowRect(owner, &o); GetWindowRect(s->hwnd, &r);
                SetWindowPos(s->hwnd, nullptr, o.left + (o.right - o.left - (r.right - r.left)) / 2, o.top + (o.bottom - o.top - (r.bottom - r.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
            ShowWindow(s->hwnd, SW_SHOW); SetForegroundWindow(s->hwnd);
        }
    } else if (cmd == "minimize") ShowWindow(s->hwnd, SW_MINIMIZE);
    else if (cmd == "close") { if (s->page == "main") request_close(); else close_shell(s->page); }
    else if (cmd == "theme") {
        dark = message.value("dark", false); config->update({{"theme", dark ? "dark" : "light"}});
        for (auto& [page, other] : shells) apply_theme(other.get());
        broadcast({{"type", "theme"}, {"dark", dark}});
    } else if (cmd == "send") {
        auto text = message.value("text", std::string());
        if (!bridge) { status("本机服务尚未就绪"); return; }
        if (text.empty()) { status("请先输入要发给手机的文字"); return; }
        bridge->queue(text); post(s, {{"type", "sent"}});
        status("已发送 " + std::to_string(vb::character_count(text)) + " 个字符到手机");
    } else if (cmd == "open") {
        auto page = message.value("page", std::string());
        if (page == "settings" || (page == "qr" && bridge)) {
            if (auto existing = shell(page)) SetForegroundWindow(existing->hwnd); else open_shell(page, 420, 200, false);
        }
    } else if (cmd == "save") {
        auto values = message.value("values", vb::json::object()), update = vb::json::object();
        for (auto key : {"show_char_count", "show_desktop_to_phone"}) if (values.contains(key) && values[key].is_boolean()) update[key] = values[key];
        if (one_of(values["close_action"], {"ask", "tray", "exit"})) update["close_action"] = values["close_action"];
        if (one_of(values["send_mode"], {"ctrl_enter", "enter"})) update["send_mode"] = values["send_mode"];
        config->update(update);
        if (!isolated && values.contains("autostart") && values["autostart"].is_boolean() && values["autostart"].get<bool>() != vb::autostart_enabled())
            vb::set_autostart(values["autostart"]);
        broadcast(settings_message()); close_shell("settings"); status("设置已保存");
    } else if (cmd == "clear_count") {
        config->update({{"character_count", 0}}); broadcast(settings_message()); status("累计字符已清零");
    } else if (cmd == "copy" && bridge) { vb::clipboard(vb::wide(bridge->url())); status("手机访问地址已复制"); post(s, {{"type", "copied"}}); }
    else if (cmd == "refresh" && bridge) { bridge->refresh(); broadcast(bridge_message()); post(s, qr_message()); }
    else if (cmd == "rotate" && bridge) { bridge->rotate(); post(s, qr_message()); status("配对链接已更新，请重新扫码"); }
    else if (cmd == "tray") { close_shell("close"); minimize_to_tray(); }
    else if (cmd == "exit") exit_app();
}

LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);

void attach_webview(Shell* s) {
    environment->CreateCoreWebView2Controller(s->hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [page = s->page](HRESULT result, ICoreWebView2Controller* created) -> HRESULT {
            auto s = shell(page);
            if (!s) { if (created) created->Close(); return S_OK; }
            if (FAILED(result) || !created) { if (page == "main") exit_app(); else close_shell(page); return result; }
            s->controller = created; s->controller->get_CoreWebView2(&s->webview); apply_theme(s);
            RECT bounds{}; GetClientRect(s->hwnd, &bounds); s->controller->put_Bounds(bounds);
            ComPtr<ICoreWebView2Settings> settings; s->webview->get_Settings(&settings);
            settings->put_AreDefaultContextMenusEnabled(FALSE); settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE); settings->put_AreDevToolsEnabled(devtools);
            if (ComPtr<ICoreWebView2Settings3> s3; SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(devtools);
            if (ComPtr<ICoreWebView2Settings9> s9; SUCCEEDED(settings.As(&s9))) s9->put_IsNonClientRegionSupportEnabled(TRUE);
            if (ComPtr<ICoreWebView2_3> w3; SUCCEEDED(s->webview.As(&w3)))
                w3->SetVirtualHostNameToFolderMapping(HOST, (vb::executable_dir() / L"ui").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
            EventRegistrationToken token{};
            s->webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri{}; args->get_Uri(&uri);
                if (std::wstring_view(uri).rfind(std::wstring(L"https://") + HOST + L"/", 0) != 0) args->put_Cancel(TRUE);
                CoTaskMemFree(uri); return S_OK;
            }).Get(), &token);
            s->webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE); return S_OK;
            }).Get(), &token);
            s->webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([page](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR raw{}; if (FAILED(args->get_WebMessageAsJson(&raw))) return S_OK;
                try { if (auto target = shell(page)) handle_web_message(target, vb::json::parse(vb::utf8(raw))); }
                catch (winrt::hresult_error const& error) { status(winrt::to_string(error.message())); }
                catch (std::exception const& error) { status(error.what()); }
                CoTaskMemFree(raw); return S_OK;
            }).Get(), &token);
            s->webview->Navigate((std::wstring(L"https://") + HOST + L"/" + vb::wide(page) + L".html").c_str());
            return S_OK;
        }).Get());
}

void open_shell(std::string const& page, double width, double min_height, bool resizable) {
    auto s = std::make_unique<Shell>(); s->page = page; s->width = width; s->min_height = min_height; s->resizable = resizable;
    auto main = page == "main";
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | (main ? WS_MINIMIZEBOX : 0);
    auto title = main ? L"声桥" : page == "settings" ? L"声桥设置" : page == "qr" ? L"手机配对" : L"关闭声桥";
    s->hwnd = CreateWindowExW(main ? WS_EX_APPWINDOW : 0, L"VoiceBridge.WebShell", title, style, CW_USEDEFAULT, CW_USEDEFAULT, 600, 400,
        main ? nullptr : main_window(), nullptr, instance, nullptr);
    if (!s->hwnd) winrt::throw_last_error();
    SetWindowLongPtrW(s->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s.get()));
    DWORD corner = 2 /*DWMWCP_ROUND*/; DwmSetWindowAttribute(s->hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    apply_theme(s.get());
    SetWindowPos(s->hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    auto extra = frame_extra(s->hwnd);
    int w = static_cast<int>(std::lround(width * scale(s->hwnd))) + extra.cx, h = static_cast<int>(std::lround(min_height * scale(s->hwnd))) + extra.cy;
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(main ? s->hwnd : main_window(), MONITOR_DEFAULTTONEAREST), &monitor); auto work = monitor.rcWork;
    SetWindowPos(s->hwnd, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h, SWP_NOZORDER);
    auto raw = s.get(); shells[page] = std::move(s);
    if (main) ShowWindow(raw->hwnd, SW_SHOW);   // secondary windows appear once their page has reported its height
    attach_webview(raw);
}

void start_bridge() {
    starter = std::thread([] {
        try {
            auto next = std::make_unique<vb::Bridge>(*config, vb::executable_dir(), vb::paste_text, vb::press_enter,
                [](std::string const& text) { post_from_any_thread({{"type", "status"}, {"text", text}}); });
            if (!override_bind.empty()) { next->displayed_ip = override_bind; next->bind(override_bind, override_port ? override_port : 18765); }
            else { if (override_port) next->port = override_port; next->start(); }
            auto raw = next.release();
            if (!main_window() || !PostMessageW(main_window(), WM_BRIDGE_JSON, 1, reinterpret_cast<LPARAM>(raw))) delete raw;
        } catch (std::exception const& error) { post_from_any_thread({{"type", "status"}, {"text", std::string("启动失败：") + error.what()}}); }
    });
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto s = reinterpret_cast<Shell*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == taskbar_created && s && s->page == "main") { tray(NIM_ADD); return 0; }
    switch (message) {
    case WM_NCCALCSIZE:
        // Drop the system caption but keep the invisible side/bottom resize borders and the DWM shadow.
        if (wparam) { auto params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam); auto top = params->rgrc[0].top;
            auto result = DefWindowProcW(hwnd, message, wparam, lparam); params->rgrc[0].top = top; return result; }
        break;
    case WM_NCHITTEST: {
        auto hit = DefWindowProcW(hwnd, message, wparam, lparam);
        if (s && !s->resizable && hit >= HTLEFT && hit <= HTBOTTOMRIGHT) return HTBORDER;
        return hit; }
    case WM_GETMINMAXINFO:
        if (s) { auto info = reinterpret_cast<MINMAXINFO*>(lparam); auto extra = frame_extra(hwnd);
            info->ptMinTrackSize.x = static_cast<LONG>(std::lround(s->width * scale(hwnd))) + extra.cx;
            info->ptMinTrackSize.y = static_cast<LONG>(std::lround(s->min_height * scale(hwnd))) + extra.cy; return 0; }
        break;
    case WM_DPICHANGED: { auto rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
    case WM_SIZE:
        if (s && s->controller) { RECT bounds{}; GetClientRect(hwnd, &bounds); s->controller->put_Bounds(bounds); s->controller->put_IsVisible(wparam != SIZE_MINIMIZED); }
        return 0;
    case WM_SETFOCUS:
        if (s && s->controller) s->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        return 0;
    case WM_TIMER:
        if (!s) return 0;
        if (wparam == SHOT_TIMER) { KillTimer(hwnd, SHOT_TIMER); capture_and_exit(s); }
        else if (wparam == PHONE_TIMER && bridge) {
            auto active = bridge->phone_active();
            if (active != phone_connected) { phone_connected = active; broadcast({{"type", "phone"}, {"connected", active}}); }
        }
        return 0;
    case WM_BRIDGE_JSON:
        if (wparam == 1) {
            bridge.reset(reinterpret_cast<vb::Bridge*>(lparam)); if (starter.joinable()) starter.join();
            broadcast(bridge_message()); SetTimer(hwnd, PHONE_TIMER, 1000, nullptr);
            status(bridge->displayed_ip != bridge->bound_ip ? "地址不可用，请打开二维码刷新地址" : "等待手机连接");
        } else {
            std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lparam));
            auto update = vb::json::parse(*text);
            if (update["type"] == "status") status(update["text"]); else broadcast(update);
        }
        return 0;
    case WM_TRAY:
        switch (LOWORD(lparam)) {
        case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONDBLCLK: restore_main(); break;
        case WM_CONTEXTMENU: {
            auto menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, TRAY_OPEN, L"打开声桥"); AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, TRAY_EXIT, L"退出");
            SetForegroundWindow(hwnd);
            auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam), 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (choice == TRAY_OPEN) restore_main(); else if (choice == TRAY_EXIT) exit_app();
            break; }
        }
        return 0;
    case WM_RUN_RESIZE: {
        // One step per DWM composition (vsync) instead of a coarse WM_TIMER; messages keep flowing meanwhile.
        if (!s || s->resizing) return 0;
        s->resizing = true;
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        bool quit = false; int quit_code = 0;
        for (;;) {
            auto t = std::min(1.0, (now_ticks() - s->resize_start) / static_cast<double>(frequency.QuadPart) / s->resize_seconds);
            set_client_height(hwnd, static_cast<int>(std::lround(s->resize_from + (s->resize_to - s->resize_from) * ease(t))));
            if (t >= 1) break;
            DwmFlush();
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { quit = true; quit_code = static_cast<int>(msg.wParam); break; }
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            if (quit || reinterpret_cast<Shell*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)) != s) break;
        }
        if (reinterpret_cast<Shell*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)) == s) { s->resizing = false; s->min_height = s->final_min_height; }
        if (quit) PostQuitMessage(quit_code);
        return 0; }
    case WM_SHELL_DESTROY:
        if (s && s->page == "close" && main_window()) EnableWindow(main_window(), TRUE);
        DestroyWindow(hwnd); return 0;
    case WM_APP_EXIT: finish_exit(); return 0;
    case WM_ACTIVATE_EXISTING:
        restore_main(); if (activation_ack) SetEvent(activation_ack); return 0;
    case WM_CLOSE:
        if (s && s->page == "main" && !closing) request_close(); else if (s) close_shell(s->page);
        return 0;
    case WM_DESTROY:
        if (s) {
            auto page = s->page;
            if (s->controller) s->controller->Close();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); shells.erase(page);
            if (page == "main") PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE, PWSTR, int) {
    HANDLE mutex{};
    instance = hinstance;
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        config_path = vb::default_config();
        vb::fs::path test_report;
        int count{}; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        for (int i = 1; i < count; ++i) {
            std::wstring argument(args[i]);
            if (argument == L"--config" && i + 1 < count) { config_path = args[++i]; isolated = true; }
            else if (argument == L"--port" && i + 1 < count) override_port = std::stoi(args[++i]);
            else if (argument == L"--bind" && i + 1 < count) override_bind = vb::utf8(args[++i]);
            else if (argument == L"--screenshot" && i + 1 < count) shot_path = args[++i];
            else if (argument == L"--shot-state" && i + 1 < count) shot_state = vb::utf8(args[++i]);
            else if (argument == L"--shot-page" && i + 1 < count) shot_page = vb::utf8(args[++i]);
            else if (argument == L"--self-test" && i + 1 < count) test_report = args[++i];
            else if (argument == L"--devtools") devtools = true;
            else throw std::runtime_error("Unknown command line argument");
        }
        LocalFree(args);
        if (!test_report.empty()) return vb::run_tests(test_report);
        winrt::check_hresult(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        auto suffix = isolated ? L".WebTest." + std::to_wstring(std::hash<std::wstring>{}(config_path.wstring())) : L"";
        mutex = CreateMutexW(nullptr, FALSE, (std::wstring(L"Local\\VoiceBridge.SingleInstance") + suffix).c_str());
        if (!mutex) winrt::throw_last_error();
        bool existing = GetLastError() == ERROR_ALREADY_EXISTS;
        activation_event = CreateEventW(nullptr, TRUE, FALSE, (std::wstring(L"Local\\VoiceBridge.ActivateExisting") + suffix).c_str());
        activation_ack = CreateEventW(nullptr, TRUE, FALSE, (std::wstring(L"Local\\VoiceBridge.ActivateExisting.Acknowledged") + suffix).c_str());
        if (existing) { AllowSetForegroundWindow(ASFW_ANY); ResetEvent(activation_ack); SetEvent(activation_event); WaitForSingleObject(activation_ack, 3000); CloseHandle(mutex); return 0; }

        config = std::make_unique<vb::Config>(config_path);
        dark = config->settings()["theme"] == "dark";
        app_icon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        tray_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSEXW type{sizeof(type)};
        type.lpfnWndProc = window_proc; type.hInstance = instance; type.lpszClassName = L"VoiceBridge.WebShell";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hIcon = app_icon; type.hIconSm = app_icon;
        type.hbrBackground = CreateSolidBrush(surface());
        RegisterClassExW(&type);
        auto data = config_path.parent_path() / L"WebView2";
        winrt::check_hresult(CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT result, ICoreWebView2Environment* created) -> HRESULT {
                if (FAILED(result) || !created) {
                    MessageBoxW(nullptr, L"无法启动 WebView2，请确认系统已安装 Microsoft Edge WebView2 运行时。", L"声桥启动失败", MB_ICONERROR);
                    PostQuitMessage(1); return S_OK;
                }
                environment = created;
                try {
                    open_shell("main", 420, 290, true);
                    tray(NIM_ADD);
                    RegisterWaitForSingleObject(&activation_wait, activation_event, [](PVOID, BOOLEAN) {
                        ResetEvent(activation_event); PostMessageW(main_window(), WM_ACTIVATE_EXISTING, 0, 0);
                    }, nullptr, INFINITE, WT_EXECUTEDEFAULT);
                    start_bridge();
                } catch (std::exception const& error) { MessageBoxW(nullptr, vb::wide(error.what()).c_str(), L"声桥启动失败", MB_ICONERROR); PostQuitMessage(1); }
                return S_OK;
            }).Get()));
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        CloseHandle(activation_event); CloseHandle(activation_ack); CloseHandle(mutex);
        return static_cast<int>(msg.wParam);
    } catch (winrt::hresult_error const& error) { MessageBoxW(nullptr, error.message().c_str(), L"声桥启动失败", MB_OK | MB_ICONERROR); }
      catch (std::exception const& error) { MessageBoxW(nullptr, vb::wide(error.what()).c_str(), L"声桥启动失败", MB_OK | MB_ICONERROR); }
    if (mutex) CloseHandle(mutex);
    return 1;
}
