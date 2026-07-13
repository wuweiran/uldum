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

    // Reserve a specific ID (for network-spawned entities).
    // Grows internal storage if needed. Returns a Handle with the current generation.
    Handle reserve(u32 id) {
        while (id >= m_generations.size()) {
            m_generations.push_back(0);
        }
        return {id, m_generations[id]};
    }

    // Reserve a specific (id, generation) dictated by an authority (the
    // network client mirroring the host). Force-sets the local generation
    // to the host's value so a recycled id can never leave the client
    // validating handles against a generation the host has advanced past.
    // The client only ever reserves ids the host assigns, so overwriting
    // the local generation here keeps the two allocators in lockstep
    // regardless of the client's own free/reuse bookkeeping.
    Handle reserve_at(u32 id, u32 generation) {
        reserve(id);
        m_generations[id] = generation;
        std::erase(m_free_list, id);
        return {id, generation};
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
