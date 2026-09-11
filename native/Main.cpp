#include "NativeApp.h"
#include "SecondaryWindows.h"
#include "NativeTests.h"
#include "UiTests.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HANDLE mutex{};
    try {
        config_path = vb::default_config();
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        int count{}; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        vb::fs::path test_report;
        for (int i = 1; i < count; ++i) {
            std::wstring argument(args[i]);
            if (argument == L"--config" && i + 1 < count) { config_path = args[++i]; isolated = true; }
            else if (argument == L"--port" && i + 1 < count) override_port = std::stoi(args[++i]);
            else if (argument == L"--bind" && i + 1 < count) override_bind = vb::utf8(args[++i]);
            else if (argument == L"--self-test" && i + 1 < count) test_report = args[++i];
            else if (argument == L"--ui-self-test" && i + 1 < count) {
                ui_test_report = args[++i]; isolated = true; override_bind = "127.0.0.1";
                config_path = ui_test_report.parent_path() / ("ui-" + vb::random_token(8) + ".json");
            }
            else throw std::runtime_error("Unknown command line argument");
        }
        LocalFree(args); init_apartment(apartment_type::single_threaded);
        if (!test_report.empty()) return vb::run_tests(test_report);
        auto suffix = isolated ? L".NativeTest." + std::to_wstring(std::hash<std::wstring>{}(config_path.wstring())) : L"";
        mutex = CreateMutexW(nullptr, FALSE, (std::wstring(L"Local\\VoiceBridge.SingleInstance") + suffix).c_str());
        if (!mutex) throw_last_error(); bool existing = GetLastError() == ERROR_ALREADY_EXISTS;
        activation_event = CreateEventW(nullptr, TRUE, FALSE, (std::wstring(L"Local\\VoiceBridge.ActivateExisting") + suffix).c_str());
        activation_ack = CreateEventW(nullptr, TRUE, FALSE, (std::wstring(L"Local\\VoiceBridge.ActivateExisting.Acknowledged") + suffix).c_str());
        if (existing) { AllowSetForegroundWindow(ASFW_ANY); ResetEvent(activation_ack); SetEvent(activation_event); WaitForSingleObject(activation_ack, 3000); }
        else Application::Start([](auto&&) {
            try { make<App>(); }
            catch (hresult_error const& error) { log_error(error.message()); throw; }
            catch (std::exception const& error) { log_error(to_hstring(error.what())); throw; }
        });
        CloseHandle(activation_event); CloseHandle(activation_ack); CloseHandle(mutex); return ui_test_exit_code;
    } catch (hresult_error const& error) { MessageBoxW(nullptr, error.message().c_str(), L"声桥启动失败", MB_OK | MB_ICONERROR); }
      catch (std::exception const& error) { MessageBoxW(nullptr, vb::wide(error.what()).c_str(), L"声桥启动失败", MB_OK | MB_ICONERROR); }
    if (mutex) CloseHandle(mutex); return 1;
}
