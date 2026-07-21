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

private:
    u32 m_next_id = 0;
};

} // namespace uldum::simulation
