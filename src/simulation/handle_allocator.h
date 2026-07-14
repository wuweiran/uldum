#pragma once

#include "simulation/handle_types.h"

#include <cassert>

namespace uldum::simulation {

class HandleAllocator {
public:
    Handle allocate() {
        assert(m_next_id != UINT32_MAX && "HandleAllocator: entity id space exhausted");
        if (m_next_id == UINT32_MAX) return {};
        return Handle{m_next_id++};
    }

    Handle reserve(u32 id) {
        if (id == UINT32_MAX) return {};
        if (id >= m_next_id) m_next_id = id + 1;
        return Handle{id};
    }

    u32 next_id() const { return m_next_id; }

private:
    u32 m_next_id = 0;
};

} // namespace uldum::simulation
