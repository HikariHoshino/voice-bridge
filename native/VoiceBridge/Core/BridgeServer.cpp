#include "BridgeServer.h"

#include "Errors.h"

#include <windows.h>
#include <bcrypt.h>
#include <ws2tcpip.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{
    using namespace winrt::Windows::Data::Json;
    using VoiceBridge::Core::HttpRequest;
    using VoiceBridge::Core::HttpResponse;

    std::string LowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    std::string TrimAscii(std::string value)
    {
        auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos)
        {
            return {};
        }
        auto last = value.find_last_not_of(" \t");
        return value.substr(first, last - first + 1);
    }

    std::optional<uint64_t> UnsignedInteger(std::string const& value)
    {
        uint64_t number{};
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc{} || end != value.data() + value.size())
        {
            return std::nullopt;
        }
        return number;
    }

    std::optional<int64_t> SignedInteger(std::string const& value)
    {
        int64_t number{};
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc{} || end != value.data() + value.size())
        {
            return std::nullopt;
        }
        return number;
    }

    std::optional<uint64_t> ContentLength(HttpRequest const& request)
    {
        auto item = request.headers.find("content-length");
        if (item == request.headers.end())
        {
            return std::nullopt;
        }
        return UnsignedInteger(TrimAscii(item->second));
    }

    bool BodyLengthValid(HttpRequest const& request, uint64_t maximum)
    {
        auto length = ContentLength(request);
        return length && *length > 0 && *length <= maximum && *length == request.body.size();
    }

    int HexDigit(char value)
    {
        if (value >= '0' && value <= '9')
        {
            return value - '0';
        }
        value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
        return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
    }

    std::string UrlDecode(std::string const& value)
    {
        std::string decoded;
        decoded.reserve(value.size());
        for (size_t index = 0; index < value.size(); ++index)
        {
            if (value[index] == '+')
            {
                decoded.push_back(' ');
            }
            else if (value[index] == '%' && index + 2 < value.size())
            {
                auto high = HexDigit(value[index + 1]);
                auto low = HexDigit(value[index + 2]);
                if (high >= 0 && low >= 0)
                {
                    decoded.push_back(static_cast<char>((high << 4) | low));
                    index += 2;
                }
                else
                {
                    decoded.push_back(value[index]);
                }
            }
            else
            {
                decoded.push_back(value[index]);
            }
        }
        return decoded;
    }

    struct ParsedTarget
    {
        std::string path;
        std::unordered_map<std::string, std::string> query;
    };

    ParsedTarget ParseTarget(std::string const& target)
    {
        ParsedTarget parsed;
        auto question = target.find('?');
        parsed.path = target.substr(0, question);
        if (question == std::string::npos)
        {
            return parsed;
        }

        auto query = target.substr(question + 1);
        size_t start{};
        while (start <= query.size())
        {
            auto end = query.find('&', start);
            auto pair = query.substr(start, end == std::string::npos ? end : end - start);
            auto equals = pair.find('=');
            auto key = UrlDecode(pair.substr(0, equals));
            auto value = equals == std::string::npos ? std::string{} : UrlDecode(pair.substr(equals + 1));
            if (!key.empty() && !parsed.query.contains(key))
            {
                parsed.query.emplace(std::move(key), std::move(value));
            }
            if (end == std::string::npos)
            {
                break;
            }
            start = end + 1;
        }
        return parsed;
    }

    bool ConstantTimeEqual(std::string const& left, std::string const& right)
    {
        size_t difference = left.size() ^ right.size();
        auto size = (std::max)(left.size(), right.size());
        for (size_t index = 0; index < size; ++index)
        {
            auto a = index < left.size() ? static_cast<unsigned char>(left[index]) : 0;
            auto b = index < right.size() ? static_cast<unsigned char>(right[index]) : 0;
            difference |= a ^ b;
        }
        return difference == 0;
    }

    JsonObject JsonResult(bool ok)
    {
        JsonObject object;
        object.Insert(L"ok", JsonValue::CreateBooleanValue(ok));
        return object;
    }

    HttpResponse JsonResponse(int status, JsonObject const& object)
    {
        return { status, "application/json; charset=utf-8", winrt::to_string(object.Stringify()) };
    }

    HttpResponse ErrorResponse(int status, wchar_t const* message)
    {
        auto object = JsonResult(false);
        object.Insert(L"error", JsonValue::CreateStringValue(message));
        return JsonResponse(status, object);
    }

    HttpResponse ErrorResponse(int status, winrt::hstring const& message)
    {
        auto object = JsonResult(false);
        object.Insert(L"error", JsonValue::CreateStringValue(message));
        return JsonResponse(status, object);
    }

    std::string RandomUrlToken(size_t byteCount)
    {
        std::vector<unsigned char> bytes(byteCount);
        auto status = BCryptGenRandom(
            nullptr,
            bytes.data(),
            static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0)
        {
            throw std::runtime_error("Cannot generate bridge session id");
        }

        constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string result;
        uint32_t accumulator{};
        int bits{};
        for (auto byte : bytes)
        {
            accumulator = (accumulator << 8) | byte;
            bits += 8;
            while (bits >= 6)
            {
                bits -= 6;
                result.push_back(alphabet[(accumulator >> bits) & 0x3f]);
            }
        }
        if (bits)
        {
            result.push_back(alphabet[(accumulator << (6 - bits)) & 0x3f]);
        }
        return result;
    }

    size_t UnicodeCharacterCount(std::wstring const& text)
    {
        size_t count{};
        for (size_t index = 0; index < text.size(); ++index)
        {
            auto unit = static_cast<uint16_t>(text[index]);
            if (unit >= 0xd800 && unit <= 0xdbff && index + 1 < text.size())
            {
                auto next = static_cast<uint16_t>(text[index + 1]);
                if (next >= 0xdc00 && next <= 0xdfff)
                {
                    ++index;
                }
            }
            ++count;
        }
        return count;
    }

    std::string ReadFile(std::filesystem::path const& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            throw std::system_error(
                std::make_error_code(std::errc::no_such_file_or_directory),
                "Cannot open bundled Voice Bridge asset");
        }
        return {
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>() };
    }

    std::string Base64(std::string const& bytes)
    {
        constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve((bytes.size() + 2) / 3 * 4);
        size_t index{};
        while (index + 3 <= bytes.size())
        {
            auto value = (static_cast<uint32_t>(static_cast<unsigned char>(bytes[index])) << 16) |
                (static_cast<uint32_t>(static_cast<unsigned char>(bytes[index + 1])) << 8) |
                static_cast<unsigned char>(bytes[index + 2]);
            result.push_back(alphabet[(value >> 18) & 0x3f]);
            result.push_back(alphabet[(value >> 12) & 0x3f]);
            result.push_back(alphabet[(value >> 6) & 0x3f]);
            result.push_back(alphabet[value & 0x3f]);
            index += 3;
        }
        auto remaining = bytes.size() - index;
        if (remaining)
        {
            auto value = static_cast<uint32_t>(static_cast<unsigned char>(bytes[index])) << 16;
            if (remaining == 2)
            {
                value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[index + 1])) << 8;
            }
            result.push_back(alphabet[(value >> 18) & 0x3f]);
            result.push_back(alphabet[(value >> 12) & 0x3f]);
            result.push_back(remaining == 2 ? alphabet[(value >> 6) & 0x3f] : '=');
            result.push_back('=');
        }
        return result;
    }

    std::filesystem::path ModuleDirectory()
    {
        std::wstring path(32768, L'\0');
        auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length == path.size())
        {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "Cannot locate Voice Bridge executable assets");
        }
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    char const* ReasonPhrase(int status)
    {
        switch (status)
        {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 423: return "Locked";
        default: return "Internal Server Error";
        }
    }

    bool SendAll(SOCKET socket, std::string const& bytes)
    {
        size_t sent{};
        while (sent < bytes.size())
        {
            auto amount = send(
                socket,
                bytes.data() + sent,
                static_cast<int>((std::min)(bytes.size() - sent, static_cast<size_t>(INT_MAX))),
                0);
            if (amount == SOCKET_ERROR || amount == 0)
            {
                return false;
            }
            sent += amount;
        }
        return true;
    }
}

