#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomationClient.h>
#include <winrt/base.h>
#include "vendor/json.hpp"
#include "vendor/httplib.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>
#include <functional>

namespace vb {
using json = nlohmann::json;
namespace fs = std::filesystem;
inline std::wstring wide(std::string const& s) { return std::wstring(winrt::to_hstring(s)); }
inline std::string utf8(std::wstring_view s) { return winrt::to_string(s); }
inline fs::path executable_dir() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return fs::path(path).parent_path();
}
inline std::string read_file(fs::path const& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(file), {}};
}
inline std::string random_token(size_t size = 32) {
    std::vector<unsigned char> bytes(size);
    winrt::check_nt(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    unsigned value = 0, bits = 0;
    for (auto byte : bytes) {
        value = (value << 8) | byte; bits += 8;
        while (bits >= 6) { bits -= 6; result += alphabet[(value >> bits) & 63]; }
    }
    if (bits) result += alphabet[(value << (6 - bits)) & 63];
    return result;
}
inline size_t character_count(std::string const& text) {
    return std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
}
inline fs::path default_config() {
    wchar_t path[32768]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, 32768)) throw std::runtime_error("LOCALAPPDATA unavailable");
    return fs::path(path) / L"VoiceBridge" / L"config.json";
}
inline json defaults() {
    return {{"close_action", "ask"}, {"send_mode", "ctrl_enter"}, {"show_char_count", true},
        {"theme", "light"}, {"character_count", 0}, {"confirm_enter", true},
        {"confirm_send", false}, {"show_desktop_to_phone", true}};
}
class Config {
    fs::path path_;
    std::mutex mutex_;
    json data_;
    void save_locked() {
        fs::create_directories(path_.parent_path());
        auto temporary = path_; temporary += L".tmp";
        { std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
          out << data_.dump(-1, ' ', false, json::error_handler_t::strict);
          out.flush(); if (!out) throw std::runtime_error("无法保存设置"); }
        if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            winrt::throw_last_error();
    }
public:
    explicit Config(fs::path path) : path_(std::move(path)), data_(json::object()) {
        try { auto parsed = json::parse(read_file(path_)); if (parsed.is_object()) data_ = parsed; } catch (...) {}
    }
    json all() { std::lock_guard lock(mutex_); return data_; }
    json settings() {
        auto data = all(), result = defaults();
        for (auto const& [key, value] : result.items()) {
            if (!data.contains(key)) continue;
            if (value.is_boolean() && data[key].is_boolean()) result[key] = data[key];
        }
        for (auto const& [key, allowed] : std::vector<std::pair<std::string, std::vector<std::string>>>{
            {"close_action", {"ask", "tray", "exit"}}, {"send_mode", {"ctrl_enter", "enter"}}, {"theme", {"light", "dark"}}})
            if (data.contains(key) && data[key].is_string() && std::find(allowed.begin(), allowed.end(), data[key].get<std::string>()) != allowed.end()) result[key] = data[key];
        if (data.contains("character_count") && data["character_count"].is_number_integer() && data["character_count"].get<int64_t>() >= 0)
            result["character_count"] = data["character_count"];
        return result;
    }
    void update(json const& values) {
        std::lock_guard lock(mutex_); auto previous = data_; data_.update(values);
        try { save_locked(); } catch (...) { data_ = previous; throw; }
    }
    int64_t add_count(size_t amount) {
        std::lock_guard lock(mutex_);
        int64_t count = 0;
        if (data_.contains("character_count") && data_["character_count"].is_number_integer()) count = std::max(int64_t(0), data_["character_count"].get<int64_t>());
        data_["character_count"] = count + amount; save_locked(); return count + amount;
    }
    std::string token(bool rotate = false) {
        auto data = all();
        if (!rotate && data.contains("token") && data["token"].is_string() && data["token"].get<std::string>().size() >= 32) return data["token"];
        auto token = random_token(); update({{"token", token}}); return token;
    }
};

