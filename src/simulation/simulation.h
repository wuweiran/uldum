#pragma once

#include "simulation/world.h"
#include "simulation/type_registry.h"
#include "simulation/ability_def.h"
#include "simulation/pathfinding.h"
#include "simulation/spatial_query.h"

#include <vector>

namespace uldum::asset { class AssetManager; }
namespace uldum::map { struct TerrainData; }

namespace uldum::simulation {

// Per-pair alliance flags (asymmetric: A→B can differ from B→A)
struct AllianceFlags {
    bool allied  = false;   // won't auto-attack, friendly AoE won't hit
    bool passive = false;   // won't fight back when attacked by this player
};

class Simulation {
public:
    bool init(asset::AssetManager& assets);
    void shutdown();
    void tick(float dt);

    // Set terrain for pathfinding, height queries, and spatial grid sizing.
    void set_terrain(const map::TerrainData* terrain);

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

    const map::TerrainData* terrain() const { return m_terrain; }

private:
    World           m_world;
    TypeRegistry    m_types;
    AbilityRegistry m_abilities;
    Pathfinder      m_pathfinder;
    SpatialGrid     m_spatial_grid;
    const map::TerrainData* m_terrain = nullptr;

    // Alliance table: m_alliances[a * m_player_count + b] = flags from a toward b
    std::vector<AllianceFlags> m_alliances;
    u32 m_player_count = 0;
};

} // namespace uldum::simulation
