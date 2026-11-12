#pragma once
#include <atomic>
#include <chrono>
#include <exception>

namespace vb {
// Buffer one operation in memory; write only after input/rejection is complete.
class FocusDiagnostics {
    inline static std::atomic<unsigned long long> sequence_{0};
    inline static thread_local FocusDiagnostics* current_ = nullptr;
    FocusDiagnostics* previous_ = current_;
    ULONGLONG started_ = GetTickCount64();
    int exceptions_ = std::uncaught_exceptions();
    json data_;
public:
    static fs::path log_path() {
        static auto path = default_config().parent_path() / L"diagnostics" /
            (L"focus-" + std::to_wstring(std::chrono::system_clock::now().time_since_epoch().count()) +
             L"-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
        return path;
    }
    static bool write(json const& data, fs::path const& path = log_path()) noexcept {
        DWORD saved = GetLastError(); bool success = false;
        try {
            static std::mutex mutex; std::lock_guard lock(mutex);
            fs::create_directories(path.parent_path());
            std::ofstream stream(path, std::ios::binary | std::ios::app);
            stream << data.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
            stream.flush(); success = static_cast<bool>(stream);
        } catch (...) {}
        if (!success) OutputDebugStringW(L"VoiceBridge: diagnostic log write failed\n");
        SetLastError(saved); return success;
    }
    explicit FocusDiagnostics(char const* action) : data_({
        {"schema", 1}, {"build", "SelfInput-20261102"},
        {"operation_id", ++sequence_}, {"pid", GetCurrentProcessId()}, {"tid", GetCurrentThreadId()},
        {"started_unix_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
        {"action", action}, {"phase", "initial_focus"}, {"events", json::array()}}) {
        if (previous_) data_["parent_operation_id"] = previous_->data_["operation_id"];
        current_ = this;
    }
    static void event(json data) noexcept {
        DWORD saved = GetLastError();
        try { if (current_) { data["elapsed_ms"] = GetTickCount64() - current_->started_; data["phase"] = current_->data_["phase"]; current_->data_["events"].push_back(std::move(data)); } } catch (...) {}
        SetLastError(saved);
    }
    static void phase(char const* value) noexcept {
        try { if (current_) current_->data_["phase"] = value; } catch (...) {}
    }
    ~FocusDiagnostics() noexcept {
        DWORD saved = GetLastError();
        try {
            data_["duration_ms"] = GetTickCount64() - started_;
            data_["outcome"] = std::uncaught_exceptions() > exceptions_ ? "exception" : "completed";
            // Process names are collected after the operation, never window titles or text.
            json processes = json::object();
            for (auto const& event : data_["events"]) {
                if (!event.contains("foreground_pid")) continue;
                DWORD pid = event["foreground_pid"].get<DWORD>();
                auto key = std::to_string(pid); if (processes.contains(key)) continue;
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                json info = {{"pid", pid}};
                if (process) {
                    wchar_t path[32768]{}; DWORD length = 32768;
                    if (QueryFullProcessImageNameW(process, 0, path, &length)) info["exe"] = utf8(fs::path(path).filename().wstring());
                    else info["image_error"] = GetLastError();
                    HANDLE token{};
                    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
                        TOKEN_ELEVATION elevation{}; DWORD size{};
                        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) info["elevated"] = elevation.TokenIsElevated != 0;
                        else info["elevation_error"] = GetLastError();
                        CloseHandle(token);
                    } else info["token_error"] = GetLastError();
                    CloseHandle(process);
                } else info["open_error"] = GetLastError();
                processes[key] = std::move(info);
            }
            data_["applications"] = std::move(processes);
            write(data_);
        } catch (...) { OutputDebugStringW(L"VoiceBridge: diagnostic serialization failed\n"); }
        current_ = previous_; SetLastError(saved);
    }
};
struct FocusProbe {
    ULONGLONG started = GetTickCount64();
    json data = {{"event", "focus_probe"}, {"stage", "foreground"}, {"decision", "unknown"}};
    void checked(char const* stage, HRESULT result) {
        data["stage"] = stage; data["hresults"][stage] = static_cast<long>(result); winrt::check_hresult(result);
    }
    ~FocusProbe() noexcept {
        try { data["query_ms"] = GetTickCount64() - started; FocusDiagnostics::event(std::move(data)); } catch (...) {}
    }
};
}