struct NoInputFocus : std::runtime_error { using std::runtime_error::runtime_error; };
inline std::vector<int64_t> input_target() {
    auto foreground = GetForegroundWindow(); if (!foreground) return {};
    DWORD process = 0; auto thread = GetWindowThreadProcessId(foreground, &process);
    GUITHREADINFO info{sizeof(info)};
    if (GetGUIThreadInfo(thread, &info) && info.hwndFocus) {
        wchar_t name[256]{}; GetClassNameW(info.hwndFocus, name, 256);
        std::wstring type(name); std::transform(type.begin(), type.end(), type.begin(), towlower);
        if (info.hwndCaret || type == L"edit" || type.find(L"richedit") != std::wstring::npos || type.find(L".edit.") != std::wstring::npos)
            return {reinterpret_cast<int64_t>(foreground), reinterpret_cast<int64_t>(info.hwndFocus)};
    }
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<int64_t> result;
    try {
        auto automation = winrt::create_instance<IUIAutomation>(CLSID_CUIAutomation);
        winrt::com_ptr<IUIAutomationElement> element;
        winrt::check_hresult(automation->GetFocusedElement(element.put()));
        int pid = 0, type = 0; BOOL enabled = FALSE;
        winrt::check_hresult(element->get_CurrentProcessId(&pid));
        winrt::check_hresult(element->get_CurrentControlType(&type));
        winrt::check_hresult(element->get_CurrentIsEnabled(&enabled));
        if (pid != static_cast<int>(process) || !enabled) throw std::runtime_error("not editable");
        winrt::com_ptr<IUIAutomationValuePattern> value;
        element->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern), value.put_void());
        if (value) {
            BOOL read_only = TRUE; winrt::check_hresult(value->get_CurrentIsReadOnly(&read_only));
            if (read_only) throw std::runtime_error("read only");
        } else {
            BSTR role{}; element->get_CurrentAriaRole(&role);
            std::wstring aria(role ? role : L""); SysFreeString(role);
            winrt::com_ptr<IUnknown> pattern; element->GetCurrentPattern(UIA_TextPatternId, pattern.put());
            // Some applications expose a custom control type, but still
            // provide a writable text pattern. The pattern is the reliable
            // indication that the focused element is an input target.
            if (!pattern || (aria != L"textbox" && aria != L"searchbox" && type != UIA_EditControlTypeId && type != UIA_DocumentControlTypeId)) throw std::runtime_error("not textbox");
        }
        SAFEARRAY* id{}; winrt::check_hresult(element->GetRuntimeId(&id));
        if (id) {
            LONG first{}, last{}; SafeArrayGetLBound(id, 1, &first); SafeArrayGetUBound(id, 1, &last);
            result = {reinterpret_cast<int64_t>(foreground), process};
            for (LONG i = first; i <= last; ++i) { int number{}; SafeArrayGetElement(id, &i, &number); result.push_back(number); }
            SafeArrayDestroy(id);
        }
    } catch (...) { result.clear(); }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
inline std::vector<int64_t> require_input_target() {
    // Browser and QQ UIA providers can publish the focused element a few
    // milliseconds after the window receives focus. Allow that handoff to
    // settle instead of rejecting the first phone send after startup.
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto target = input_target();
        if (!target.empty()) return target;
        if (attempt != 3) Sleep(25);
    }
    throw NoInputFocus("电脑端未检测到输入光标，请先在电脑输入框中点击一下");
}
inline void clipboard(std::wstring const& text) {
    auto owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    if (!owner) winrt::throw_last_error();
    bool opened = false;
    for (int attempt = 0; attempt < 20 && !(opened = OpenClipboard(owner)); ++attempt) Sleep(10);
    if (!opened) { DestroyWindow(owner); throw std::runtime_error("无法打开剪贴板"); }
    auto memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    bool success = false;
    if (memory) {
        auto destination = GlobalLock(memory);
        if (destination) {
            memcpy(destination, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(memory);
            success = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
        }
        if (!success) GlobalFree(memory);
    }
    CloseClipboard(); DestroyWindow(owner);
    if (!success) throw std::runtime_error("无法写入剪贴板");
}
inline void send_keys(std::initializer_list<std::pair<WORD, bool>> keys) {
    std::vector<INPUT> inputs;
    for (auto [key, up] : keys) { INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wVk = key; input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0; inputs.push_back(input); }
    if (SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) != inputs.size()) {
        for (auto& input : inputs) input.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        throw std::runtime_error("Windows 阻止了向当前光标输入文字");
    }
}
inline void paste_text_to(std::string const& text, HWND expected_window) {
    auto target = require_input_target();
    if (expected_window && target.front() != reinterpret_cast<int64_t>(expected_window)) throw NoInputFocus("测试输入窗口未获得焦点，未发送任何按键");
    clipboard(wide(text));
    if (require_input_target() != target) throw NoInputFocus("电脑端输入光标已变化，请重新点击输入框后再发送");
    send_keys({{WORD(VK_CONTROL), false}, {WORD('V'), false}, {WORD('V'), true}, {WORD(VK_CONTROL), true}});
}
inline void paste_text(std::string const& text) { paste_text_to(text, nullptr); }
inline void press_enter_to(HWND expected_window) {
    auto target = require_input_target();
    if (expected_window && target.front() != reinterpret_cast<int64_t>(expected_window)) throw NoInputFocus("测试输入窗口未获得焦点，未发送任何按键");
    send_keys({{WORD(VK_RETURN), false}, {WORD(VK_RETURN), true}});
}
inline void press_enter() { press_enter_to(nullptr); }

