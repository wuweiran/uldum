#include "app/session_request.h"
#include "core/log.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace uldum::app {

namespace {
constexpr const char* TAG = "SessionRequest";
}

std::optional<SessionInfo> request_session(std::string_view server_url,
                                           std::string_view map_path) {
    httplib::Client cli(std::string{server_url});
    cli.set_connection_timeout(5, 0);

    nlohmann::json body;
    body["map"] = std::string{map_path};

    auto res = cli.Post("/sessions", body.dump(), "application/json");
    if (!res) {
        log::error(TAG, "Orchestrator request failed: {}", httplib::to_string(res.error()));
        return std::nullopt;
    }
    if (res->status != 201) {
        log::error(TAG, "Orchestrator rejected request (HTTP {}): {}", res->status, res->body);
        return std::nullopt;
    }

    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(res->body);
    } catch (const std::exception& e) {
        log::error(TAG, "Orchestrator response parse failed: {}", e.what());
        return std::nullopt;
    }

    SessionInfo info;
    info.addr       = resp.value("addr", "");
    info.port       = static_cast<u16>(resp.value("port", 0));
    info.session_id = resp.value("session_id", "");
    auto tokens_it  = resp.find("tokens");
    if (info.addr.empty() || info.port == 0 || tokens_it == resp.end() ||
        !tokens_it->is_array() || tokens_it->empty()) {
        log::error(TAG, "Orchestrator response missing required fields");
        return std::nullopt;
    }
    for (const auto& t : *tokens_it) info.tokens.push_back(t.get<std::string>());

    log::info(TAG, "Got session {} → worker {}:{} ({} token(s))",
              info.session_id, info.addr, info.port, info.tokens.size());
    return info;
}

} // namespace uldum::app
