#pragma once

#include "simulation/handle_types.h"

#include <vector>

namespace uldum::simulation {

// Manages handle allocation with generational reuse.
// Allocates IDs, tracks generations, recycles freed slots.
class HandleAllocator {
public:
    Handle allocate() {
        u32 id;
        if (!m_free_list.empty()) {
            id = m_free_list.back();
            m_free_list.pop_back();
        } else {
            id = static_cast<u32>(m_generations.size());
            m_generations.push_back(0);
        }
        return {id, m_generations[id]};
    }

    void free(Handle h) {
        if (!is_valid(h)) return;
        m_generations[h.id]++;
        m_free_list.push_back(h.id);
    }

    bool is_valid(Handle h) const {
        return h.id < m_generations.size() && m_generations[h.id] == h.generation;
    }

    u32 capacity() const { return static_cast<u32>(m_generations.size()); }

private:
    std::vector<u32> m_generations;
    std::vector<u32> m_free_list;
};

} // namespace uldum::simulation
