#pragma once

#include "SettingsStore.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace VoiceBridge::Core
{
    struct NetworkBinding
    {
        std::wstring displayAddress;
        std::wstring bindAddress;
        bool available{};
    };

    class NetworkManager
    {
    public:
        static std::optional<std::wstring> ChooseLanIp(
            std::vector<std::wstring> const& addresses,
            std::set<std::wstring> const& gatewayAddresses = {});
        static std::set<std::wstring> ActiveIPv4Addresses();
        static std::wstring LocalIp();
        static NetworkBinding StartupBinding(SettingsStore& settingsStore);
        static NetworkBinding StartupBinding(
            SettingsStore& settingsStore,
            std::set<std::wstring> const& activeAddresses,
            std::wstring const& detectedAddress);

    private:
        static std::optional<std::wstring> NormalizeIPv4(std::wstring const& address);
    };
}
