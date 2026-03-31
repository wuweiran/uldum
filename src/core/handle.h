#pragma once

#include "core/types.h"

#include <cassert>
#include <vector>

namespace uldum {

// ── Handle ─────────────────────────────────────────────────────────────────
// Generational handle — index + generation prevents use-after-free.
template <typename T>
struct Handle {
    u32 index      = UINT32_MAX;
    u32 generation = 0;

    bool is_valid() const { return index != UINT32_MAX; }

    bool operator==(const Handle&) const = default;
};

// ── ResourcePool ───────────────────────────────────────────────────────────
// Generational slot map. O(1) add/get/remove. Handles stay stable.
template <typename T>
class ResourcePool {
public:
    Handle<T> add(T&& value) {
        u32 idx;
        if (!m_free_list.empty()) {
            idx = m_free_list.back();
            m_free_list.pop_back();
        } else {
            idx = static_cast<u32>(m_slots.size());
            m_slots.push_back({});
        }

        auto& slot = m_slots[idx];
        slot.data  = std::move(value);
        slot.alive = true;
        ++m_count;

        return {idx, slot.generation};
    }

    T* get(Handle<T> h) {
        if (h.index >= m_slots.size()) return nullptr;
        auto& slot = m_slots[h.index];
        if (!slot.alive || slot.generation != h.generation) return nullptr;
        return &slot.data;
    }

    const T* get(Handle<T> h) const {
        if (h.index >= m_slots.size()) return nullptr;
        const auto& slot = m_slots[h.index];
        if (!slot.alive || slot.generation != h.generation) return nullptr;
        return &slot.data;
    }

    bool remove(Handle<T> h) {
        if (h.index >= m_slots.size()) return false;
        auto& slot = m_slots[h.index];
        if (!slot.alive || slot.generation != h.generation) return false;

        slot.alive = false;
        slot.generation++;
        slot.data = T{};
        m_free_list.push_back(h.index);
        --m_count;
        return true;
    }

    void clear() {
        m_slots.clear();
        m_free_list.clear();
        m_count = 0;
    }

    u32 count() const { return m_count; }

private:
    struct Slot {
        T    data{};
        u32  generation = 0;
        bool alive      = false;
    };

    std::vector<Slot> m_slots;
    std::vector<u32>  m_free_list;
    u32               m_count = 0;
};

} // namespace uldum
