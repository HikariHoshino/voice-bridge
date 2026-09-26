// Voice Bridge desktop shell: a Win32 window hosting the WebView2 UI in ui/.
// The HTTP service, input injection and settings stay in ../native/BridgeCore.h.
#include "BridgeCore.h"
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <wrl.h>
#include <WebView2.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT WM_BRIDGE_JSON = WM_APP + 1;   // lParam: std::string* owned by the receiver
constexpr UINT WM_ACTIVATE_EXISTING = WM_APP + 2;
constexpr UINT_PTR RESIZE_TIMER = 1, SHOT_TIMER = 2;
constexpr double MIN_WIDTH = 420, RESIZE_SECONDS = 0.2;
constexpr wchar_t HOST[] = L"app.voicebridge";

HWND window{};
ComPtr<ICoreWebView2Controller> controller;
ComPtr<ICoreWebView2> webview;
std::unique_ptr<vb::Config> config;
std::unique_ptr<vb::Bridge> bridge;
std::thread starter;
vb::fs::path config_path, shot_path;
std::string shot_state, override_bind;
int override_port = 0;
bool isolated = false, devtools = false, dark = false, closing = false;
double min_height = 290;
std::string last_status = "正在连接本机服务…";
int resize_from = 0, resize_to = 0; ULONGLONG resize_start = 0;
HANDLE activation_event{}, activation_ack{}, activation_wait{};

double scale() { return GetDpiForWindow(window) / 96.0; }
SIZE frame_extra() {
    RECT outer{}, client{}; GetWindowRect(window, &outer); GetClientRect(window, &client);
    return {outer.right - outer.left - client.right, outer.bottom - outer.top - client.bottom};
}
void set_client_height(int pixels) {
    RECT outer{}; GetWindowRect(window, &outer); auto extra = frame_extra();
    SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, pixels + extra.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void post(vb::json const& message) { if (webview) webview->PostWebMessageAsJson(vb::wide(message.dump()).c_str()); }
void post_from_any_thread(vb::json const& message) {
    auto text = new std::string(message.dump());
    if (!PostMessageW(window, WM_BRIDGE_JSON, 0, reinterpret_cast<LPARAM>(text))) delete text;
}
vb::json settings_message() {
    auto s = config->settings();
    return {{"type", "settings"}, {"theme", s["theme"]}, {"sendMode", s["send_mode"]}, {"showCount", s["show_char_count"]},
        {"showDesktop", s["show_desktop_to_phone"]}, {"count", s["character_count"]}};
}
vb::json bridge_message() {
    if (!bridge) return {{"type", "bridge"}, {"ready", false}};
    return {{"type", "bridge"}, {"ready", true}, {"ip", bridge->displayed_ip}, {"port", bridge->port},
        {"available", bridge->displayed_ip == bridge->bound_ip}};
}
void status(std::string const& text) {
    last_status = text;
    post({{"type", "status"}, {"text", text}, {"count", config->settings()["character_count"]}});
}

void apply_theme() {
    BOOL use_dark = dark; DwmSetWindowAttribute(window, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &use_dark, sizeof(use_dark));
    COLORREF border = dark ? RGB(0x1E, 0x23, 0x2B) : RGB(0xE8, 0xEC, 0xF1);
    DwmSetWindowAttribute(window, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    if (ComPtr<ICoreWebView2Controller2> c2; controller && SUCCEEDED(controller.As(&c2)))
        c2->put_DefaultBackgroundColor(dark ? COREWEBVIEW2_COLOR{255, 0x1E, 0x23, 0x2B} : COREWEBVIEW2_COLOR{255, 0xE8, 0xEC, 0xF1});
}

void exit_app() {
    if (closing) return; closing = true;
    if (activation_wait) { UnregisterWaitEx(activation_wait, INVALID_HANDLE_VALUE); activation_wait = nullptr; }
    if (starter.joinable()) starter.join();
    bridge.reset();
    if (controller) controller->Close();
    DestroyWindow(window);
}

void capture_and_exit() {
    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(shot_path.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream))) { exit_app(); return; }
    webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([stream](HRESULT) -> HRESULT { stream->Commit(STGC_DEFAULT); exit_app(); return S_OK; }).Get());
}

