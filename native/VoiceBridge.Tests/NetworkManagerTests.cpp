#include "../VoiceBridge/Core/NetworkManager.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace
{
    using namespace VoiceBridge::Core;

    void Require(bool condition, char const* reason)
    {
        if (!condition)
        {
            throw std::runtime_error(reason);
        }
    }

    void ResetConfig(std::filesystem::path const& config)
    {
        std::ofstream(config, std::ios::binary | std::ios::trunc) << "{}";
    }

    void TestPrivateRangePreference()
    {
        auto selected = NetworkManager::ChooseLanIp({
            L"10.0.0.8", L"172.20.4.5", L"192.168.9.7" });
        Require(selected == L"192.168.9.7",
            "the native selector must keep the Python 192.168, 172.16, 10 preference order");
    }

    void TestGatewayAdapterWins()
    {
        auto selected = NetworkManager::ChooseLanIp(
            { L"192.168.56.1", L"192.168.5.7" },
            { L"192.168.5.7" });
        Require(selected == L"192.168.5.7",
            "a default-gateway adapter must win over a host-only virtual adapter");
    }

    void TestFirstAddressPersists(std::filesystem::path const& config)
    {
        ResetConfig(config);
        SettingsStore settings(config);
        auto first = NetworkManager::StartupBinding(
            settings,
            { L"127.0.0.1", L"192.168.5.7" },
            L"192.168.5.7");
        Require(first.available && first.bindAddress == L"192.168.5.7",
            "the first active LAN address must be used for display and binding");
        Require(settings.LoadSavedLanIp() == L"192.168.5.7",
            "the first active LAN address must be persisted");

        auto restarted = NetworkManager::StartupBinding(
            settings,
            { L"127.0.0.1", L"192.168.5.7", L"192.168.5.8" },
            L"192.168.5.8");
        Require(restarted.displayAddress == L"192.168.5.7" &&
            restarted.bindAddress == L"192.168.5.7",
            "restart must reuse the saved active address instead of silently switching");
    }

    void TestStaleAddressIsNotReplaced(std::filesystem::path const& config)
    {
        ResetConfig(config);
        SettingsStore settings(config);
        settings.SaveLanIp(L"192.168.10.104");
        auto binding = NetworkManager::StartupBinding(
            settings,
            { L"127.0.0.1", L"192.168.10.105" },
            L"192.168.10.105");
        Require(!binding.available && binding.displayAddress == L"192.168.10.104" &&
            binding.bindAddress == L"127.0.0.1",
            "a stale saved address must remain visible while the server binds only to loopback");
        Require(settings.LoadSavedLanIp() == L"192.168.10.104",
            "temporary network loss must not replace the saved address");
    }
}

void RunNetworkManagerTests(std::filesystem::path const& directory)
{
    auto config = directory / L"network-manager.json";
    TestPrivateRangePreference();
    TestGatewayAdapterWins();
    TestFirstAddressPersists(config);
    TestStaleAddressIsNotReplaced(config);
    std::filesystem::remove(config);
}
