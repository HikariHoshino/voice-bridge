#include "../VoiceBridge/Core/SettingsStore.h"

#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    using VoiceBridge::Core::SettingsStore;

    void Require(bool condition, char const* reason)
    {
        if (!condition)
        {
            throw std::runtime_error(reason);
        }
    }

    std::string ReadBytes(std::filesystem::path const& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return {
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>() };
    }

    void WriteBytes(std::filesystem::path const& path, std::string const& bytes)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    void TestPythonSettingsCompatibility(std::filesystem::path const& config)
    {
        WriteBytes(config,
            R"({"close_action":"tray","send_mode":"enter","show_char_count":false,"theme":"dark","character_count":12,"confirm_enter":false,"confirm_send":true,"show_desktop_to_phone":false,"token":"preserved-token-with-at-least-32-characters","lan_ip":"192.168.5.7","future_key":"保留"})");

        SettingsStore store(config);
        auto settings = store.Load();
        Require(settings.closeAction == L"tray", "close_action must remain compatible");
        Require(settings.sendMode == L"enter", "send_mode must remain compatible");
        Require(!settings.showCharacterCount, "show_char_count must remain compatible");
        Require(settings.theme == L"dark", "theme must remain compatible");
        Require(settings.characterCount == 12, "character_count must remain compatible");
        Require(!settings.confirmEnter && settings.confirmSend,
            "mobile confirmations must remain compatible");
        Require(!settings.showDesktopToPhone,
            "desktop-to-phone visibility must remain compatible");

        settings.characterCount = 17;
        store.Save(settings);
        auto object = winrt::Windows::Data::Json::JsonObject::Parse(
            winrt::to_hstring(ReadBytes(config)));
        Require(object.GetNamedString(L"token") ==
            L"preserved-token-with-at-least-32-characters",
            "saving settings must not replace the pairing token");
        Require(object.GetNamedString(L"lan_ip") == L"192.168.5.7",
            "saving settings must not replace the saved LAN address");
        Require(object.GetNamedString(L"future_key") == L"保留",
            "saving settings must preserve unknown future keys");
    }

    void TestDefaultsRejectWrongJsonTypes(std::filesystem::path const& config)
    {
        WriteBytes(config,
            R"({"close_action":"later","send_mode":true,"show_char_count":1,"theme":"system","character_count":-3,"confirm_enter":"yes","confirm_send":0,"show_desktop_to_phone":null})");
        auto settings = SettingsStore(config).Load();
        Require(settings.closeAction == L"ask", "invalid close_action must use the old default");
        Require(settings.sendMode == L"ctrl_enter", "invalid send_mode must use the old default");
        Require(settings.showCharacterCount, "non-boolean show_char_count must use the default");
        Require(settings.theme == L"light", "invalid theme must use the old default");
        Require(settings.characterCount == 0, "negative character_count must use the default");
        Require(settings.confirmEnter && !settings.confirmSend,
            "invalid confirmation values must use old defaults");
        Require(settings.showDesktopToPhone,
            "invalid transfer visibility must use the old default");
    }

    void TestTokenAndCountPersistence(std::filesystem::path const& config)
    {
        WriteBytes(config, "{}");
        SettingsStore store(config);
        auto first = store.LoadOrCreateToken();
        auto reused = store.LoadOrCreateToken();
        auto rotated = store.LoadOrCreateToken(true);
        Require(first.size() == 43, "a 256-bit URL-safe token must contain 43 characters");
        Require(first == reused, "the existing token must be reused until rotation");
        Require(first != rotated, "rotation must invalidate the previous token");
        Require(rotated.find_first_not_of(
            L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::wstring::npos,
            "the token must remain URL-safe without padding");
        Require(store.AddCharacterCount(12) == 12, "character counts must start at zero");
        Require(store.AddCharacterCount(5) == 17, "character counts must accumulate persistently");
        Require(store.LoadOrCreateToken() == rotated,
            "character count updates must not replace the token");
    }

    void TestNativeAutostartCommand()
    {
        Require(
            SettingsStore::StartupCommand(L"C:\\Tools\\VoiceBridge.exe") ==
                L"\"C:\\Tools\\VoiceBridge.exe\"",
            "native autostart must contain only the quoted executable");
    }
}

void RunBridgeServerTests(std::filesystem::path const& directory);

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
    {
        std::wcerr << L"Usage: VoiceBridge.Tests.exe <project-test-directory>\n";
        return 2;
    }

    winrt::init_apartment();
    auto directory = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(directory);
    auto config = directory / L"settings-store.json";

    try
    {
        TestPythonSettingsCompatibility(config);
        TestDefaultsRejectWrongJsonTypes(config);
        TestTokenAndCountPersistence(config);
        TestNativeAutostartCommand();
        RunBridgeServerTests(directory);
        std::filesystem::remove(config);
        std::wcout << L"Native core tests: 10 passed\n";
        return 0;
    }
    catch (std::exception const& error)
    {
        std::filesystem::remove(config);
        std::cerr << "SettingsStoreTests failed: " << error.what() << '\n';
        return 1;
    }
}
