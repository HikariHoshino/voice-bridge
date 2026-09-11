#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace VoiceBridge::Core
{
    struct Settings
    {
        std::wstring closeAction{ L"ask" };
        std::wstring sendMode{ L"ctrl_enter" };
        bool showCharacterCount{ true };
        std::wstring theme{ L"light" };
        uint64_t characterCount{};
        bool confirmEnter{ true };
        bool confirmSend{};
        bool showDesktopToPhone{ true };
    };

    class SettingsStore
    {
    public:
        explicit SettingsStore(std::filesystem::path configPath = DefaultConfigPath());

        static std::filesystem::path DefaultConfigPath();
        static std::wstring StartupCommand(std::filesystem::path const& executable);

        Settings Load() const;
        void Save(Settings const& settings);
        uint64_t AddCharacterCount(uint64_t amount);
        std::wstring LoadOrCreateToken(bool rotate = false);
        std::optional<std::wstring> LoadSavedLanIp() const;
        void SaveLanIp(std::wstring const& address);
        void UpdateConfirmation(
            std::optional<bool> confirmEnter,
            std::optional<bool> confirmSend);

        bool AutostartEnabled() const;
        void SetAutostartEnabled(bool enabled) const;

        std::filesystem::path const& ConfigPath() const noexcept;

    private:
        std::filesystem::path m_configPath;
        static std::mutex s_configMutex;
    };
}
