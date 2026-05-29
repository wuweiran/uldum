#include "app/engine.h"
#include "core/log.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr const char* TAG = "Dev";

struct DevArgs {
    std::string map_path;           // --map
    std::string locale;             // --locale
    std::string server_url;         // --server  (opt into orchestrator flow)
    std::string connect_address;    // --connect <host:port>
    std::string token_hex;          // --token <hex>
    bool host_mode = false;         // --host
};

void print_usage() {
    std::fprintf(stderr,
        "Usage: uldum_dev [options]\n"
        "  --map <path>             Map to load (default: maps/test_map.uldmap, single-player)\n"
        "  --locale <code>          BCP 47 locale, e.g. 'en' or 'zh-CN'\n"
        "  --host                   Host a LAN session on port 7777 (direct, no orchestrator)\n"
        "  --connect <host:port>    Direct UDP join to a LAN host or worker\n"
        "  --token <hex>            Bearer token to present in C_JOIN (use with --connect)\n"
        "  --server <url>           Talk to an uldum_server orchestrator (e.g. http://127.0.0.1:8080)\n");
}

DevArgs parse_args(int argc, char* argv[]) {
    DevArgs out;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            out.map_path = argv[++i];
        } else if (std::strcmp(argv[i], "--locale") == 0 && i + 1 < argc) {
            out.locale = argv[++i];
        } else if (std::strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            out.server_url = argv[++i];
        } else if (std::strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            out.connect_address = argv[++i];
        } else if (std::strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
            out.token_hex = argv[++i];
        } else if (std::strcmp(argv[i], "--host") == 0) {
            out.host_mode = true;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            std::exit(0);
        }
    }
    return out;
}

// Split a "host:port" string. Returns false on malformed input.
bool split_host_port(const std::string& s, std::string& host, uldum::u16& port) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    host = s.substr(0, colon);
    try {
        port = static_cast<uldum::u16>(std::stoi(s.substr(colon + 1)));
    } catch (...) { return false; }
    return true;
}

// Ask the orchestrator for a session. On success, fills `out_args` with
// connection target + the local player's token. Tokens for other slots
// are printed to stderr so the dev can paste them to friends.
bool request_session_from_orchestrator(const DevArgs& dev,
                                       uldum::LaunchArgs& out_args) {
    httplib::Client cli(dev.server_url);
    cli.set_connection_timeout(5, 0);

    nlohmann::json body;
    body["map"] = dev.map_path;

    auto res = cli.Post("/sessions", body.dump(), "application/json");
    if (!res) {
        uldum::log::error(TAG, "Orchestrator request failed: {}",
                          httplib::to_string(res.error()));
        return false;
    }
    if (res->status != 201) {
        uldum::log::error(TAG, "Orchestrator rejected request (HTTP {}): {}",
                          res->status, res->body);
        return false;
    }

    nlohmann::json resp;
    try {
        resp = nlohmann::json::parse(res->body);
    } catch (const std::exception& e) {
        uldum::log::error(TAG, "Orchestrator response parse failed: {}", e.what());
        return false;
    }

    std::string addr = resp.value("addr", "");
    uldum::u16 port  = static_cast<uldum::u16>(resp.value("port", 0));
    auto tokens_it   = resp.find("tokens");
    if (addr.empty() || port == 0 || tokens_it == resp.end() ||
        !tokens_it->is_array() || tokens_it->empty()) {
        uldum::log::error(TAG, "Orchestrator response missing required fields");
        return false;
    }
    std::string my_token = (*tokens_it)[0].get<std::string>();

    out_args.net_mode        = uldum::network::Mode::Client;
    out_args.connect_address = addr;
    out_args.port            = port;
    out_args.auth_token.assign(my_token.begin(), my_token.end());

    uldum::log::info(TAG, "Got session {} → connecting to {}:{}",
                     resp.value("session_id", ""), addr, port);
    if (tokens_it->size() > 1) {
        std::fprintf(stderr, "\nAdditional slot tokens (share with other players):\n");
        for (size_t i = 1; i < tokens_it->size(); ++i) {
            std::fprintf(stderr, "  slot %zu: %s\n", i, (*tokens_it)[i].get<std::string>().c_str());
        }
        std::fprintf(stderr, "\nEach other player runs:\n"
                             "  uldum_dev --connect %s:%u --token <their-token>\n\n",
                     addr.c_str(), port);
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    auto dev = parse_args(argc, argv);
    uldum::LaunchArgs args;
    args.locale = dev.locale;

    if (!dev.server_url.empty()) {
        // Orchestrator flow: dev acts as game-backend-stand-in, then becomes
        // the first player. The map path is required so the orchestrator
        // knows what to spawn.
        if (dev.map_path.empty()) {
            std::fprintf(stderr, "--server requires --map <path>\n");
            return 2;
        }
        args.map_path = dev.map_path;
        if (!request_session_from_orchestrator(dev, args)) return 1;
        args.auto_start = true;
    } else if (!dev.connect_address.empty()) {
        // Direct-connect: skip the orchestrator entirely. Used by player
        // #2+ when player #1 ran with --server and shared the port + token.
        std::string host;
        uldum::u16 port = 7777;
        if (!split_host_port(dev.connect_address, host, port)) {
            std::fprintf(stderr, "Invalid --connect value '%s' (expected host:port)\n",
                         dev.connect_address.c_str());
            return 2;
        }
        args.net_mode        = uldum::network::Mode::Client;
        args.connect_address = host;
        args.port            = port;
        args.map_path        = dev.map_path.empty() ? std::string{"maps/test_map.uldmap"} : dev.map_path;
        if (!dev.token_hex.empty()) {
            args.auth_token.assign(dev.token_hex.begin(), dev.token_hex.end());
        }
        args.auto_start = true;
    } else if (dev.host_mode) {
        // LAN host mode — direct UDP listener, no orchestrator.
        args.net_mode   = uldum::network::Mode::Host;
        args.port       = 7777;
        args.map_path   = dev.map_path.empty() ? std::string{"maps/test_map.uldmap"} : dev.map_path;
        args.auto_start = true;
    } else if (!dev.map_path.empty()) {
        // Offline single-player.
        args.net_mode   = uldum::network::Mode::Offline;
        args.map_path   = dev.map_path;
        args.auto_start = true;
    }
    // else: no auto-start flags — drop into the dev console.

    uldum::Engine engine;
    if (!engine.init(args)) {
        uldum::log::error("Main", "Engine initialization failed");
        return 1;
    }

    engine.run();
    engine.shutdown();
    return 0;
}
