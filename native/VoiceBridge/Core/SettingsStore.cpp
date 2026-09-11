#include "SettingsStore.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace
{
    using namespace winrt::Windows::Data::Json;

    constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t RunValue[] = L"VoiceBridge";

    JsonObject ReadObject(std::filesystem::path const& path)
    {
        try
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return {};
            }
            std::string bytes(
                (std::istreambuf_iterator<char>(stream)),
                std::istreambuf_iterator<char>());
            return JsonObject::Parse(winrt::to_hstring(bytes));
        }
        catch (...)
        {
            return {};
        }
    }

    void WriteObject(std::filesystem::path const& path, JsonObject const& object)
    {
        auto parent = path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }

        auto temporary = path;
        temporary.replace_extension(L".tmp");
        auto bytes = winrt::to_string(object.Stringify());
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                throw std::system_error(
                    std::make_error_code(std::errc::io_error),
                    "Cannot write Voice Bridge configuration");
            }
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            stream.flush();
            if (!stream)
            {
                throw std::system_error(
                    std::make_error_code(std::errc::io_error),
                    "Cannot flush Voice Bridge configuration");
            }
        }

        if (!MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            auto error = GetLastError();
            DeleteFileW(temporary.c_str());
            throw std::system_error(
                static_cast<int>(error),
                std::system_category(),
                "Cannot replace Voice Bridge configuration");
        }
    }

    std::optional<std::wstring> StringValue(JsonObject const& object, wchar_t const* key)
    {
        auto value = object.TryLookup(key);
        if (value && value.ValueType() == JsonValueType::String)
        {
            return std::wstring(value.GetString());
        }
        return std::nullopt;
    }

    std::optional<bool> BooleanValue(JsonObject const& object, wchar_t const* key)
    {
        auto value = object.TryLookup(key);
        if (value && value.ValueType() == JsonValueType::Boolean)
        {
            return value.GetBoolean();
        }
        return std::nullopt;
    }

    std::optional<uint64_t> CountValue(JsonObject const& object, wchar_t const* key)
    {
        auto value = object.TryLookup(key);
        if (!value || value.ValueType() != JsonValueType::Number)
        {
            return std::nullopt;
        }
        auto number = value.GetNumber();
        if (!std::isfinite(number) || number < 0.0 || std::floor(number) != number ||
            number > static_cast<double>((std::numeric_limits<uint64_t>::max)()))
        {
            return std::nullopt;
        }
        return static_cast<uint64_t>(number);
    }

    void PutString(JsonObject const& object, wchar_t const* key, std::wstring const& value)
    {
        object.Insert(key, JsonValue::CreateStringValue(value));
    }

    void PutBoolean(JsonObject const& object, wchar_t const* key, bool value)
    {
        object.Insert(key, JsonValue::CreateBooleanValue(value));
    }

    void PutNumber(JsonObject const& object, wchar_t const* key, uint64_t value)
    {
        object.Insert(key, JsonValue::CreateNumberValue(static_cast<double>(value)));
    }

    std::wstring GenerateToken()
    {
        std::array<unsigned char, 32> bytes{};
        auto status = BCryptGenRandom(
            nullptr,
            bytes.data(),
            static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0)
        {
            throw std::runtime_error("Cannot generate the pairing token");
        }

        constexpr wchar_t alphabet[] =
            L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::wstring token;
        token.reserve(43);
        uint32_t accumulator{};
        int bits{};
        for (auto byte : bytes)
        {
            accumulator = (accumulator << 8) | byte;
            bits += 8;
            while (bits >= 6)
            {
                bits -= 6;
                token.push_back(alphabet[(accumulator >> bits) & 0x3f]);
            }
        }
        if (bits)
        {
            token.push_back(alphabet[(accumulator << (6 - bits)) & 0x3f]);
        }
        return token;
    }

    std::filesystem::path ExecutablePath()
    {
        std::wstring buffer(32768, L'\0');
        auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length || length == buffer.size())
        {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "Cannot locate Voice Bridge executable");
        }
        buffer.resize(length);
        return buffer;
    }
}

namespace VoiceBridge::Core
{
    std::mutex SettingsStore::s_configMutex;

    SettingsStore::SettingsStore(std::filesystem::path configPath) :
        m_configPath(std::move(configPath))
    {
    }

    std::filesystem::path SettingsStore::DefaultConfigPath()
    {
        std::wstring localAppData(32768, L'\0');
        auto length = GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData.data(),
            static_cast<DWORD>(localAppData.size()));
        if (length && length < localAppData.size())
        {
            localAppData.resize(length);
            return std::filesystem::path(localAppData) / L"VoiceBridge" / L"config.json";
        }

