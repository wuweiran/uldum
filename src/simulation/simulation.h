#pragma once

#include "simulation/world.h"
#include "simulation/type_registry.h"
#include "simulation/ability_def.h"
#include "simulation/pathfinding.h"
#include "simulation/spatial_query.h"
#include "simulation/fog_of_war.h"

#include <string>
#include <string_view>
#include <vector>

namespace uldum::asset { class AssetManager; }
namespace uldum::map { struct TerrainData; }

namespace uldum::simulation {

// Per-pair alliance flags (asymmetric: A→B can differ from B→A)
struct AllianceFlags {
    bool allied        = false;   // won't auto-attack, friendly AoE won't hit
    bool passive       = false;   // won't fight back when attacked by this player
    bool shared_vision = false;   // shares fog of war vision with this player
};

class Simulation {
public:
    bool init(asset::AssetManager& assets);
    void shutdown();
    void tick(float dt);

    // Set terrain for pathfinding, height queries, and spatial grid sizing.
    void set_terrain(const map::TerrainData* terrain);

    // Apply all PathingBlocker components to the runtime pathing grid.
    // Call once after map entities are created.
    void sync_pathing_blockers();

    World&       world()       { return m_world; }
    const World& world() const { return m_world; }

    TypeRegistry&       types()       { return m_types; }
    const TypeRegistry& types() const { return m_types; }

    AbilityRegistry&       abilities()       { return m_abilities; }
    const AbilityRegistry& abilities() const { return m_abilities; }

    Pathfinder&       pathfinder()       { return m_pathfinder; }
    const Pathfinder& pathfinder() const { return m_pathfinder; }

    SpatialGrid&       spatial_grid()       { return m_spatial_grid; }
    const SpatialGrid& spatial_grid() const { return m_spatial_grid; }

    FogOfWar&       fog()       { return m_fog; }
    const FogOfWar& fog() const { return m_fog; }

    // ── Alliance system ──────────────────────────────────────────────────
    // Initialize with player count (call after loading manifest).
    void init_alliances(u32 player_count);

    // Set alliance from player_a toward player_b (asymmetric).
    void set_alliance(Player a, Player b, bool allied, bool passive = false);

    // Query: does player_a consider player_b an ally?
    bool is_allied(Player a, Player b) const;

    // Query: is player_a passive toward player_b?
    bool is_passive(Player a, Player b) const;

    // Query: does player_a consider player_b an enemy? (not allied and not same player)
    bool is_enemy(Player a, Player b) const;

    // Set shared vision from player_a toward player_b.
    void set_shared_vision(Player a, Player b, bool shared);

    // Query: does player_a share vision with player_b?
    bool has_shared_vision(Player a, Player b) const;

    // Static target-filter evaluation. Pure read of world + alliance
    // state — no side effects, no Lua. Both client and server reach
    // the same answer given the same world snapshot. Returns true iff
    // `target` is a valid candidate for an ability authored with this
    // `filter` cast by `caster`. Used by the HUD's drag-cast snap to
    // pick candidate units, and as the canonical commit-time check.
    bool target_filter_passes(const TargetFilter& filter,
                              Unit caster, Unit target) const;

    // Per-player names. Indexed by Player.id; empty string for unknown ids.
    // Populated by App::start_session from the finalized lobby, surfaced to
    // Lua via GetPlayerName(player).
    void set_player_names(std::vector<std::string> names) { m_player_names = std::move(names); }
    std::string_view get_player_name(Player p) const {
        if (p.id < m_player_names.size()) return m_player_names[p.id];
        return {};
    }

    const map::TerrainData* terrain() const { return m_terrain; }

private:
    World           m_world;
    TypeRegistry    m_types;
    AbilityRegistry m_abilities;
    Pathfinder      m_pathfinder;
    SpatialGrid     m_spatial_grid;
    FogOfWar        m_fog;
    const map::TerrainData* m_terrain = nullptr;

    // Alliance table: m_alliances[a * m_player_count + b] = flags from a toward b
    std::vector<AllianceFlags> m_alliances;
    u32 m_player_count = 0;

    std::vector<std::string> m_player_names;
};

} // namespace uldum::simulation