void handle_web_message(vb::json const& message) {
    auto cmd = message.value("cmd", std::string());
    if (cmd == "ready") {
        post(settings_message()); post(bridge_message()); status(last_status);
        if (!shot_path.empty()) { post({{"type", "shot"}, {"state", shot_state}}); SetTimer(window, SHOT_TIMER, 900, nullptr); }
    } else if (cmd == "minimize") ShowWindow(window, SW_MINIMIZE);
    else if (cmd == "close") exit_app();
    else if (cmd == "theme") { dark = message.value("dark", false); config->update({{"theme", dark ? "dark" : "light"}}); apply_theme(); }
    else if (cmd == "layout") {
        min_height = message.value("min", min_height);
        resize_to = static_cast<int>(std::lround(message.value("height", min_height) * scale()));
        RECT client{}; GetClientRect(window, &client); resize_from = client.bottom;
        if (message.value("animate", true) && resize_from != resize_to) { resize_start = GetTickCount64(); SetTimer(window, RESIZE_TIMER, 8, nullptr); }
        else set_client_height(resize_to);
    } else if (cmd == "send") {
        auto text = message.value("text", std::string());
        if (!bridge) { status("本机服务尚未就绪"); return; }
        if (text.empty()) { status("请先输入要发给手机的文字"); return; }
        bridge->queue(text); post({{"type", "sent"}});
        status("已发送 " + std::to_string(vb::character_count(text)) + " 个字符到手机");
    } else if (cmd == "settings" || cmd == "qr") status("该窗口将在下一步迁移到新界面");
}

void create_webview() {
    auto data = config_path.parent_path() / L"WebView2";
    CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            if (FAILED(result)) { MessageBoxW(window, L"无法启动 WebView2，请确认系统已安装 Microsoft Edge WebView2 运行时。", L"声桥启动失败", MB_ICONERROR); exit_app(); return result; }
            return environment->CreateCoreWebView2Controller(window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT result, ICoreWebView2Controller* created) -> HRESULT {
                    if (FAILED(result) || !created) { exit_app(); return result; }
                    controller = created; controller->get_CoreWebView2(&webview); apply_theme();
                    RECT bounds{}; GetClientRect(window, &bounds); controller->put_Bounds(bounds);
                    ComPtr<ICoreWebView2Settings> settings; webview->get_Settings(&settings);
                    settings->put_AreDefaultContextMenusEnabled(FALSE); settings->put_IsStatusBarEnabled(FALSE);
                    settings->put_IsZoomControlEnabled(FALSE); settings->put_AreDevToolsEnabled(devtools);
                    if (ComPtr<ICoreWebView2Settings3> s3; SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(devtools);
                    if (ComPtr<ICoreWebView2Settings9> s9; SUCCEEDED(settings.As(&s9))) s9->put_IsNonClientRegionSupportEnabled(TRUE);
                    if (ComPtr<ICoreWebView2_3> w3; SUCCEEDED(webview.As(&w3)))
                        w3->SetVirtualHostNameToFolderMapping(HOST, (vb::executable_dir() / L"ui").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
                    EventRegistrationToken token{};
                    webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                        LPWSTR uri{}; args->get_Uri(&uri);
                        if (std::wstring_view(uri).rfind(std::wstring(L"https://") + HOST + L"/", 0) != 0) args->put_Cancel(TRUE);
                        CoTaskMemFree(uri); return S_OK;
                    }).Get(), &token);
                    webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                        args->put_Handled(TRUE); return S_OK;
                    }).Get(), &token);
                    webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                        LPWSTR raw{}; if (FAILED(args->get_WebMessageAsJson(&raw))) return S_OK;
                        try { handle_web_message(vb::json::parse(vb::utf8(raw))); }
                        catch (std::exception const& error) { status(error.what()); }
                        CoTaskMemFree(raw); return S_OK;
                    }).Get(), &token);
                    webview->Navigate((std::wstring(L"https://") + HOST + L"/main.html").c_str());
                    return S_OK;
                }).Get());
        }).Get());
}

