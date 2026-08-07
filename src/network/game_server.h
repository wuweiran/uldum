#pragma once

#include "network/protocol.h"
#include "simulation/simulation.h"
#include "script/script.h"

#include <span>
#include <unordered_map>
#include <unordered_set>

namespace uldum::asset { class AssetManager; }
namespace uldum::map { class MapManager; }
namespace uldum::audio { class AudioEngine; }
namespace uldum::hud { class Hud; }

namespace uldum::network {

class NetworkManager;

// GameServer owns the authoritative game state: simulation and scripting.
// For local play, the engine calls tick() directly — zero overhead.
// For multiplayer (Phase 13b+), commands arrive from the network and
// state deltas are broadcast to clients.
class GameServer {
public:
    // Two-phase init:
    // 1) init_simulation — must be called before map loading (map registers types/entities)
    // 2) init_game — called after map load (alliances, scripting, map scripts)
    bool init_simulation(asset::AssetManager& assets);

    // `pre_main_hook` fires after Lua is initialized + constants are
    // loaded but BEFORE the map's `main()` runs. The worker uses this
    // to inject the GAME_SESSION global so map scripts can read
    // session-supplied data from inside main().
    using PreMainHook = std::function<void(script::ScriptEngine&)>;
    bool init_game(map::MapManager& map,
                   audio::AudioEngine* audio = nullptr,
                   PreMainHook pre_main_hook = {});

    // Finish a scene switch after terrain and preplaced entities are loaded:
    // reset the Lua VM and re-run main() for `scene_name`. `pre_main` fires
    // after the VM is re-inited + constants loaded but before main() runs.
    bool switch_scene(map::MapManager& map, std::string_view scene_name,
                      PreMainHook pre_main = {});

    // Host scene-switch barrier, shared by uldum_dev and uldum_worker. The
    // network transitions (broadcast, mark_self_loaded, finish) and the
    // finalize-scene bookkeeping live here; the caller injects only the two
    // parts that genuinely differ between host and worker.
    //
    // `local_teardown` loads terrain and preplaced entities while resetting the
    // caller's local scene state. `pre_main` re-installs VM callbacks before the
    // new scene's main() runs.
    using TeardownHook = std::function<void(const std::string& scene_name)>;

    // Phase 1: broadcast S_SCENE_SWITCH, run `local_teardown`, stash the target,
    // and mark self loaded. Call once when a LoadScene request is drained.
    void begin_scene_switch(NetworkManager& net, std::string_view scene_name,
                            const TeardownHook& local_teardown);

    // Phase 2, poll form: once every peer has acked, reset Lua, run the new
    // scene's main(), and close the barrier. Safe to call every frame.
    bool try_finish_scene_switch(NetworkManager& net, map::MapManager& map,
                                 const PreMainHook& pre_main = {});

    // True between begin_scene_switch and try_finish_scene_switch's completion.
    bool scene_switch_pending() const { return !m_pending_finalize_scene.empty(); }

    void shutdown();

    // Run one simulation tick (fixed dt). Ticks simulation then scripts.
    void tick(f32 dt);

    // Wire the authoritative server's outbound broadcasts to `net`. This is
    // the SHARED server→client plumbing that both the host (uldum_dev) and the
    // headless worker (uldum_worker) install, so a script event (effect, item
    // pickup, ability change, cooldown, EndGame, player leave) reaches clients
    // the same way regardless of who runs the server. Installs only the
    // send/broadcast halves — never touches a renderer / camera / selection /
    // HUD, so it is safe on a headless worker. The host chains its own
    // local-player apply (play the effect on its renderer, etc.) ON TOP of
    // these, capturing and calling through. Call from init_game's pre-main hook,
    // after ScriptEngine::init installs trigger dispatch and before main() runs.
    // NOTE: HUD sync is NOT here — it must be wired at set_hud time (before
    // init_game runs main()), by whoever owns the Hud. See worker_main /
    // Engine::start_session.
    void wire_to_network(NetworkManager& net);
    void set_command_system(simulation::CommandSystem& commands) {
        m_commands = &commands;
    }
    void set_hud_replay_source(hud::Hud* hud) { m_hud_replay = hud; }
    void set_placement_count(u32 count) { m_placement_count = count; }
    u32 placement_count() const { return m_placement_count; }
    bool receive_order(simulation::Player player, std::span<const u8> packet);
    void receive_node_event(simulation::Player player, std::span<const u8> packet);
    void peer_disconnected(u32 player_id);
    void player_dropped(u32 player_id);
    void broadcast_tick(NetworkManager& net, u32 tick);
    void send_spawn_burst(NetworkManager& net, u32 peer_id,
                          simulation::Player player);
    void broadcast_update(NetworkManager& net, u32 entity_id,
                          std::span<const u8> packet);
    void broadcast_entity_event(NetworkManager& net, u32 entity_id,
                                std::span<const u8> packet);
    void replay_persistent_state(NetworkManager& net, u32 peer_id,
                                 simulation::Player player);
    void clear_replication();
    void clear_replication(simulation::Player player);

    // ── Accessors ────────────────────────────────────────────────────────
    simulation::Simulation&       simulation()       { return m_simulation; }
    const simulation::Simulation& simulation() const { return m_simulation; }

    script::ScriptEngine&         script()           { return m_script; }
    const script::ScriptEngine&   script() const     { return m_script; }

private:
    // Shared "load this scene's scripts + run main()" tail used by both
    // init_game and switch_scene: set_player_count, pathing/grid, script paths,
    // save dir, engine constants, pre_main hook, then load + call main.lua for
    // `scene_name`. Assumes the script VM is freshly inited and the world's
    // entities for the scene already exist. Returns false on a hard error
    // (missing / erroring main).
    bool run_scene_scripts(map::MapManager& map, std::string_view scene_name,
                           const PreMainHook& pre_main);
    MaterializeData materialize_data(u32 entity_id) const;
    std::vector<ColdRecord> collect_cold_records(u32 entity_id) const;
    void send_spawn(NetworkManager& net, u32 peer_id,
                    simulation::Player player, u32 entity_id, bool born);
    void send_cold_batch(NetworkManager& net, u32 peer_id, u32 entity_id);
    void send_inventory_state(NetworkManager& net, u32 peer_id, u32 carrier_id);
    bool is_visible_to(u32 entity_id, simulation::Player player) const;

    simulation::Simulation  m_simulation;
    script::ScriptEngine    m_script;
    audio::AudioEngine*     m_audio = nullptr;   // retained from init_game for switch_scene's VM re-init
    hud::Hud*               m_hud_replay = nullptr;
    simulation::CommandSystem* m_commands = nullptr;
    u32 m_placement_count = 0;
    std::unordered_map<u32, std::unordered_set<u32>> m_known_by_player;
    std::unordered_map<u32, u32> m_controlled_unit_by_player;
    std::unordered_set<u32> m_prev_tick_entities;

    // Set in begin_scene_switch (phase 1), consumed in try_finish_scene_switch
    // (phase 2). Non-empty == a scene switch is mid-barrier. Replaces the copies
    // the host loop and worker loop each kept, so the two can't drift.
    std::string             m_pending_finalize_scene;
};

} // namespace uldum::network
