#include "NetworkManager.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <system_error>
#include <unordered_set>

namespace
{
    class WinsockScope
    {
    public:
        WinsockScope()
        {
            WSADATA data{};
            auto result = WSAStartup(MAKEWORD(2, 2), &data);
            if (result != 0)
            {
                throw std::system_error(result, std::system_category(), "Cannot start Winsock");
            }
        }

        ~WinsockScope()
        {
            WSACleanup();
        }
    };

    struct AdapterAddresses
    {
        std::vector<std::wstring> all;
        std::set<std::wstring> routed;
    };

    std::optional<std::wstring> AddressText(SOCKADDR const* address)
    {
        if (!address || address->sa_family != AF_INET)
        {
            return std::nullopt;
        }
        wchar_t text[INET_ADDRSTRLEN]{};
        auto ipv4 = reinterpret_cast<sockaddr_in const*>(address)->sin_addr;
        if (!InetNtopW(AF_INET, &ipv4, text, static_cast<DWORD>(std::size(text))))
        {
            return std::nullopt;
        }
        return text;
    }

    AdapterAddresses EnumerateAdapters()
    {
        ULONG size = 16 * 1024;
        std::vector<unsigned char> buffer(size);
        auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        auto flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
            GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        auto result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
        if (result == ERROR_BUFFER_OVERFLOW)
        {
            buffer.resize(size);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
        }
        if (result != NO_ERROR)
        {
            return {};
        }

        AdapterAddresses found;
        for (auto adapter = adapters; adapter; adapter = adapter->Next)
        {
            if (adapter->OperStatus != IfOperStatusUp)
            {
                continue;
            }
            bool hasDefaultGateway{};
            for (auto gateway = adapter->FirstGatewayAddress; gateway; gateway = gateway->Next)
            {
                auto text = AddressText(gateway->Address.lpSockaddr);
                if (text && *text != L"0.0.0.0")
                {
                    hasDefaultGateway = true;
                    break;
                }
            }
            for (auto address = adapter->FirstUnicastAddress; address; address = address->Next)
            {
                auto text = AddressText(address->Address.lpSockaddr);
                if (!text || *text == L"0.0.0.0")
                {
                    continue;
                }
                found.all.push_back(*text);
                if (hasDefaultGateway)
                {
                    found.routed.insert(*text);
                }
            }
        }
        return found;
    }

    bool InPrivateRange(std::wstring const& address, int range)
    {
        IN_ADDR value{};
        if (InetPtonW(AF_INET, address.c_str(), &value) != 1)
        {
            return false;
        }
        auto host = ntohl(value.S_un.S_addr);
        switch (range)
        {
        case 0: return (host & 0xffff0000u) == 0xc0a80000u;
        case 1: return (host & 0xfff00000u) == 0xac100000u;
        default: return (host & 0xff000000u) == 0x0a000000u;
        }
    }
}

namespace VoiceBridge::Core
{
    std::optional<std::wstring> NetworkManager::ChooseLanIp(
        std::vector<std::wstring> const& addresses,
        std::set<std::wstring> const& gatewayAddresses)
    {
        std::vector<std::wstring> unique;
        std::unordered_set<std::wstring> seen;
        for (auto const& address : addresses)
        {
            if (seen.insert(address).second)
            {
                unique.push_back(address);
            }
        }

        if (!gatewayAddresses.empty())
        {
            std::vector<std::wstring> routed;
            std::copy_if(unique.begin(), unique.end(), std::back_inserter(routed),
                [&](std::wstring const& address)
                {
                    return gatewayAddresses.contains(address);
                });
            if (!routed.empty())
            {
                unique = std::move(routed);
            }
        }

        for (int range = 0; range < 3; ++range)
        {
            auto found = std::find_if(unique.begin(), unique.end(),
                [range](std::wstring const& address)
                {
                    return InPrivateRange(address, range);
                });
            if (found != unique.end())
            {
                return *found;
            }
        }
        return std::nullopt;
    }

    std::set<std::wstring> NetworkManager::ActiveIPv4Addresses()
    {
        WinsockScope winsock;
        auto addresses = EnumerateAdapters();
        std::set<std::wstring> active(addresses.all.begin(), addresses.all.end());
        active.insert(addresses.routed.begin(), addresses.routed.end());
        active.insert(L"127.0.0.1");
        return active;
    }

    std::wstring NetworkManager::LocalIp()
    {
        WinsockScope winsock;
        auto addresses = EnumerateAdapters();
        if (auto preferred = ChooseLanIp(addresses.all, addresses.routed))
        {
            return *preferred;
        }

        auto socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socketHandle == INVALID_SOCKET)
        {
            return L"127.0.0.1";
        }
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(1);
        InetPtonW(AF_INET, L"10.255.255.255", &remote.sin_addr);
        if (connect(socketHandle, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == SOCKET_ERROR)
        {
            closesocket(socketHandle);
            return L"127.0.0.1";
        }
        sockaddr_in local{};
        int size = sizeof(local);
        if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&local), &size) == SOCKET_ERROR)
        {
            closesocket(socketHandle);
            return L"127.0.0.1";
        }
        closesocket(socketHandle);
        wchar_t text[INET_ADDRSTRLEN]{};
        if (!InetNtopW(AF_INET, &local.sin_addr, text, static_cast<DWORD>(std::size(text))))
        {
            return L"127.0.0.1";
        }
        return text;
    }

    NetworkBinding NetworkManager::StartupBinding(SettingsStore& settingsStore)
    {
        return StartupBinding(settingsStore, ActiveIPv4Addresses(), LocalIp());
    }

    NetworkBinding NetworkManager::StartupBinding(
        SettingsStore& settingsStore,
        std::set<std::wstring> const& activeAddresses,
        std::wstring const& detectedAddress)
    {
        auto saved = settingsStore.LoadSavedLanIp();
        if (saved)
        {
            if (auto normalized = NormalizeIPv4(*saved))
            {
                auto available = activeAddresses.contains(*normalized);
                return { *normalized, available ? *normalized : L"127.0.0.1", available };
            }
        }

        auto selected = NormalizeIPv4(detectedAddress).value_or(L"127.0.0.1");
        auto available = selected != L"127.0.0.1";
        if (available)
        {
            settingsStore.SaveLanIp(selected);
        }
        return { selected, selected, available };
    }

    std::optional<std::wstring> NetworkManager::NormalizeIPv4(std::wstring const& address)
    {
        IN_ADDR value{};
        if (InetPtonW(AF_INET, address.c_str(), &value) != 1)
        {
            return std::nullopt;
        }
        auto host = ntohl(value.S_un.S_addr);
        auto first = host >> 24;
        if (host == 0 || first >= 224 && first <= 239)
        {
            return std::nullopt;
        }
        wchar_t text[INET_ADDRSTRLEN]{};
        if (!InetNtopW(AF_INET, &value, text, static_cast<DWORD>(std::size(text))))
        {
            return std::nullopt;
        }
        return text;
    }
}
