#pragma once

#include "SettingsStore.h"

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace VoiceBridge::Core
{
    inline constexpr uint64_t MaxTextBytes = 100'000;

    struct HttpRequest
    {
        std::string method;
        std::string target;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };

    struct HttpResponse
    {
        int status{};
        std::string contentType{ "application/json; charset=utf-8" };
        std::string body;
    };

    struct PhoneMessage
    {
        uint64_t id{};
        std::wstring text;
    };

    class BridgeServer
    {
    public:
        using InputCallback = std::function<void(std::wstring const&)>;
        using StatusCallback = std::function<void(std::wstring const&)>;
        using EnterCallback = std::function<void()>;

        BridgeServer(
            std::wstring token,
            SettingsStore& settingsStore,
            InputCallback inputCallback,
            StatusCallback statusCallback,
            EnterCallback enterCallback,
            std::string page = {});
        ~BridgeServer();

        BridgeServer(BridgeServer const&) = delete;
        BridgeServer& operator=(BridgeServer const&) = delete;

        void Start(std::wstring const& address, uint16_t port = 8765);
        void Stop() noexcept;
        bool Running() const noexcept;
        uint16_t BoundPort() const noexcept;

        void AcceptingPhoneText(bool accepting) noexcept;
        bool AcceptingPhoneText() const noexcept;

        uint64_t QueueForPhone(std::wstring const& text);
        size_t AcknowledgePhoneMessages(uint64_t through, std::string const& clientSession);
        std::vector<PhoneMessage> MessagesForPhone(
            uint64_t after,
            std::string const& clientSession,
            std::chrono::milliseconds wait = std::chrono::seconds(20));

        std::string const& SessionId() const noexcept;
        HttpResponse HandleRequest(HttpRequest const& request);

        static std::string LoadBundledPage();

    private:
        std::wstring m_token;
        SettingsStore& m_settingsStore;
        InputCallback m_inputCallback;
        StatusCallback m_statusCallback;
        EnterCallback m_enterCallback;
        std::string m_page;
        std::string m_sessionId;
        std::atomic_bool m_acceptingPhoneText{ true };
        std::atomic_bool m_running{};
        SOCKET m_listenSocket{ INVALID_SOCKET };
        uint16_t m_boundPort{};
        std::thread m_acceptThread;
        std::mutex m_workersMutex;
        std::vector<std::thread> m_workers;
        std::mutex m_phoneMutex;
        std::condition_variable m_phoneCondition;
        std::vector<PhoneMessage> m_phoneMessages;
        uint64_t m_nextMessageId{ 1 };
        bool m_winsockStarted{};

        void AcceptLoop();
        void HandleClient(SOCKET client);
    };
}
