#pragma once

#include "simulation/world.h"
#include "simulation/type_registry.h"
#include "simulation/pathfinding.h"

namespace uldum::asset { class AssetManager; }
namespace uldum::map { struct TerrainData; }

namespace uldum::simulation {

class Simulation {
public:
    bool init(asset::AssetManager& assets);
    void shutdown();
    void tick(float dt);

    // Set terrain for pathfinding and height queries.
    void set_terrain(const map::TerrainData* terrain) { m_pathfinder.set_terrain(terrain); }

    World&       world()       { return m_world; }
    const World& world() const { return m_world; }

    TypeRegistry&       types()       { return m_types; }
    const TypeRegistry& types() const { return m_types; }

    Pathfinder&       pathfinder()       { return m_pathfinder; }
    const Pathfinder& pathfinder() const { return m_pathfinder; }

private:
    World        m_world;
    TypeRegistry m_types;
    Pathfinder   m_pathfinder;
};

} // namespace uldum::simulation
