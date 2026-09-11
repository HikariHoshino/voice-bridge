#include "../VoiceBridge/Core/BridgeServer.h"
#include "../VoiceBridge/Core/Errors.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{
    using namespace VoiceBridge::Core;
    using namespace winrt::Windows::Data::Json;

    void Require(bool condition, char const* reason)
    {
        if (!condition)
        {
            throw std::runtime_error(reason);
        }
    }

    HttpRequest Request(
        std::string method,
        std::string target,
        std::string body = {})
    {
        HttpRequest request{ std::move(method), std::move(target), {}, std::move(body) };
        if (!request.body.empty())
        {
            request.headers["content-length"] = std::to_string(request.body.size());
        }
        return request;
    }

    JsonObject JsonBody(HttpResponse const& response)
    {
        return JsonObject::Parse(winrt::to_hstring(response.body));
    }

    BridgeServer CreateServer(
        SettingsStore& settings,
        BridgeServer::InputCallback input,
        BridgeServer::StatusCallback status = [](std::wstring const&) {},
        BridgeServer::EnterCallback enter = [] {})
    {
        return BridgeServer(
            L"test-token",
            settings,
            std::move(input),
            std::move(status),
            std::move(enter),
            "<!doctype html><title>声桥</title>");
    }

    void TestAuthorizationAndRouting(std::filesystem::path const& config)
    {
        SettingsStore settings(config);
        std::wstring status;
        auto server = CreateServer(
            settings,
            [](std::wstring const&) {},
            [&](std::wstring const& value) { status = value; });

        Require(server.HandleRequest(Request("GET", "/")).status == 403,
            "every phone route must require the pairing token");
        auto root = server.HandleRequest(Request("GET", "/?token=test-token"));
        Require(root.status == 200 && root.contentType == "text/html; charset=utf-8",
            "the paired root route must serve the mobile page");
        Require(status == L"手机已连接", "opening the mobile page must update desktop status");
        Require(server.HandleRequest(
            Request("GET", "/missing?token=test-token")).status == 404,
            "unknown paired GET routes must remain 404");
        Require(server.HandleRequest(
            Request("POST", "/missing?token=test-token")).status == 403,
            "unknown POST routes must not reveal routing details");
    }

    void TestSendEnterPauseAndLimits(std::filesystem::path const& config)
    {
        SettingsStore settings(config);
        std::wstring received;
        int enterCount{};
        auto server = CreateServer(
            settings,
            [&](std::wstring const& text) { received = text; },
            [](std::wstring const&) {},
            [&] { ++enterCount; });

        auto source = std::string(R"({"text":"甲  \r\n\n\t乙 🌟"})");
        auto sent = server.HandleRequest(Request(
            "POST", "/send?token=test-token", source));
        Require(sent.status == 200, "valid UTF-8 text must be accepted");
        Require(received == L"甲  \r\n\n\t乙 🌟",
            "line breaks, spaces, tabs and Unicode must reach the input callback unchanged");

        auto empty = Request("POST", "/send?token=test-token", "{}");
        Require(server.HandleRequest(empty).status == 400,
            "missing text must be rejected before input injection");
        auto oversized = Request("POST", "/send?token=test-token");
        oversized.headers["content-length"] = std::to_string(MaxTextBytes + 1);
        Require(server.HandleRequest(oversized).status == 400,
            "requests over 100 KB must be rejected without reading their body");

        Require(server.HandleRequest(
            Request("POST", "/enter?token=test-token")).status == 200,
            "paired remote Enter must be accepted");
        Require(enterCount == 1, "one remote Enter request must invoke one callback");
        server.AcceptingPhoneText(false);
        Require(server.HandleRequest(
            Request("POST", "/send?token=test-token", source)).status == 423,
            "paused receive mode must reject phone text");
        Require(server.HandleRequest(
            Request("POST", "/enter?token=test-token")).status == 423,
            "paused receive mode must reject remote Enter");
    }

    void TestMissingInputFocusIsReported(std::filesystem::path const& config)
    {
        SettingsStore settings(config);
        std::wstring status;
        auto server = CreateServer(
            settings,
            [](std::wstring const&)
            {
                throw NoTextInputFocusError("电脑端未检测到输入光标");
            },
            [&](std::wstring const& value) { status = value; });
        auto body = std::string("{\"text\":\"不应发送\"}");
        auto response = server.HandleRequest(Request(
            "POST", "/send?token=test-token", body));
        Require(response.status == 409, "missing editable focus must remain a conflict response");
        Require(status == L"电脑端未检测到输入光标",
            "the desktop must show the actionable focus error");
        Require(JsonBody(response).GetNamedString(L"error") == status,
            "the phone must receive the same actionable focus error");
    }

    void TestMobileSettingsAndAcknowledgement(std::filesystem::path const& config)
    {
        SettingsStore settings(config);
        settings.Save({});
        auto server = CreateServer(settings, [](std::wstring const&) {});

        auto update = std::string("{\"confirm_enter\":false,\"confirm_send\":true}");
        auto updated = server.HandleRequest(Request(
            "POST", "/settings?token=test-token", update));
        Require(updated.status == 200, "valid mobile confirmation settings must persist");
        auto loaded = settings.Load();
        Require(!loaded.confirmEnter && loaded.confirmSend,
            "the desktop config must store both mobile confirmation choices");
        auto current = JsonBody(server.HandleRequest(Request(
            "GET", "/settings?token=test-token")));
        Require(!current.GetNamedBoolean(L"confirm_enter") &&
            current.GetNamedBoolean(L"confirm_send"),
            "GET /settings must return the persisted choices");

        for (int index = 1; index <= 51; ++index)
        {
            server.QueueForPhone(L"消息 " + std::to_wstring(index));
        }
        auto reset = JsonBody(server.HandleRequest(Request(
            "GET", "/receive?token=test-token&after=999&session=stale")));
        auto messages = reset.GetNamedArray(L"messages");
        Require(messages.Size() == 50, "the in-memory phone queue must retain at most 50 messages");
        Require(messages.GetObjectAt(0).GetNamedNumber(L"id") == 2,
            "queue trimming must retain the newest 50 message ids");

        auto staleAck = std::string("{\"session\":\"stale\",\"through\":51}");
        auto stale = JsonBody(server.HandleRequest(Request(
            "POST", "/ack?token=test-token", staleAck)));
        Require(stale.GetNamedNumber(L"acknowledged") == 0,
            "a stale phone session must not remove current messages");

        auto ack = std::string("{\"session\":\"") + server.SessionId() +
            "\",\"through\":51}";
        auto accepted = JsonBody(server.HandleRequest(Request(
            "POST", "/ack?token=test-token", ack)));
        Require(accepted.GetNamedNumber(L"acknowledged") == 50,
            "the current phone session must acknowledge delivered messages");
        Require(server.MessagesForPhone(
            0, server.SessionId(), std::chrono::milliseconds(0)).empty(),
            "acknowledged messages must be removed from memory");
    }

    void TestSocketTransport(std::filesystem::path const& config)
    {
        SettingsStore settings(config);
        auto server = CreateServer(settings, [](std::wstring const&) {});
        server.Start(L"127.0.0.1", 0);
        Require(server.BoundPort() != 0, "binding port zero must expose the selected test port");

        auto client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Require(client != INVALID_SOCKET, "the loopback transport test must create a client socket");
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(server.BoundPort());
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(connect(client, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0,
            "the native HTTP listener must accept a loopback client");
        auto request = std::string(
            "GET /?token=test-token HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
        Require(send(client, request.data(), static_cast<int>(request.size()), 0) == request.size(),
            "the transport test must send one complete request");

        std::string response;
        std::array<char, 1'024> buffer{};
        for (;;)
        {
            auto received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received <= 0)
            {
                break;
            }
            response.append(buffer.data(), received);
        }
        closesocket(client);
        server.Stop();
        Require(response.starts_with("HTTP/1.1 200 OK\r\n"),
            "the listener must serialize the routed HTTP response");
        Require(response.ends_with("<!doctype html><title>声桥</title>"),
            "the listener must return the configured mobile page bytes");
    }

    void TestBundledMobilePage()
    {
        auto page = BridgeServer::LoadBundledPage();
        Require(page.find("{{APP_ICON_BASE64}}") == std::string::npos,
            "the served mobile page must not expose the icon placeholder");
        Require(page.find("data:image/png;base64,") != std::string::npos,
            "the served mobile page must embed the shared application icon");
        Require(page.find("id=\"send\"") != std::string::npos &&
            page.find("id=\"enter\"") != std::string::npos,
            "the production mobile page must retain its primary actions");
    }
}

void RunBridgeServerTests(std::filesystem::path const& directory)
{
    auto config = directory / L"bridge-server.json";
    TestAuthorizationAndRouting(config);
    TestSendEnterPauseAndLimits(config);
    TestMissingInputFocusIsReported(config);
    TestMobileSettingsAndAcknowledgement(config);
    TestSocketTransport(config);
    TestBundledMobilePage();
    std::filesystem::remove(config);
}
