#pragma once

#include "simulation/world.h"
#include "simulation/type_registry.h"

namespace uldum::asset { class AssetManager; }

namespace uldum::simulation {

class Simulation {
public:
    bool init(asset::AssetManager& assets);
    void shutdown();
    void tick(float dt);

    World&       world()       { return m_world; }
    const World& world() const { return m_world; }

    TypeRegistry&       types()       { return m_types; }
    const TypeRegistry& types() const { return m_types; }

private:
    World        m_world;
    TypeRegistry m_types;
};

} // namespace uldum::simulation
