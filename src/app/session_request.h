#pragma once

#include "core/types.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Orchestrator session-request helper. Shared by the CLI `--server` path and
// the dev console's "Host via Server" button. The declaration is httplib-free
// on purpose: SessionInfo is pure data so the `uldum` shared lib (which builds
// the console + DevApp) can include this header and hold a callback of this
// shape WITHOUT linking cpp-httplib. The implementation (session_request.cpp)
// pulls in httplib and is compiled only into the `uldum_dev` executable, which
// already links it.
namespace uldum::app {

// What the orchestrator returns for a freshly spawned worker session.
struct SessionInfo {
    std::string              addr;        // worker UDP address (e.g. "127.0.0.1")
    u16                      port = 0;    // worker UDP port (orchestrator-assigned, 9000-9999)
    std::vector<std::string> tokens;      // one bearer token per player slot; tokens[0] = caller's
    std::string              session_id;  // orchestrator session id (for logs)
};

// POST /sessions to `server_url` (e.g. "http://127.0.0.1:8080") for `map_path`.
// Returns the session on HTTP 201 with a well-formed body; std::nullopt on any
// transport / status / parse error (details logged). Blocking; 5s connect timeout.
std::optional<SessionInfo> request_session(std::string_view server_url,
                                           std::string_view map_path);

} // namespace uldum::app