        PWSTR knownFolder{};
        winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &knownFolder));
        std::filesystem::path path(knownFolder);
        CoTaskMemFree(knownFolder);
        return path / L"VoiceBridge" / L"config.json";
    }

    std::wstring SettingsStore::StartupCommand(std::filesystem::path const& executable)
    {
        return L"\"" + executable.wstring() + L"\"";
    }

    Settings SettingsStore::Load() const
    {
        std::scoped_lock lock(s_configMutex);
        auto object = ReadObject(m_configPath);
        Settings settings;

        if (auto value = StringValue(object, L"close_action");
            value && (*value == L"ask" || *value == L"tray" || *value == L"exit"))
        {
            settings.closeAction = *value;
        }
        if (auto value = StringValue(object, L"send_mode");
            value && (*value == L"ctrl_enter" || *value == L"enter"))
        {
            settings.sendMode = *value;
        }
        if (auto value = BooleanValue(object, L"show_char_count"))
        {
            settings.showCharacterCount = *value;
        }
        if (auto value = StringValue(object, L"theme");
            value && (*value == L"light" || *value == L"dark"))
        {
            settings.theme = *value;
        }
        if (auto value = CountValue(object, L"character_count"))
        {
            settings.characterCount = *value;
        }
        if (auto value = BooleanValue(object, L"confirm_enter"))
        {
            settings.confirmEnter = *value;
        }
        if (auto value = BooleanValue(object, L"confirm_send"))
        {
            settings.confirmSend = *value;
        }
        if (auto value = BooleanValue(object, L"show_desktop_to_phone"))
        {
            settings.showDesktopToPhone = *value;
        }
        return settings;
    }

    void SettingsStore::Save(Settings const& settings)
    {
        std::scoped_lock lock(s_configMutex);
        auto object = ReadObject(m_configPath);
        PutString(object, L"close_action", settings.closeAction);
        PutString(object, L"send_mode", settings.sendMode);
        PutBoolean(object, L"show_char_count", settings.showCharacterCount);
        PutString(object, L"theme", settings.theme);
        PutNumber(object, L"character_count", settings.characterCount);
        PutBoolean(object, L"confirm_enter", settings.confirmEnter);
        PutBoolean(object, L"confirm_send", settings.confirmSend);
        PutBoolean(object, L"show_desktop_to_phone", settings.showDesktopToPhone);
        WriteObject(m_configPath, object);
    }

    uint64_t SettingsStore::AddCharacterCount(uint64_t amount)
    {
        std::scoped_lock lock(s_configMutex);
        auto object = ReadObject(m_configPath);
        auto count = CountValue(object, L"character_count").value_or(0);
        if (amount > (std::numeric_limits<uint64_t>::max)() - count)
        {
            throw std::overflow_error("Voice Bridge character count overflow");
        }
        count += amount;
        PutNumber(object, L"character_count", count);
        WriteObject(m_configPath, object);
        return count;
    }

    std::wstring SettingsStore::LoadOrCreateToken(bool rotate)
    {
        std::scoped_lock lock(s_configMutex);
        auto object = ReadObject(m_configPath);
        auto token = StringValue(object, L"token");
        if (!rotate && token && token->size() >= 32)
        {
            return *token;
        }
        auto generated = GenerateToken();
        PutString(object, L"token", generated);
        WriteObject(m_configPath, object);
        return generated;
    }

    std::optional<std::wstring> SettingsStore::LoadSavedLanIp() const
    {
        std::scoped_lock lock(s_configMutex);
        return StringValue(ReadObject(m_configPath), L"lan_ip");
    }

    void SettingsStore::SaveLanIp(std::wstring const& address)
    {
        std::scoped_lock lock(s_configMutex);
        auto object = ReadObject(m_configPath);
        PutString(object, L"lan_ip", address);
        WriteObject(m_configPath, object);
    }

    void SettingsStore::UpdateConfirmation(
        std::optional<bool> confirmEnter,
        std::optional<bool> confirmSend)
    {
        std::scoped_lock lock(s_configMutex);
        auto object = ReadObject(m_configPath);
        if (confirmEnter)
        {
            PutBoolean(object, L"confirm_enter", *confirmEnter);
        }
        if (confirmSend)
        {
            PutBoolean(object, L"confirm_send", *confirmSend);
        }
        WriteObject(m_configPath, object);
    }

    bool SettingsStore::AutostartEnabled() const
    {
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        {
            return false;
        }

        std::wstring value(32768, L'\0');
        DWORD type{};
        DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
        auto result = RegQueryValueExW(
            key,
            RunValue,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(value.data()),
            &size);
        RegCloseKey(key);
        if (result != ERROR_SUCCESS || type != REG_SZ || size < sizeof(wchar_t))
        {
            return false;
        }
        value.resize(size / sizeof(wchar_t) - 1);
        return value == StartupCommand(ExecutablePath());
    }

    void SettingsStore::SetAutostartEnabled(bool enabled) const
    {
        HKEY key{};
        winrt::check_hresult(HRESULT_FROM_WIN32(RegCreateKeyExW(
            HKEY_CURRENT_USER,
            RunKey,
            0,
            nullptr,
            0,
            KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr)));

        LSTATUS result{};
        if (enabled)
        {
            auto command = StartupCommand(ExecutablePath());
            result = RegSetValueExW(
                key,
                RunValue,
                0,
                REG_SZ,
                reinterpret_cast<BYTE const*>(command.c_str()),
                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            result = RegDeleteValueW(key, RunValue);
            if (result == ERROR_FILE_NOT_FOUND)
            {
                result = ERROR_SUCCESS;
            }
        }
        RegCloseKey(key);
        winrt::check_hresult(HRESULT_FROM_WIN32(result));
    }

    std::filesystem::path const& SettingsStore::ConfigPath() const noexcept
    {
        return m_configPath;
    }
}
