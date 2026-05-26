#include "asset/asset.h"
#include "map/map.h"
#include "network/game_server.h"
#include "network/network.h"
#include "network/lobby.h"
#include "simulation/command_system.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#  include <io.h>      // _isatty, _fileno
#else
#  include <unistd.h>  // isatty, STDIN_FILENO
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// uldum_worker: one process per active game session. Headless — no
// renderer, no audio, no window. Game-agnostic (no `game.json`).
// Operator (or `uldum_server` orchestrator) supplies the map path
// via `--map` and a listen port via `--port`.

static constexpr const char* TAG = "Worker";
static constexpr float TICK_DT = 1.0f / 32.0f;

struct WorkerArgs {
    std::string map_path;
    uldum::u16  port = 7777;
};

// Config the orchestrator pipes to us on stdin. All fields are optional.
// When the worker runs standalone (LAN / dev), there's no stdin config
// at all — the worker checks `isatty(stdin)` and skips the read.
struct WorkerStdinConfig {
    // Per-player tokens the worker will accept in `C_JOIN`. Empty list
    // means "auth disabled" — accept any joiner. The orchestrator passes
    // one entry per expected player.
    std::vector<std::string> tokens;

    // Arbitrary per-session blob forwarded from the game backend. Becomes
    // a `GAME_SESSION` global in Lua before `main()` runs. Null when the
    // game backend supplied no `initial_data`.
    nlohmann::json session = nlohmann::json::value_t::null;
};

static bool stdin_has_data() {
    // If stdin is a terminal (interactive), nobody is piping us a config.
    // Skip the read so the worker doesn't block waiting for keyboard input.
#ifdef _WIN32
    return _isatty(_fileno(stdin)) == 0;
#else
    return isatty(STDIN_FILENO) == 0;
#endif
}

static bool read_stdin_config(WorkerStdinConfig& out) {
    if (!stdin_has_data()) return false;

    std::ostringstream ss;
    ss << std::cin.rdbuf();
    std::string body = ss.str();
    if (body.empty()) return false;

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(body);
    } catch (const std::exception& e) {
        uldum::log::error(TAG, "Failed to parse stdin config JSON: {}", e.what());
        return false;
    }

    if (auto it = doc.find("tokens"); it != doc.end() && it->is_array()) {
        for (const auto& t : *it) {
            if (t.is_string()) out.tokens.push_back(t.get<std::string>());
        }
    }
    if (auto it = doc.find("session"); it != doc.end()) {
        out.session = *it;
    }
    return true;
}

static void print_usage() {
    std::fprintf(stderr,
        "Usage: uldum_worker --map <path> [--port <n>]\n"
        "  --map <path>    Path to a packaged .uldmap (required).\n"
        "  --port <n>      Listen port. Default: 7777.\n");
}