void start_bridge() {
    starter = std::thread([] {
        try {
            auto next = std::make_unique<vb::Bridge>(*config, vb::executable_dir(), vb::paste_text, vb::press_enter,
                [](std::string const& text) { post_from_any_thread({{"type", "status"}, {"text", text}}); });
            if (!override_bind.empty()) { next->displayed_ip = override_bind; next->bind(override_bind, override_port ? override_port : 18765); }
            else { if (override_port) next->port = override_port; next->start(); }
            auto raw = next.release();
            PostMessageW(window, WM_BRIDGE_JSON, 1, reinterpret_cast<LPARAM>(raw));
        } catch (std::exception const& error) { post_from_any_thread({{"type", "status"}, {"text", std::string("启动失败：") + error.what()}}); }
    });
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_NCCALCSIZE:
        // Drop the system caption but keep the invisible side/bottom resize borders and DWM shadow.
        if (wparam) { auto params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam); auto top = params->rgrc[0].top;
            auto result = DefWindowProcW(hwnd, message, wparam, lparam); params->rgrc[0].top = top; return result; }
        break;
    case WM_GETMINMAXINFO:
        if (window) { auto info = reinterpret_cast<MINMAXINFO*>(lparam); auto extra = frame_extra();
            info->ptMinTrackSize.x = static_cast<LONG>(std::lround(MIN_WIDTH * scale())) + extra.cx;
            info->ptMinTrackSize.y = static_cast<LONG>(std::lround(min_height * scale())) + extra.cy; return 0; }
        break;
    case WM_DPICHANGED: { auto rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
    case WM_SIZE:
        if (controller) { RECT bounds{}; GetClientRect(hwnd, &bounds); controller->put_Bounds(bounds); }
        if (controller) controller->put_IsVisible(wparam != SIZE_MINIMIZED);
        return 0;
    case WM_TIMER:
        if (wparam == RESIZE_TIMER) {
            auto t = std::min(1.0, (GetTickCount64() - resize_start) / 1000.0 / RESIZE_SECONDS), eased = t * t * (3 - 2 * t);
            set_client_height(static_cast<int>(std::lround(resize_from + (resize_to - resize_from) * eased)));
            if (t >= 1) KillTimer(hwnd, RESIZE_TIMER);
        } else if (wparam == SHOT_TIMER) { KillTimer(hwnd, SHOT_TIMER); capture_and_exit(); }
        return 0;
    case WM_BRIDGE_JSON:
        if (wparam == 1) { bridge.reset(reinterpret_cast<vb::Bridge*>(lparam)); if (starter.joinable()) starter.join(); post(bridge_message());
            status(bridge->displayed_ip != bridge->bound_ip ? "地址不可用，请刷新二维码" : "等待手机连接"); }
        else { std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lparam));
            auto update = vb::json::parse(*text);
            if (update["type"] == "status") { last_status = update["text"]; update["count"] = config->settings()["character_count"]; }
            post(update); }
        return 0;
    case WM_ACTIVATE_EXISTING:
        ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(hwnd);
        if (activation_ack) SetEvent(activation_ack); return 0;
    case WM_CLOSE: exit_app(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE mutex{};
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        config_path = vb::default_config();
        int count{}; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        for (int i = 1; i < count; ++i) {
            std::wstring argument(args[i]);
            if (argument == L"--config" && i + 1 < count) { config_path = args[++i]; isolated = true; }
            else if (argument == L"--port" && i + 1 < count) override_port = std::stoi(args[++i]);
            else if (argument == L"--bind" && i + 1 < count) override_bind = vb::utf8(args[++i]);
            else if (argument == L"--screenshot" && i + 1 < count) shot_path = args[++i];
            else if (argument == L"--shot-state" && i + 1 < count) shot_state = vb::utf8(args[++i]);
            else if (argument == L"--devtools") devtools = true;
            else throw std::runtime_error("Unknown command line argument");
        }
        LocalFree(args);
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
        WNDCLASSEXW type{sizeof(type)};
        type.lpfnWndProc = window_proc; type.hInstance = instance; type.lpszClassName = L"VoiceBridge.WebShell";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1)); type.hIconSm = type.hIcon;
        type.hbrBackground = CreateSolidBrush(dark ? RGB(0x1E, 0x23, 0x2B) : RGB(0xE8, 0xEC, 0xF1));
        RegisterClassExW(&type);
        window = CreateWindowExW(WS_EX_APPWINDOW, type.lpszClassName, L"声桥", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
            CW_USEDEFAULT, CW_USEDEFAULT, 600, 400, nullptr, nullptr, instance, nullptr);
        if (!window) winrt::throw_last_error();
        DWORD corner = 2 /*DWMWCP_ROUND*/; DwmSetWindowAttribute(window, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
        apply_theme();
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        { auto extra = frame_extra(); int w = static_cast<int>(std::lround(MIN_WIDTH * scale())) + extra.cx, h = static_cast<int>(std::lround(min_height * scale())) + extra.cy;
          MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor); auto work = monitor.rcWork;
          SetWindowPos(window, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h, SWP_NOZORDER); }
        ShowWindow(window, SW_SHOW);
        RegisterWaitForSingleObject(&activation_wait, activation_event, [](PVOID, BOOLEAN) {
            ResetEvent(activation_event); PostMessageW(window, WM_ACTIVATE_EXISTING, 0, 0);
        }, nullptr, INFINITE, WT_EXECUTEDEFAULT);
        create_webview();
        start_bridge();
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        CloseHandle(activation_event); CloseHandle(activation_ack); CloseHandle(mutex);
        return 0;
    } catch (winrt::hresult_error const& error) { MessageBoxW(nullptr, error.message().c_str(), L"声桥启动失败", MB_OK | MB_ICONERROR); }
      catch (std::exception const& error) { MessageBoxW(nullptr, vb::wide(error.what()).c_str(), L"声桥启动失败", MB_OK | MB_ICONERROR); }
    if (mutex) CloseHandle(mutex);
    return 1;
}
