#pragma once

#include "simulation/simulation.h"
#include "script/script.h"

namespace uldum::asset { class AssetManager; }
namespace uldum::map { class MapManager; }
namespace uldum::audio { class AudioEngine; }

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

    // Server-authoritative scene switch, shared by host (uldum_dev) and worker
    // (uldum_worker): wipe + swap terrain, load the new scene's placements,
    // reset the Lua VM, and re-run main() for `scene_name`. `pre_main` fires
    // after the VM is re-inited + constants loaded but BEFORE main() runs — the
    // caller MUST use it to re-install every callback the VM reset cleared
    // (set_hud / set_script / wire_to_network / set_scene_switch_fn / render
    // hooks / etc.), exactly as at initial init_game. Returns the new
    // preplaced/dynamic id boundary (world().entities.next_id()) so the caller
    // can set_placement_count; UINT32_MAX on failure. Does NOT touch the
    // network barrier (broadcast / mark_self_loaded / finish) — that stays with
    // the caller's loop.
    u32 switch_scene(map::MapManager& map, asset::AssetManager& assets,
                     std::string_view scene_name, PreMainHook pre_main = {});

    // Host scene-switch barrier, shared by uldum_dev and uldum_worker. The
    // network transitions (broadcast, mark_self_loaded, finish) and the
    // finalize-scene bookkeeping live here; the caller injects only the two
    // parts that genuinely differ between host and worker.
    //
    // `local_teardown` wipes the caller's own scene state after the barrier
    // opens (host: sim + terrain + renderer/camera/HUD; worker: sim + terrain
    // + headless HUD model). `pre_main` re-installs the caller's VM callbacks
    // before the new scene's main() runs (see switch_scene).
    using TeardownHook = std::function<void(const std::string& scene_name)>;

    // Phase 1: broadcast S_SCENE_SWITCH, run `local_teardown`, stash the target,
    // and mark self loaded. Call once when a LoadScene request is drained.
    void begin_scene_switch(NetworkManager& net, std::string_view scene_name,
                            const TeardownHook& local_teardown);

    // Phase 2, poll form: once every peer has acked, re-run the new scene
    // (switch_scene + set_placement_count) and close the barrier. Safe to call
    // every frame — self-guards on is_scene_switching + all_peers_loaded + a
    // stashed target. Returns true only on the frame the switch completes.
    bool try_finish_scene_switch(NetworkManager& net, map::MapManager& map,
                                 asset::AssetManager& assets,
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
    // these, capturing and calling through. Call AFTER init_game (which installs
    // the script's own trigger dispatch that item-sync chains onto).
    // NOTE: HUD sync is NOT here — it must be wired at set_hud time (before
    // init_game runs main()), by whoever owns the Hud. See worker_main /
    // Engine::start_session.
    void wire_to_network(NetworkManager& net);

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

    simulation::Simulation  m_simulation;
    script::ScriptEngine    m_script;
    audio::AudioEngine*     m_audio = nullptr;   // retained from init_game for switch_scene's VM re-init

    // Set in begin_scene_switch (phase 1), consumed in try_finish_scene_switch
    // (phase 2). Non-empty == a scene switch is mid-barrier. Replaces the copies
    // the host loop and worker loop each kept, so the two can't drift.
    std::string             m_pending_finalize_scene;
};

} // namespace uldum::network
