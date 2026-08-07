#pragma once

#include "network/protocol.h"
#include "simulation/simulation.h"

#include <optional>

namespace uldum::asset { class AssetManager; }
namespace uldum::simulation { struct LocalView; }

namespace uldum::network {

// GameClient owns the network client's game state — the sibling of GameServer.
// GameServer owns the authoritative simulation and ticks it; GameClient owns the
// replica: its World is the network mirror (fed by the wire + interpolation), its
// Vision the client's local fog. The client never calls Simulation::tick(), only
// Simulation::client_tick() (timer decay + fog). This replaces the old "client
// borrows an empty GameServer + world()/vision() overrides" hack — HUD / picker /
// target_filter / is_enemy read gameClient.simulation() directly.
class GameClient {
public:
    // Init the replica simulation. Call before map load — map content builds into
    // this sim's world, exactly as the host builds into GameServer's.
    bool init_simulation(asset::AssetManager& assets);

    void shutdown();

    // Per-frame client derivation — the twin of GameServer::tick(). The client
    // never runs the authoritative rules; this forwards to Simulation::client_tick
    // (timer decay + fog), which is correct at any variable dt (linear /
    // idempotent), so it runs per FRAME, not on the host's fixed TICK_DT step.
    void tick(f32 dt);
    void set_view(simulation::LocalView& view) { m_view = &view; }

    void apply_spawn(const MaterializeData& data, bool play_birth);
    void apply_hide(u32 entity_id);
    void apply_destroy(u32 entity_id);
    void apply_unit_state(UnitStateData state);
    void apply_projectile_state(ProjectileStateData state);
    void apply_attack_start(const AnimEventData& event);
    std::optional<u32> apply_projectile_dying(u32 entity_id);
    void apply_cold(ColdData cold);

    simulation::Simulation&       simulation()       { return m_simulation; }
    const simulation::Simulation& simulation() const { return m_simulation; }

private:
    void spawn_entity(const MaterializeData& data, bool play_birth);
    void destroy_entity(u32 entity_id);
    void apply_cold_record(u32 entity_id, const ColdRecord& record);
    u32 begin_snapshot_tick(u32 tick);
    void apply_interpolation();

    struct Snapshot {
        u32 tick = 0;
        f64 receive_time = 0;
        std::vector<UnitState> units;
        std::vector<ProjectileState> projectiles;
    };

    simulation::Simulation m_simulation;
    simulation::LocalView* m_view = nullptr;
    Snapshot m_snapshots[2];
    u32 m_snap_idx = 0;
    bool m_has_two_snaps = false;
};

} // namespace uldum::network