struct Address { std::string ip; bool gateway; };
inline std::vector<Address> addresses() {
    ULONG size = 16384; std::vector<unsigned char> buffer(size);
    auto result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    if (result == ERROR_BUFFER_OVERFLOW) { buffer.resize(size); result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size); }
    if (result != NO_ERROR) return {};
    std::vector<Address> found;
    for (auto adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
            if (entry->Address.lpSockaddr->sa_family != AF_INET) continue;
            char value[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(entry->Address.lpSockaddr)->sin_addr, value, sizeof(value));
            found.push_back({value, adapter->FirstGatewayAddress != nullptr});
        }
    }
    return found;
}
inline std::string choose_ip(std::vector<Address> const& list) {
    bool routed = std::any_of(list.begin(), list.end(), [](auto const& a) { return a.gateway; });
    for (int priority = 0; priority < 4; ++priority) for (auto const& a : list) {
        if (routed && !a.gateway) continue;
        unsigned b1{}, b2{}; sscanf_s(a.ip.c_str(), "%u.%u", &b1, &b2);
        int rank = b1 == 192 && b2 == 168 ? 0 : b1 == 172 && b2 >= 16 && b2 < 32 ? 1 : b1 == 10 ? 2 : 3;
        if (rank == priority && b1 != 169 && b1 != 127) return a.ip;
    }
    return "127.0.0.1";
}
inline bool valid_ip(std::string const& value) {
    in_addr ip{}; return inet_pton(AF_INET, value.c_str(), &ip) == 1 && ip.s_addr != 0 && (ntohl(ip.s_addr) >> 28) != 14;
}
inline std::string base64(std::string const& bytes) {
    DWORD length{};
    CryptBinaryToStringA(reinterpret_cast<BYTE const*>(bytes.data()), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length);
    std::string result(length, '\0');
    winrt::check_bool(CryptBinaryToStringA(reinterpret_cast<BYTE const*>(bytes.data()), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &length));
    result.resize(length); return result;
}

