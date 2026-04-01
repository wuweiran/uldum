#pragma once

#include "simulation/world.h"
#include "simulation/type_registry.h"
#include "simulation/pathfinding.h"
#include "simulation/spatial_query.h"

namespace uldum::asset { class AssetManager; }
namespace uldum::map { struct TerrainData; }

namespace uldum::simulation {

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

    Pathfinder&       pathfinder()       { return m_pathfinder; }
    const Pathfinder& pathfinder() const { return m_pathfinder; }

    SpatialGrid&       spatial_grid()       { return m_spatial_grid; }
    const SpatialGrid& spatial_grid() const { return m_spatial_grid; }

private:
    World        m_world;
    TypeRegistry m_types;
    Pathfinder   m_pathfinder;
    SpatialGrid  m_spatial_grid;
};

} // namespace uldum::simulation
