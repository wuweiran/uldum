#include "asset/asset.h"
#include "map/map.h"
#include "network/game_server.h"
#include "network/network.h"
#include "network/lobby.h"
#include "simulation/command_system.h"
#include "core/log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>

// Dedicated server: headless, no renderer/audio/window. Game-agnostic
// — no `game.json` reads, no per-game config bundled. Operator supplies
// a map path via `--map` and (optionally) a port via `--port`.

static constexpr const char* TAG = "Server";
static constexpr float TICK_DT = 1.0f / 32.0f;

struct ServerArgs {
    std::string map_path;
    uldum::u16  port = 7777;
};

static void print_usage() {
    std::fprintf(stderr,
        "Usage: uldum_server --map <path> [--port <n>]\n"
        "  --map <path>    Path to a packaged .uldmap (required).\n"
        "  --port <n>      Listen port. Default: 7777.\n");
}

static bool parse_args(int argc, char* argv[], ServerArgs& out) {
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
    ServerArgs args;
    if (!parse_args(argc, argv, args)) {
        print_usage();
        return 2;
    }

    uldum::log::info(TAG, "=== Uldum Dedicated Server ===");
    uldum::log::info(TAG, "Map: {}, Port: {}", args.map_path, args.port);

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

    // Init game logic (no renderer/audio — pass nullptr)
    if (!server.init_game(map)) {
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

    // Dedicated server is authoritative-but-not-playing: populate the lobby
    // from the manifest with every slot left Open. Clients claim slots via
    // their dev-UI lobby; the server auto-commits Start once all slots are
    // filled (no one on the server presses a Start button).
    uldum::network::init_lobby_from_manifest(network.lobby_state(), args.map_path, map.manifest());

    // Set map script-hash for client validation. SHA-256 over every
    // .lua file in the map (lexicographic order). Mismatch is a hard
    // reject — clients on a different map version can't join.
    network.set_map_hash(map.compute_script_hash(assets));
    network.set_disconnect_timeout(map.manifest().disconnect_timeout);
    network.set_pause_on_disconnect(map.manifest().pause_on_disconnect);

    uldum::log::info(TAG, "Server started — waiting for {} players on port {}",
                     max_players, args.port);

    // Main loop: fixed timestep simulation
    auto last_time = std::chrono::steady_clock::now();
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
        // peer has seated themselves. Server has no UI to press Start, so
        // these two conditions together are the trigger.
        if (network.phase() == uldum::network::Phase::Lobby &&
            uldum::network::lobby_all_slots_claimed(network.lobby_state()) &&
            network.all_connected_peers_seated()) {
            uldum::log::info(TAG, "Lobby full — auto-committing start");
            network.host_commit_start();
            // Server is headless and pre-loaded — it's always "loaded" for
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

        // Sleep to avoid busy-waiting (~1ms granularity)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    network.shutdown();
    server.shutdown();
    map.shutdown();
    assets.shutdown();

    uldum::log::info(TAG, "Server shut down");
    return 0;
}