class Bridge {
    struct Host { httplib::Server server; std::thread thread; std::atomic<bool> stopping{false}; };
    Config& config_;
    std::unique_ptr<Host> host_;
    std::mutex mutex_, input_mutex_;
    std::condition_variable condition_;
    std::deque<json> messages_;
    int64_t next_id_ = 1;
    std::string token_, session_ = random_token(8), page_;
    std::function<void(std::string const&)> input_, status_;
    std::function<void()> enter_;
    bool authorized(httplib::Request const& request) {
        auto supplied = request.get_param_value("token"); std::lock_guard lock(mutex_);
        size_t difference = supplied.size() ^ token_.size();
        for (size_t i = 0; i < token_.size(); ++i) difference |= token_[i] ^ (i < supplied.size() ? supplied[i] : 0);
        return difference == 0;
    }
    static void respond(httplib::Response& response, int status, json body) {
        response.status = status; response.set_header("Cache-Control", "no-store");
        response.set_content(body.dump(), "application/json; charset=utf-8");
    }
    static void error(httplib::Response& response, int status, std::string const& message) { respond(response, status, {{"ok", false}, {"error", message}}); }
    static json payload(httplib::Request const& request, size_t limit) {
        if (request.body.empty() || request.body.size() > limit) throw std::invalid_argument("invalid size");
        auto body = json::parse(request.body);
        if (!body.is_object()) throw std::invalid_argument("invalid object");
        return body;
    }
    json mobile_settings() { auto s = config_.settings(); return {{"ok", true}, {"confirm_enter", s["confirm_enter"]}, {"confirm_send", s["confirm_send"]}}; }
    void route(Host& host) {
        host.server.set_payload_max_length(100000);
        host.server.set_read_timeout(5); host.server.set_write_timeout(5);
        host.server.new_task_queue = [] { return new httplib::ThreadPool(16); };
        host.server.set_pre_routing_handler([this](auto const& req, auto& res) {
            if (!authorized(req)) { error(res, 403, "配对链接无效"); return httplib::Server::HandlerResponse::Handled; }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        host.server.Get("/", [this](auto const&, auto& res) {
            status_("手机已连接"); res.set_header("Cache-Control", "no-store"); res.set_content(page_, "text/html; charset=utf-8");
        });
        host.server.Get("/settings", [this](auto const&, auto& res) { respond(res, 200, mobile_settings()); });
        host.server.Post("/settings", [this](auto const& req, auto& res) {
            try {
                auto body = payload(req, 1024), values = json::object();
                for (auto key : {"confirm_enter", "confirm_send"}) if (body.contains(key)) {
                    if (!body[key].is_boolean()) throw std::invalid_argument("boolean required"); values[key] = body[key];
                }
                if (values.empty()) throw std::invalid_argument("empty settings");
                config_.update(values); respond(res, 200, mobile_settings());
            } catch (json::exception const&) { error(res, 400, "设置内容无效"); }
              catch (std::invalid_argument const&) { error(res, 400, "设置内容无效"); }
        });
        host.server.Get("/receive", [this, &host](auto const& req, auto& res) {
            int64_t after = 0;
            try { auto value = req.get_param_value("after"); if (!value.empty()) { size_t used{}; after = std::max(int64_t(0), std::stoll(value, &used)); if (used != value.size()) throw std::invalid_argument("id"); } }
            catch (...) { error(res, 400, "消息序号无效"); return; }
            json found = json::array(); std::string session;
            {
                std::unique_lock lock(mutex_);
                if (req.get_param_value("session") != session_) after = 0;
                condition_.wait_for(lock, std::chrono::seconds(20), [&] { return host.stopping || (!messages_.empty() && messages_.back()["id"].get<int64_t>() > after); });
                for (auto const& item : messages_) if (item["id"].get<int64_t>() > after) found.push_back(item);
                session = session_;
            }
            if (!authorized(req)) { error(res, 403, "配对链接无效"); return; }
            respond(res, 200, {{"ok", true}, {"session", session}, {"messages", found}});
        });
        host.server.Post("/ack", [this](auto const& req, auto& res) {
            try {
                auto body = payload(req, 1024);
                if (!body.contains("session") || !body["session"].is_string() || !body.contains("through") || !body["through"].is_number_integer() || body["through"].get<int64_t>() < 0) throw std::invalid_argument("ack");
                size_t count = 0; std::string session;
                { std::lock_guard lock(mutex_); session = session_;
                  if (body["session"] == session_) while (!messages_.empty() && messages_.front()["id"] <= body["through"]) { messages_.pop_front(); ++count; } }
                if (count) status_("手机已确认接收 " + std::to_string(count) + " 条消息");
                respond(res, 200, {{"ok", true}, {"session", session}, {"acknowledged", count}});
            } catch (...) { error(res, 400, "确认信息无效"); }
        });
        auto input_handler = [this](httplib::Request const& req, httplib::Response& res) {
            std::lock_guard guard(input_mutex_);
            if (!accepting) { error(res, 423, "电脑已暂停接收"); return; }
            try {
                if (req.path == "/enter") { enter_(); status_("已按下回车"); }
                else {
                    auto body = payload(req, 100000);
                    if (!body.contains("text") || !body["text"].is_string() || body["text"].get<std::string>().empty()) throw std::invalid_argument("text");
                    auto text = body["text"].get<std::string>();
                    input_(text); config_.add_count(character_count(text));
                    status_("已输入 " + std::to_string(character_count(text)) + " 个字符");
                }
                respond(res, 200, {{"ok", true}});
            } catch (NoInputFocus const& e) { status_(e.what()); error(res, 409, e.what()); }
              catch (json::exception const&) { error(res, 400, "文字内容无效"); }
              catch (std::invalid_argument const&) { error(res, 400, "文字内容无效"); }
              catch (...) { status_("电脑输入失败"); error(res, 500, "电脑输入失败"); }
        };
        host.server.Post("/send", input_handler); host.server.Post("/enter", input_handler);
        host.server.set_error_handler([](auto const& req, auto& res) {
            if (res.status == 404) error(res, req.method == "POST" ? 403 : 404, req.method == "POST" ? "配对链接无效" : "页面不存在");
            if (res.status == 413) error(res, 400, "内容为空或过长");
        });
        host.server.set_exception_handler([](auto const&, auto& res, std::exception_ptr) { error(res, 500, "电脑服务处理失败"); });
    }
    void stop_host(std::unique_ptr<Host>& host) {
        if (!host) return; host->stopping = true; condition_.notify_all(); host->server.stop();
        if (host->thread.joinable()) host->thread.join(); host.reset();
    }
public:
    std::atomic<bool> accepting{true};
    std::string displayed_ip, bound_ip;
    int port = 8765;
    Bridge(Config& config, fs::path resources, std::function<void(std::string const&)> input = paste_text,
        std::function<void()> enter = press_enter, std::function<void(std::string const&)> status = [](auto const&) {})
        : config_(config), token_(config.token()), input_(std::move(input)), status_(std::move(status)), enter_(std::move(enter)) {
        page_ = read_file(resources / "mobile.html");
        const std::string marker = "{{APP_ICON_BASE64}}";
        auto image = base64(read_file(resources / "assets" / "voice-bridge.png"));
        size_t position = 0;
        while ((position = page_.find(marker, position)) != std::string::npos) {
            page_.replace(position, marker.size(), image); position += image.size();
        }
    }
    ~Bridge() { stop_host(host_); }
    void bind(std::string const& ip, int requested_port, std::function<void()> const& before_commit = {}) {
        auto next = std::make_unique<Host>(); route(*next);
        int actual_port = requested_port;
        if (requested_port == 0) actual_port = next->server.bind_to_any_port(ip);
        else if (!next->server.bind_to_port(ip, requested_port)) throw std::runtime_error("端口绑定失败，请检查声桥是否已运行或端口被占用");
        if (actual_port <= 0) throw std::runtime_error("无法绑定本机服务");
        auto raw = next.get(); next->thread = std::thread([raw] { raw->server.listen_after_bind(); });
        next->server.wait_until_ready();
        try { if (before_commit) before_commit(); }
        catch (...) { stop_host(next); throw; }
        stop_host(host_); host_ = std::move(next); bound_ip = ip; port = actual_port;
    }
    void start() {
        auto list = addresses(); auto data = config_.all();
        displayed_ip = data.contains("lan_ip") && data["lan_ip"].is_string() && valid_ip(data["lan_ip"]) ? data["lan_ip"].get<std::string>() : choose_ip(list);
        bool available = displayed_ip == "127.0.0.1" || std::any_of(list.begin(), list.end(), [&](auto const& a) { return a.ip == displayed_ip; });
        auto ip = available ? displayed_ip : "127.0.0.1";
        try { bind(ip, port); } catch (...) { if (ip == "127.0.0.1") throw; bind("127.0.0.1", port); }
        if (!data.contains("lan_ip") && displayed_ip != "127.0.0.1") config_.update({{"lan_ip", displayed_ip}});
    }
    bool refresh() {
        auto ip = choose_ip(addresses());
        if (ip == "127.0.0.1") { status_("当前未找到可用局域网，地址保持不变"); return false; }
        if (ip == bound_ip && ip == displayed_ip) { status_("当前地址未变化：" + ip); return false; }
        bind(ip, port, [&] { config_.update({{"lan_ip", ip}}); }); displayed_ip = ip;
        { std::lock_guard lock(mutex_); session_ = random_token(8); }
        condition_.notify_all();
        status_("地址已刷新并保存：" + ip); return true;
    }
    std::string url() { std::lock_guard lock(mutex_); return "http://" + displayed_ip + ":" + std::to_string(port) + "/?token=" + token_; }
    void rotate() { auto token = config_.token(true); { std::lock_guard lock(mutex_); token_ = token; } condition_.notify_all(); }
    int64_t queue(std::string const& text) {
        std::lock_guard lock(mutex_); auto id = next_id_++; messages_.push_back({{"id", id}, {"text", text}});
        if (messages_.size() > 50) messages_.pop_front(); condition_.notify_all(); return id;
    }
};

inline std::wstring startup_command() {
    return L"\"" + (executable_dir() / L"VoiceBridge.exe").wstring() + L"\"";
}
inline bool autostart_enabled() {
    wchar_t value[32768]{}; DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"VoiceBridge", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS && value == startup_command();
}
inline void set_autostart(bool enabled) {
    HKEY key{}; winrt::check_win32(RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr));
    auto command = startup_command();
    auto result = enabled ? RegSetValueExW(key, L"VoiceBridge", 0, REG_SZ, reinterpret_cast<BYTE const*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * 2)) : RegDeleteValueW(key, L"VoiceBridge");
    RegCloseKey(key); if (result != ERROR_FILE_NOT_FOUND) winrt::check_win32(result);
}
}