static bool parse_args(int argc, char* argv[], WorkerArgs& out) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            out.map_path = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            out.port = static_cast<uldum::u16>(std::stoi(argv[++i]));
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return false;
        }
    }
    if (out.map_path.empty()) {
        std::fprintf(stderr, "Error: --map is required.\n");
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    // Workers send their end-of-session result on stdout; logs must
    // not pollute it. Other binaries keep the default stdout sink.
    uldum::log::redirect_to_stderr();

    WorkerArgs args;
    if (!parse_args(argc, argv, args)) {
        print_usage();
        return 2;
    }

    uldum::log::info(TAG, "=== Uldum Session Worker ===");
    uldum::log::info(TAG, "Map: {}, Port: {}", args.map_path, args.port);

    // Optional stdin config from orchestrator: token table, etc.
    WorkerStdinConfig cfg;
    bool have_config = read_stdin_config(cfg);
    if (have_config) {
        uldum::log::info(TAG, "stdin config: {} token(s)", cfg.tokens.size());
    } else {
        uldum::log::info(TAG, "no stdin config — running in standalone mode (accept all joiners)");
    }

    // Init subsystems (no renderer, no audio, no window)
    uldum::asset::AssetManager assets;
    assets.init("engine");

    uldum::network::GameServer server;
    uldum::network::NetworkManager network;
    uldum::map::MapManager map;

    // Init simulation
    if (!server.init_simulation(assets)) {
        uldum::log::error(TAG, "Failed to init simulation");
        return 1;
    }

    // Load map
    if (!map.init()) {
        uldum::log::error(TAG, "Failed to init map manager");
        return 1;
    }
    if (!map.load_map(args.map_path, assets, server.simulation())) {
        uldum::log::error(TAG, "Failed to load map '{}'", args.map_path);
        return 1;
    }

    // Set water layers
    {
        std::vector<uldum::u8> shallow, deep;
        map.tileset().get_water_layer_ids(shallow, deep);
        map.terrain().set_water_layers(shallow, deep);
    }

    // Init game logic. The pre-main hook injects GAME_SESSION (when the
    // orchestrator supplied initial_data) so the map's main() can read
    // session-specific config like loadouts or custom rules.
    auto pre_main = [&cfg](uldum::script::ScriptEngine& s) {
        if (!cfg.session.is_null()) {
            s.set_global_from_json("GAME_SESSION", cfg.session);
            uldum::log::info(TAG, "GAME_SESSION global installed from stdin config");
        }
    };
    if (!server.init_game(map, nullptr, pre_main)) {
        uldum::log::error(TAG, "Failed to init game");
        return 1;
    }

    // Set terrain for pathfinding
    server.simulation().set_terrain(&map.terrain());

    // Init networking as host
    uldum::simulation::CommandSystem commands;
    commands.init(&server.simulation().world());
    uldum::u32 max_players = static_cast<uldum::u32>(map.manifest().players.size());
    if (!network.init_host(args.port, max_players, server.simulation(), commands)) {
        uldum::log::error(TAG, "Failed to init network on port {}", args.port);
        return 1;
    }

    // The worker is authoritative-but-not-playing: populate the lobby
    // from the manifest with every slot left Open. Clients claim slots via
    // their dev-UI lobby; the worker auto-commits Start once all slots are
    // filled (no one on the worker presses a Start button).
    uldum::network::init_lobby_from_manifest(network.lobby_state(), args.map_path, map.manifest());

    // Set map script-hash for client validation. SHA-256 over every
    // .lua file in the map (lexicographic order). Mismatch is a hard
    // reject — clients on a different map version can't join.
    network.set_map_hash(map.compute_script_hash(assets));
    network.set_disconnect_timeout(map.manifest().disconnect_timeout);
    network.set_pause_on_disconnect(map.manifest().pause_on_disconnect);

    // Auth-on-join: install the validator only when we received a token
    // table from the orchestrator. Standalone (no stdin config) skips
    // this and falls back to the engine's "no callback = accept all"
    // default — preserves Phase 23's LAN / dev path unchanged.
    if (have_config && !cfg.tokens.empty()) {
        std::unordered_set<std::string> token_set(cfg.tokens.begin(), cfg.tokens.end());
        network.set_auth_callback(
            [token_set = std::move(token_set)](std::span<const uldum::u8> token,
                                               std::string_view /*peer_name*/) {
                std::string presented(reinterpret_cast<const char*>(token.data()), token.size());
                return token_set.contains(presented);
            });
    }

    uldum::log::info(TAG, "Worker started — waiting for {} players on port {}",
                     max_players, args.port);

    // Main loop: fixed timestep simulation
    auto start_time = std::chrono::steady_clock::now();
    auto last_time  = start_time;
    float accumulator = 0;
    bool running = true;

    while (running) {
        auto now = std::chrono::steady_clock::now();
        float frame_dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;

        // Cap to avoid spiral of death
        if (frame_dt > 0.25f) frame_dt = 0.25f;

        // Network receive
        network.update(frame_dt);

        // Auto-commit the lobby when it's fully claimed and every connected
        // peer has seated themselves. Worker has no UI to press Start, so
        // these two conditions together are the trigger.
        if (network.phase() == uldum::network::Phase::Lobby &&
            uldum::network::lobby_all_slots_claimed(network.lobby_state()) &&
            network.all_connected_peers_seated()) {
            uldum::log::info(TAG, "Lobby full — auto-committing start");
            network.host_commit_start();
            // Worker is headless and pre-loaded — it's always "loaded" for
            // handshake purposes. Mark self immediately; once all clients
            // send C_LOAD_DONE, host_finish_start wraps things up below.
            network.mark_self_loaded();
        }

        // Transition Loading → Playing once every peer has sent C_LOAD_DONE.
        if (network.phase() == uldum::network::Phase::Loading &&
            network.all_peers_loaded()) {
            uldum::log::info(TAG, "All peers loaded — finishing start");
            network.host_finish_start();
        }

        // Tick simulation
        if (network.is_game_started() && !network.is_paused()) {
            accumulator += frame_dt;
            static uldum::u32 tick_counter = 0;
            while (accumulator >= TICK_DT) {
                server.tick(TICK_DT);
                tick_counter++;
                network.host_broadcast_tick(tick_counter);
                accumulator -= TICK_DT;
            }
        }

        // Game-end: Lua called EndGame. Drop out of the main loop so the
        // worker can write its result to stdout and exit. The orchestrator
        // (parent) reads that result and forwards it to the configured
        // webhook URL. Give the network a beat to flush S_END to clients
        // before tearing down.
        if (network.is_game_ended()) {
            uldum::log::info(TAG, "EndGame received — finalizing session");
            for (int i = 0; i < 10; ++i) {  // ~100ms of S_END drain
                network.update(0.01f);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            running = false;
            break;
        }

        // Sleep to avoid busy-waiting (~1ms granularity)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Final result on stdout — the orchestrator captures this exact bytes
    // and POSTs it (verbatim) to the game backend's configured webhook URL.
    {
        auto end_time = std::chrono::steady_clock::now();
        uldum::f32 duration_s = std::chrono::duration<uldum::f32>(end_time - start_time).count();
        const auto& ed = network.end_data();

        nlohmann::json result;
        result["duration_s"] = duration_s;
        result["winner_id"]  = ed.winner_id;
        // stats_json is whatever the Lua side passed to EndGame — a raw
        // string. We forward it as-is; if it parses as JSON, include
        // the parsed form; otherwise pass the string through.
        if (!ed.stats_json.empty()) {
            try {
                result["stats"] = nlohmann::json::parse(ed.stats_json);
            } catch (...) {
                result["stats_raw"] = ed.stats_json;
            }
        }
        std::cout << result.dump() << std::endl;
        std::cout.flush();
    }

    network.shutdown();
    server.shutdown();
    map.shutdown();
    assets.shutdown();

    uldum::log::info(TAG, "Worker shut down");
    return 0;
}