namespace VoiceBridge::Core
{
    BridgeServer::BridgeServer(
        std::wstring token,
        SettingsStore& settingsStore,
        InputCallback inputCallback,
        StatusCallback statusCallback,
        EnterCallback enterCallback,
        std::string page) :
        m_token(std::move(token)),
        m_settingsStore(settingsStore),
        m_inputCallback(std::move(inputCallback)),
        m_statusCallback(std::move(statusCallback)),
        m_enterCallback(std::move(enterCallback)),
        m_page(page.empty() ? LoadBundledPage() : std::move(page)),
        m_sessionId(RandomUrlToken(8))
    {
    }

    BridgeServer::~BridgeServer()
    {
        Stop();
    }

    void BridgeServer::Start(std::wstring const& address, uint16_t port)
    {
        if (m_running)
        {
            throw std::logic_error("Voice Bridge server is already running");
        }

        WSADATA winsock{};
        auto startup = WSAStartup(MAKEWORD(2, 2), &winsock);
        if (startup != 0)
        {
            throw std::system_error(startup, std::system_category(), "Cannot start Winsock");
        }
        m_winsockStarted = true;

        try
        {
            m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (m_listenSocket == INVALID_SOCKET)
            {
                throw std::system_error(WSAGetLastError(), std::system_category(), "Cannot create HTTP socket");
            }

            sockaddr_in endpoint{};
            endpoint.sin_family = AF_INET;
            endpoint.sin_port = htons(port);
            if (InetPtonW(AF_INET, address.c_str(), &endpoint.sin_addr) != 1)
            {
                throw std::invalid_argument("Bridge bind address is not IPv4");
            }
            if (bind(m_listenSocket, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR)
            {
                throw std::system_error(WSAGetLastError(), std::system_category(), "Cannot bind HTTP socket");
            }
            if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
            {
                throw std::system_error(WSAGetLastError(), std::system_category(), "Cannot listen on HTTP socket");
            }

            int endpointSize = sizeof(endpoint);
            if (getsockname(
                m_listenSocket,
                reinterpret_cast<sockaddr*>(&endpoint),
                &endpointSize) == SOCKET_ERROR)
            {
                throw std::system_error(WSAGetLastError(), std::system_category(), "Cannot read HTTP socket address");
            }
            m_boundPort = ntohs(endpoint.sin_port);
            m_running = true;
            m_acceptThread = std::thread(&BridgeServer::AcceptLoop, this);
        }
        catch (...)
        {
            Stop();
            throw;
        }
    }

    void BridgeServer::Stop() noexcept
    {
        m_running = false;
        m_phoneCondition.notify_all();
        if (m_listenSocket != INVALID_SOCKET)
        {
            shutdown(m_listenSocket, SD_BOTH);
            closesocket(m_listenSocket);
            m_listenSocket = INVALID_SOCKET;
        }
        if (m_acceptThread.joinable())
        {
            m_acceptThread.join();
        }

        std::vector<std::thread> workers;
        {
            std::scoped_lock lock(m_workersMutex);
            workers.swap(m_workers);
        }
        for (auto& worker : workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
        if (m_winsockStarted)
        {
            WSACleanup();
            m_winsockStarted = false;
        }
        m_boundPort = 0;
    }

    bool BridgeServer::Running() const noexcept
    {
        return m_running;
    }

    uint16_t BridgeServer::BoundPort() const noexcept
    {
        return m_boundPort;
    }

    void BridgeServer::AcceptingPhoneText(bool accepting) noexcept
    {
        m_acceptingPhoneText = accepting;
    }

    bool BridgeServer::AcceptingPhoneText() const noexcept
    {
        return m_acceptingPhoneText;
    }

    uint64_t BridgeServer::QueueForPhone(std::wstring const& text)
    {
        std::scoped_lock lock(m_phoneMutex);
        auto id = m_nextMessageId++;
        m_phoneMessages.push_back({ id, text });
        if (m_phoneMessages.size() > 50)
        {
            m_phoneMessages.erase(m_phoneMessages.begin(), m_phoneMessages.end() - 50);
        }
        m_phoneCondition.notify_all();
        return id;
    }

    size_t BridgeServer::AcknowledgePhoneMessages(
        uint64_t through,
        std::string const& clientSession)
    {
        std::scoped_lock lock(m_phoneMutex);
        if (clientSession != m_sessionId)
        {
            return 0;
        }
        auto previous = m_phoneMessages.size();
        std::erase_if(m_phoneMessages, [through](PhoneMessage const& message)
        {
            return message.id <= through;
        });
        return previous - m_phoneMessages.size();
    }

    std::vector<PhoneMessage> BridgeServer::MessagesForPhone(
        uint64_t after,
        std::string const& clientSession,
        std::chrono::milliseconds wait)
    {
        if (clientSession != m_sessionId)
        {
            after = 0;
        }
        std::unique_lock lock(m_phoneMutex);
        auto available = [&]
        {
            return !m_running || std::any_of(
                m_phoneMessages.begin(),
                m_phoneMessages.end(),
                [after](PhoneMessage const& message) { return message.id > after; });
        };
        if (!available() && wait.count() > 0)
        {
            m_phoneCondition.wait_for(lock, wait, available);
        }

        std::vector<PhoneMessage> messages;
        std::copy_if(
            m_phoneMessages.begin(),
            m_phoneMessages.end(),
            std::back_inserter(messages),
            [after](PhoneMessage const& message) { return message.id > after; });
        return messages;
    }

    std::string const& BridgeServer::SessionId() const noexcept
    {
        return m_sessionId;
    }

    HttpResponse BridgeServer::HandleRequest(HttpRequest const& request)
    {
        auto parsed = ParseTarget(request.target);
        auto token = parsed.query.find("token");
        if (token == parsed.query.end() ||
            !ConstantTimeEqual(token->second, winrt::to_string(m_token)))
        {
            return ErrorResponse(403, L"配对链接无效");
        }

        if (request.method == "GET")
        {
            if (parsed.path == "/settings")
            {
                auto settings = m_settingsStore.Load();
                auto object = JsonResult(true);
                object.Insert(L"confirm_enter", JsonValue::CreateBooleanValue(settings.confirmEnter));
                object.Insert(L"confirm_send", JsonValue::CreateBooleanValue(settings.confirmSend));
                return JsonResponse(200, object);
            }
            if (parsed.path == "/receive")
            {
                auto afterText = parsed.query.contains("after") ? parsed.query.at("after") : "0";
                auto afterNumber = SignedInteger(afterText);
                if (!afterNumber)
                {
                    return ErrorResponse(400, L"消息序号无效");
                }
                auto after = *afterNumber < 0 ? 0 : static_cast<uint64_t>(*afterNumber);
                auto session = parsed.query.contains("session") ? parsed.query.at("session") : std::string{};
                auto messages = MessagesForPhone(after, session);

                JsonArray array;
                for (auto const& message : messages)
                {
                    JsonObject item;
                    item.Insert(L"id", JsonValue::CreateNumberValue(static_cast<double>(message.id)));
                    item.Insert(L"text", JsonValue::CreateStringValue(message.text));
                    array.Append(item);
                }
                auto object = JsonResult(true);
                object.Insert(L"session", JsonValue::CreateStringValue(winrt::to_hstring(m_sessionId)));
                object.Insert(L"messages", array);
                return JsonResponse(200, object);
            }
            if (parsed.path != "/")
            {
                return ErrorResponse(404, L"页面不存在");
            }
            m_statusCallback(L"手机已连接");
            return { 200, "text/html; charset=utf-8", m_page };
        }

        if (request.method != "POST" ||
            (parsed.path != "/send" && parsed.path != "/enter" &&
                parsed.path != "/settings" && parsed.path != "/ack"))
        {
            return ErrorResponse(403, L"配对链接无效");
        }

        if (parsed.path == "/ack")
        {
            if (!BodyLengthValid(request, 1'024))
            {
                return ErrorResponse(400, L"确认信息无效");
            }
            try
            {
                auto payload = JsonObject::Parse(winrt::to_hstring(request.body));
                auto sessionValue = payload.TryLookup(L"session");
                auto throughValue = payload.TryLookup(L"through");
                if (!sessionValue || sessionValue.ValueType() != JsonValueType::String ||
                    !throughValue || throughValue.ValueType() != JsonValueType::Number)
                {
                    return ErrorResponse(400, L"确认信息无效");
                }
                auto throughNumber = throughValue.GetNumber();
                if (!std::isfinite(throughNumber) || throughNumber < 0 ||
                    std::floor(throughNumber) != throughNumber ||
                    throughNumber > static_cast<double>((std::numeric_limits<uint64_t>::max)()))
                {
                    return ErrorResponse(400, L"确认信息无效");
                }
                auto acknowledged = AcknowledgePhoneMessages(
                    static_cast<uint64_t>(throughNumber),
                    winrt::to_string(sessionValue.GetString()));
                if (acknowledged)
                {
                    m_statusCallback(
                        L"手机已确认接收 " + std::to_wstring(acknowledged) + L" 条消息");
                }
                auto object = JsonResult(true);
                object.Insert(L"session", JsonValue::CreateStringValue(winrt::to_hstring(m_sessionId)));
                object.Insert(L"acknowledged", JsonValue::CreateNumberValue(static_cast<double>(acknowledged)));
                return JsonResponse(200, object);
            }
            catch (...)
            {
                return ErrorResponse(400, L"确认信息无效");
            }
        }

        if (parsed.path == "/settings")
        {
            if (!BodyLengthValid(request, 1'024))
            {
                return ErrorResponse(400, L"设置内容无效");
            }
            try
            {
                auto payload = JsonObject::Parse(winrt::to_hstring(request.body));
                std::optional<bool> confirmEnter;
                std::optional<bool> confirmSend;
                if (payload.HasKey(L"confirm_enter"))
                {
                    auto value = payload.Lookup(L"confirm_enter");
                    if (value.ValueType() != JsonValueType::Boolean)
                    {
                        return ErrorResponse(400, L"设置内容无效");
                    }
                    confirmEnter = value.GetBoolean();
                }
                if (payload.HasKey(L"confirm_send"))
                {
                    auto value = payload.Lookup(L"confirm_send");
                    if (value.ValueType() != JsonValueType::Boolean)
                    {
                        return ErrorResponse(400, L"设置内容无效");
                    }
                    confirmSend = value.GetBoolean();
                }
                if (!confirmEnter && !confirmSend)
                {
                    return ErrorResponse(400, L"设置内容无效");
                }
                m_settingsStore.UpdateConfirmation(confirmEnter, confirmSend);
                auto settings = m_settingsStore.Load();
                auto object = JsonResult(true);
                object.Insert(L"confirm_enter", JsonValue::CreateBooleanValue(settings.confirmEnter));
                object.Insert(L"confirm_send", JsonValue::CreateBooleanValue(settings.confirmSend));
                return JsonResponse(200, object);
            }
            catch (...)
            {
                return ErrorResponse(400, L"设置内容无效");
            }
        }

        if (!m_acceptingPhoneText)
        {
            return ErrorResponse(423, L"电脑已暂停接收");
        }

        if (parsed.path == "/enter")
        {
            try
            {
                m_enterCallback();
                m_statusCallback(L"已按下回车");
                return JsonResponse(200, JsonResult(true));
            }
            catch (NoTextInputFocusError const& error)
            {
                auto message = winrt::to_hstring(error.what());
                m_statusCallback(message.c_str());
                return ErrorResponse(409, message);
            }
            catch (std::exception const& error)
            {
                m_statusCallback(L"发送回车失败：" + std::wstring(winrt::to_hstring(error.what())));
                return ErrorResponse(500, L"电脑回车失败");
            }
        }

        if (!BodyLengthValid(request, MaxTextBytes))
        {
            return ErrorResponse(400, L"内容为空或过长");
        }

        std::wstring text;
        try
        {
            auto payload = JsonObject::Parse(winrt::to_hstring(request.body));
            auto value = payload.TryLookup(L"text");
            if (!value || value.ValueType() != JsonValueType::String || value.GetString().empty())
            {
                return ErrorResponse(400, L"文字内容无效");
            }
            text = value.GetString();
        }
        catch (...)
        {
            return ErrorResponse(400, L"文字内容无效");
        }

        try
        {
            m_inputCallback(text);
        }
        catch (NoTextInputFocusError const& error)
        {
            auto message = winrt::to_hstring(error.what());
            m_statusCallback(message.c_str());
            return ErrorResponse(409, message);
        }
        catch (std::exception const& error)
        {
            m_statusCallback(L"发送失败：" + std::wstring(winrt::to_hstring(error.what())));
            return ErrorResponse(500, L"电脑输入失败");
        }

        m_statusCallback(L"已输入 " + std::to_wstring(UnicodeCharacterCount(text)) + L" 个字符");
        return JsonResponse(200, JsonResult(true));
    }

    std::string BridgeServer::LoadBundledPage()
    {
        auto assets = ModuleDirectory() / L"Assets";
        auto page = ReadFile(assets / L"mobile.html");
        auto icon = Base64(ReadFile(assets / L"voice-bridge.png"));
        constexpr std::string_view marker = "{{APP_ICON_BASE64}}";
        size_t position{};
        while ((position = page.find(marker, position)) != std::string::npos)
        {
            page.replace(position, marker.size(), icon);
            position += icon.size();
        }
        return page;
    }

    void BridgeServer::AcceptLoop()
    {
        while (m_running)
        {
            auto client = accept(m_listenSocket, nullptr, nullptr);
            if (client == INVALID_SOCKET)
            {
                if (!m_running)
                {
                    return;
                }
                continue;
            }
            std::scoped_lock lock(m_workersMutex);
            m_workers.emplace_back([this, client]
            {
                HandleClient(client);
                shutdown(client, SD_BOTH);
                closesocket(client);
            });
        }
    }

    void BridgeServer::HandleClient(SOCKET client)
    {
        DWORD timeout = 5'000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        std::string bytes;
        bytes.reserve(4'096);
        std::array<char, 4'096> buffer{};
        size_t headerEnd{};
        while ((headerEnd = bytes.find("\r\n\r\n")) == std::string::npos)
        {
            if (bytes.size() >= 16'384)
            {
                return;
            }
            auto received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received <= 0)
            {
                return;
            }
            bytes.append(buffer.data(), received);
        }

        HttpRequest request;
        auto headerBytes = bytes.substr(0, headerEnd);
        std::istringstream headers(headerBytes);
        std::string line;
        if (!std::getline(headers, line))
        {
            return;
        }
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        std::istringstream requestLine(line);
        std::string version;
        if (!(requestLine >> request.method >> request.target >> version))
        {
            return;
        }

        while (std::getline(headers, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            auto colon = line.find(':');
            if (colon != std::string::npos)
            {
                request.headers.emplace(
                    LowerAscii(TrimAscii(line.substr(0, colon))),
                    TrimAscii(line.substr(colon + 1)));
            }
        }

        auto bodyStart = headerEnd + 4;
        auto length = ContentLength(request).value_or(0);
        if (length <= MaxTextBytes)
        {
            request.body = bytes.substr(bodyStart);
            while (request.body.size() < length)
            {
                auto remaining = static_cast<int>((std::min)(
                    length - request.body.size(),
                    static_cast<uint64_t>(buffer.size())));
                auto received = recv(client, buffer.data(), remaining, 0);
                if (received <= 0)
                {
                    break;
                }
                request.body.append(buffer.data(), received);
            }
            if (request.body.size() > length)
            {
                request.body.resize(static_cast<size_t>(length));
            }
        }

        auto response = HandleRequest(request);
        std::ostringstream output;
        output << "HTTP/1.1 " << response.status << ' ' << ReasonPhrase(response.status) << "\r\n"
            << "Content-Type: " << response.contentType << "\r\n"
            << "Content-Length: " << response.body.size() << "\r\n"
            << "Cache-Control: no-store\r\n"
            << "Connection: close\r\n\r\n"
            << response.body;
        SendAll(client, output.str());
    }
}
