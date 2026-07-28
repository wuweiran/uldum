#include "asset/asset.h"
#include "map/map.h"
#include "network/game_server.h"
#include "network/network.h"
#include "network/lobby.h"
#include "script/script.h"
#include "hud/hud.h"
#include "hud/hud_loader.h"
#include "simulation/command_system.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#  include <io.h>      // _isatty, _fileno
#else
#  include <unistd.h>  // isatty, STDIN_FILENO
#endif

#include <chrono>
#include <charconv>
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

static bool parse_u16_port(const char* s, uldum::u16& out) {
    if (!s || *s == '\0') return false;
    const char* end = s;
    while (*end) ++end;
    uldum::u32 value = 0;
    auto [ptr, ec] = std::from_chars(s, end, value);
    if (ec != std::errc{} || ptr != end) return false;
    if (value < 1 || value > 65535) return false;
    out = static_cast<uldum::u16>(value);
    return true;
}

static bool parse_args(int argc, char* argv[], WorkerArgs& out) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            out.map_path = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (!parse_u16_port(argv[++i], out.port)) {
                std::fprintf(stderr, "Error: --port must be 1..65535.\n");
                return false;
            }
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
    if (!assets.init("engine")) {
        uldum::log::error(TAG, "Failed to init AssetManager (engine.uldpak missing?)");
        return 1;
    }

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
    if (!map.load_map(args.map_path, assets)) {
        uldum::log::error(TAG, "Failed to load map '{}'", args.map_path);
        return 1;
    }
    {
        auto& sim = server.simulation();
        sim.world().clear_entities();
        if (!uldum::simulation::register_map_types(sim, assets, map.map_root())) {
            uldum::log::error(TAG, "Failed to load map types for '{}'", args.map_path);
            return 1;
        }
        sim.set_terrain(&map.terrain());
        uldum::simulation::apply_scene_data(sim, map.mutable_scene());
    }

    // Preplaced/dynamic id boundary — mirror of Engine::start_session (engine.cpp).
    // load_map + apply_scene_data just built the initial scene's preplaced
    // entities (ids [0,N) from placements.bin); init_game's main() (run
    // below) then creates DYNAMIC entities starting at N. The worker MUST ship N
    // in S_WELCOME so the client — which builds the same preplaced set locally —
    // agrees on the boundary. Without this m_placement_count stayed 0, the client
    // logged PLACEMENT DESYNC (host=0 vs client=N) and every id-keyed message was
    // mis-routed. Captured here BEFORE init_game; published after init_host below.
    const uldum::u32 initial_placement_count =
        server.simulation().world().entities.next_id();

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

    // Headless HUD MODEL. uldum_hud is a pure data layer (no RHI / renderer),
    // so the worker owns one to be the authoritative source of text tags +
    // in-game UI nodes. The map's main() calls CreateNode / CreateTextTag on
    // it; without a HUD those bindings early-return and clients see nothing.
    // Rendering happens only on clients, which receive the sync packets.
    uldum::hud::Hud hud;
    hud.init();
    // Load the map's hud.json into the headless HUD so CreateNode("id", …) can
    // resolve its template. Templates come ONLY from hud.json (add_node_template);
    // without this the worker's CreateNode finds no template, creates nothing,
    // and clients see no nodes/dialogs (text tags still work — they're inline,
    // template-free). Viewport 0×0 is fine: the 'nodes' block is pure JSON
    // storage; only composites resolve rects (unused headless).
    {
        std::string hud_path = map.map_root() + "/hud.json";
        uldum::hud::load_from_asset(hud, hud_path, 0, 0);
    }

    // Server→client + inbound wiring, factored into one lambda so it runs at
    // boot AND again after every scene switch (GameServer::switch_scene resets
    // the Lua VM, clearing all script callbacks — the pre_main hook must
    // re-install them before the new scene's main() runs). Mirror of the host's
    // wire_host_broadcasts / start_session wiring, minus the renderer/HUD-render
    // halves the headless worker doesn't have.
    std::string pending_scene;   // set by set_scene_switch_fn; drained in the loop
    auto wire_server = [&](uldum::script::ScriptEngine& /*s*/) {
        // HUD sync at set_hud time — before main() creates nodes/tags.
        hud.set_sync_fn([&network](const std::vector<uldum::u8>& pkt, uldum::u32 mask) {
            network.host_hud_sync(pkt, mask);
        });
        server.script().set_hud(&hud);
        network.set_script(&server.script());       // inbound C_NODE_EVENT → Lua
        network.set_hud_replay_source(&hud);         // join-replay of persistent HUD
        server.wire_to_network(network);             // effects/items/abilities/EndGame → clients
        // Next LoadScene: capture the target; the main loop drives the barrier.
        server.script().set_scene_switch_fn([&pending_scene](std::string_view scene) {
            pending_scene.assign(scene);
        });
    };

    // Init networking as host FIRST — before init_game runs main(). The HUD
    // sync and the map's main()-time CreateNode / CreateTextTag both need a live
    // host_hud_sync target, so the network must exist before main() runs.
    // init_host only needs the simulation + commands (ready now), not init_game.
    uldum::simulation::CommandSystem commands;
    commands.init(&server.simulation().world());
    uldum::u32 max_players = static_cast<uldum::u32>(map.manifest().players.size());
    if (!network.init_host(args.port, max_players, server.simulation(), commands)) {
        uldum::log::error(TAG, "Failed to init network on port {}", args.port);
        return 1;
    }
    // Publish the initial preplaced/dynamic boundary captured after load_map
    // (before init_game's main() created any dynamics). Shipped in S_WELCOME so
    // the client's boundary matches — fixes the PLACEMENT DESYNC (host=0).
    network.set_placement_count(initial_placement_count);

    // init_game runs the initial scene's main(); wire_server as its pre_main so
    // callbacks are live when main() creates HUD nodes / fires effects. pre_main
    // runs after script.init (bindings) but before main, so wire_to_network's
    // chain onto on_item_picked_up etc. captures the real bindings.
    if (!server.init_game(map, nullptr,
                          [&](uldum::script::ScriptEngine& s) {
                              pre_main(s);   // GAME_SESSION global (stdin config)
                              wire_server(s);
                          })) {
        uldum::log::error(TAG, "Failed to init game");
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

    // WC3-style lobby start countdown. Once every connected peer is seated
    // (and at least one is present), count down, then auto-commit — the worker
    // has no Start button. Open slots are allowed at start (they go unused), so
    // a 2-slot map starts with a single player. Any lobby change that unseats a
    // peer or empties the lobby cancels the countdown; it restarts when the
    // seated condition holds again.
    constexpr float kLobbyStartCountdown = 5.0f;
    bool  lobby_counting = false;
    float lobby_countdown = 0.0f;

    // Lobby startup timeout — backstop against a worker that spawns but whose
    // game never starts: a join that's rejected (wrong map hash, bad token)
    // leaves the peer unregistered, and a dev that opens a session then closes
    // without seating leaves nobody. Neither trips the abandoned-session end
    // (that needs the game to have STARTED), so without this the worker would
    // sit in Lobby forever holding its port. If no game has committed within
    // this window of spawn, the worker exits so the orchestrator reaper frees
    // the port. This is also the only reaper once the orchestrator is down
    // (it leaves workers orphaned on shutdown, by design).
    constexpr float kLobbyStartupTimeout = 120.0f;

    while (running) {
        auto now = std::chrono::steady_clock::now();
        float frame_dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;

        // Cap to avoid spiral of death
        if (frame_dt > 0.25f) frame_dt = 0.25f;

        // Network receive
        network.update(frame_dt);

        // Lobby start countdown. Ready = still in the lobby, at least one
        // connected peer, and every connected peer seated. The condition is
        // recomputed from live state each loop, so a join-seatless / leave /
        // slot-release flips it false and cancels the countdown automatically.
        if (network.phase() == uldum::network::Phase::Lobby) {
            bool ready = network.connected_peer_count() > 0 &&
                         network.all_connected_peers_seated();
            if (ready) {
                if (!lobby_counting) {
                    lobby_counting  = true;
                    lobby_countdown = kLobbyStartCountdown;
                    uldum::log::info(TAG, "All players seated — starting in {:.0f}s",
                                     kLobbyStartCountdown);
                }
                lobby_countdown -= frame_dt;
                if (lobby_countdown <= 0.0f) {
                    lobby_counting = false;
                    uldum::log::info(TAG, "Countdown complete — committing start");
                    network.host_commit_start();
                    // Worker is headless and pre-loaded — it's always "loaded"
                    // for handshake purposes. Mark self immediately; once all
                    // clients send C_LOAD_DONE, host_finish_start wraps up below.
                    network.mark_self_loaded();
                }
            } else if (lobby_counting) {
                lobby_counting = false;
                uldum::log::info(TAG, "Lobby changed — start countdown cancelled");
            }

            // Startup-timeout backstop (see kLobbyStartupTimeout). Only checked
            // while still in Lobby — once the game starts, the abandoned-session
            // end (network side) takes over.
            float lobby_elapsed = std::chrono::duration<float>(now - start_time).count();
            if (lobby_elapsed >= kLobbyStartupTimeout) {
                uldum::log::info(TAG, "No game started within {:.0f}s of spawn — exiting",
                                 kLobbyStartupTimeout);
                running = false;
                break;
            }
        }

        // Transition Loading → Playing once every peer has sent C_LOAD_DONE.
        if (network.phase() == uldum::network::Phase::Loading &&
            network.all_peers_loaded()) {
            uldum::log::info(TAG, "All peers loaded — finishing start");
            network.host_finish_start();
        }

        // Scene switch (Lua LoadScene). Two-phase barrier mirroring the host:
        //   Phase 1: a pending switch → tell clients (reliable-ordered, they
        //            react first), wipe our terrain/entities locally, mark self
        //            loaded, and stash the target for phase 2.
        //   Phase 2: once every peer acked C_LOAD_DONE → reload the scene +
        //            re-run main (GameServer::switch_scene, re-wiring via
        //            wire_server as pre_main), refresh the placement boundary,
        //            then burst spawns + resume ticks (host_finish_scene_switch).
        static std::string finalize_scene;
        if (!pending_scene.empty() && !network.is_scene_switching()) {
            std::string scene = std::move(pending_scene);
            pending_scene.clear();
            network.host_broadcast_scene_switch(scene);
            // Local server teardown: wipe entities + swap terrain so the world
            // is empty across the barrier. switch_scene re-wipes idempotently in
            // phase 2, so this just gets us to the empty state now (no placements).
            server.simulation().world().clear_entities();
            map.switch_scene_terrain_only(scene, assets);
            network.reset_local_view();
            // Reset the headless HUD model too — mirror of the host's
            // scene_switch_local_teardown (m_hud.reset_scene_state()). Without
            // this the previous scene's Lua-created nodes (e.g. the "N waves"
            // composite) and text tags linger in the worker's Hud and get
            // resurrected on clients by the phase-2 spawn-burst emit_state_to
            // replay — even though each client cleared them in its own teardown.
            hud.reset_scene_state();
            finalize_scene = std::move(scene);
            network.mark_self_loaded();
        }
        if (network.is_scene_switching() && !finalize_scene.empty() &&
            network.all_peers_loaded()) {
            std::string scene = std::move(finalize_scene);
            finalize_scene.clear();
            uldum::u32 boundary = server.switch_scene(map, assets, scene, wire_server);
            if (boundary != UINT32_MAX) {
                network.set_placement_count(boundary);
            }
            network.host_finish_scene_switch();
            uldum::log::info(TAG, "Scene switch '{}' complete — sim resuming", scene);
        }

        // Tick simulation (paused while a scene switch is in flight).
        if (network.is_game_started() && !network.is_paused() &&
            !network.is_scene_switching()) {
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
        result["duration_s"]   = duration_s;
        result["winning_team"] = ed.winning_team;
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
