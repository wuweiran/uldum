#pragma once

#include "simulation/entity_types.h"

#include <cassert>

namespace uldum::simulation {

// Hands out ECS entity ids — one monotonic counter, never recycled. A dead
// entity's id is never reused, so a stale reference (e.g. a Lua handle to a
// destroyed unit) reliably fails World::contains rather than aliasing a new
// entity.
class EntityAllocator {
public:
    Entity allocate() {
        assert(m_next_id != UINT32_MAX && "EntityAllocator: entity id space exhausted");
        if (m_next_id == UINT32_MAX) return {};
        return Entity{m_next_id++};
    }

    Entity reserve(u32 id) {
        if (id == UINT32_MAX) return {};
        if (id >= m_next_id) m_next_id = id + 1;
        return Entity{id};
    }

    u32 next_id() const { return m_next_id; }

    // Restart the id counter at 0. Only safe when EVERY entity is being
    // destroyed in the same breath (World::clear_entities) AND no handle
    // survives the wipe — true at a scene boundary, where the world is
    // fully cleared and Lua state doesn't persist. This lets each scene's
    // placement entities occupy a deterministic [0, N) range on host and
    // client alike.
    void reset() { m_next_id = 0; }

private:
    u32 m_next_id = 0;
};

} // namespace uldum::simulation
