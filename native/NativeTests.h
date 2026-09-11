#pragma once
#include "BridgeCore.h"
#include <future>
namespace vb {
inline int run_tests(fs::path const& report) {
    json results = json::array();
    auto check = [&](bool condition, char const* intent) { results.push_back({{"test", intent}, {"passed", condition}}); };
    try {
        fs::create_directories(report.parent_path());
        auto test_config = report.parent_path() / ("config-" + random_token(8) + ".json");
        Config config(test_config);
        check(config.settings() == defaults(), "missing configuration uses the original defaults");
        auto token = config.token();
        config.update({{"theme", "dark"}, {"custom_key", "preserved"}, {"character_count", -8}, {"confirm_send", "invalid"}});
        Config reloaded(test_config);
        check(reloaded.token() == token, "pairing survives restart");
        check(reloaded.settings()["theme"] == "dark" && reloaded.all()["custom_key"] == "preserved", "settings preserve unknown keys");
        check(reloaded.settings()["character_count"] == 0 && reloaded.settings()["confirm_send"] == false, "invalid typed settings cannot leak into behavior");
        std::string received; int enters = 0; bool focus = true;
        Bridge bridge(config, executable_dir(), [&](auto const& text) { if (!focus) throw NoInputFocus("no focus"); received = text; }, [&] { if (!focus) throw NoInputFocus("no focus"); ++enters; });
        bridge.displayed_ip = "127.0.0.1"; bridge.bind("127.0.0.1", 0);
        httplib::Client client("127.0.0.1", bridge.port); client.set_read_timeout(25);
        auto path = [&](std::string const& route) { return route + "?token=" + token; };
        auto post = [&](std::string const& route, json const& body) { return client.Post(path(route), body.dump(), "application/json"); };
        auto get = client.Get("/"); check(get && get->status == 403, "unpaired clients cannot load phone page");
        get = client.Get(path("/")); check(get && get->status == 200 && get->body.find("{{APP_ICON_BASE64}}") == std::string::npos && get->body.find("data:image/png;base64,") != std::string::npos, "phone page and inline icon are served without a cloud dependency");
        auto text = std::string("  # 标题\r\n\n- 项目  \t😀𠮷\n```cpp\n  x = 1;\n```\n");
        auto result = post("/send", {{"text", text}});
        check(result && result->status == 200 && received == text, "phone text preserves CRLF LF whitespace tabs Markdown and non-BMP Unicode");
        check(config.settings()["character_count"] == character_count(text), "character count counts Unicode scalars instead of UTF-8 bytes");
        bridge.accepting = false; received.clear();
        result = post("/send", {{"text", "blocked"}}); check(result && result->status == 423 && received.empty(), "pause prevents text injection");
        result = post("/enter", json::object()); check(result && result->status == 423 && enters == 0, "pause prevents remote Enter");
        bridge.accepting = true; focus = false;
        result = post("/send", {{"text", "blocked"}}); check(result && result->status == 409 && received.empty(), "missing input focus rejects text without counting or injecting");
        result = post("/enter", json::object()); check(result && result->status == 409 && enters == 0, "missing input focus rejects Enter");
        focus = true; result = post("/enter", json::object()); check(result && result->status == 200 && enters == 1, "confirmed Enter dispatches once");
        for (auto const& invalid : std::vector<json>{json::array(), nullptr, {{"text", 1}}, {{"text", ""}}}) {
            result = post("/send", invalid); check(result && result->status == 400, "malformed text is rejected without crashing the service");
        }
        result = client.Post(path("/send"), std::string(100001, 'x'), "application/json"); check(result && result->status == 400, "oversized input is bounded at 100KB");
        result = post("/settings", {{"confirm_send", true}, {"confirm_enter", false}}); check(result && result->status == 200 && config.settings()["confirm_send"] == true, "mobile confirmations persist on desktop");
        result = post("/settings", {{"confirm_send", 1}}); check(result && result->status == 400 && config.settings()["confirm_send"] == true, "invalid mobile setting does not overwrite the saved choice");
        bridge.queue(text);
        get = client.Get(path("/receive") + "&after=999&session=old"); auto body = json::parse(get->body); auto session = body["session"].get<std::string>();
        check(get->status == 200 && body["messages"].size() == 1 && body["messages"][0]["text"] == text, "restart session resets phone cursor and preserves queued text");
        result = post("/ack", {{"session", "old"}, {"through", 1}}); check(result && json::parse(result->body)["acknowledged"] == 0, "stale session cannot erase undelivered text");
        result = post("/ack", {{"session", session}, {"through", true}}); check(result && result->status == 400, "boolean is not an acknowledgement sequence");
        result = post("/ack", {{"session", session}, {"through", 1}}); check(result && json::parse(result->body)["acknowledged"] == 1, "phone acknowledgement removes only delivered messages");
        auto waiting = std::async(std::launch::async, [&] { httplib::Client poller("127.0.0.1", bridge.port); return poller.Get(path("/receive") + "&after=1&session=" + session); });
        bridge.queue("wake"); auto delivery = waiting.get(); check(delivery && json::parse(delivery->body)["messages"][0]["text"] == "wake", "long polling wakes when desktop sends new text");
        for (int i = 0; i < 60; ++i) bridge.queue(std::to_string(i));
        get = client.Get(path("/receive")); body = json::parse(get->body);
        check(body["messages"].size() == 50 && body["messages"][0]["text"] == "10", "offline queue retains only the newest 50 messages");
        get = client.Get(path("/receive") + "&after=abc"); check(get && get->status == 400, "invalid polling sequence fails loudly");
        bridge.rotate(); get = client.Get(path("/settings")); check(get && get->status == 403, "re-pair immediately revokes old links");
        token = config.token(); get = client.Get(path("/settings")); check(get && get->status == 200, "new pairing token authenticates");
        check(choose_ip({{"10.1.0.3", true}, {"192.168.2.1", false}}) == "10.1.0.3", "gateway adapter wins over unrelated private virtual networks");
        check(choose_ip({{"10.1.0.3", true}, {"192.168.2.2", true}}) == "192.168.2.2", "LAN preference matches original 192.168 then 172.16 then 10 ordering");
    } catch (std::exception const& error) { results.push_back({{"test", "test execution"}, {"passed", false}, {"error", error.what()}}); }
      catch (winrt::hresult_error const& error) { results.push_back({{"test", "Windows execution"}, {"passed", false}, {"error", winrt::to_string(error.message())}}); }
    auto failures = std::count_if(results.begin(), results.end(), [](auto const& r) { return r["passed"] == false; });
    std::ofstream output(report, std::ios::binary); output << json{{"passed", results.size() - failures}, {"failed", failures}, {"tests", results}}.dump(2);
    return failures ? 1 : 0;
}
}
