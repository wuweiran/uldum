#pragma once

#include "core/types.h"

#include <cassert>
#include <span>
#include <vector>

namespace uldum::simulation {

// Sparse set — O(1) add/remove/get, cache-friendly iteration over dense array.
// Indexed by handle ID (u32).
template <typename T>
class SparseSet {
public:
    T& add(u32 id, T&& value) {
        ensure_sparse(id);
        assert(!has(id) && "SparseSet: duplicate add");

        u32 dense_index = static_cast<u32>(m_dense_data.size());
        m_sparse[id] = dense_index;
        m_dense_ids.push_back(id);
        m_dense_data.push_back(std::move(value));
        return m_dense_data.back();
    }

    void remove(u32 id) {
        if (!has(id)) return;

        u32 dense_index = m_sparse[id];
        u32 last_index  = static_cast<u32>(m_dense_data.size()) - 1;

        if (dense_index != last_index) {
            // Swap with last element
            u32 last_id = m_dense_ids[last_index];
            m_dense_data[dense_index] = std::move(m_dense_data[last_index]);
            m_dense_ids[dense_index]  = last_id;
            m_sparse[last_id]         = dense_index;
        }

        m_dense_data.pop_back();
        m_dense_ids.pop_back();
        m_sparse[id] = INVALID;
    }

    T* get(u32 id) {
        if (!has(id)) return nullptr;
        return &m_dense_data[m_sparse[id]];
    }

    const T* get(u32 id) const {
        if (!has(id)) return nullptr;
        return &m_dense_data[m_sparse[id]];
    }

    bool has(u32 id) const {
        return id < m_sparse.size() && m_sparse[id] != INVALID;
    }

    u32 count() const { return static_cast<u32>(m_dense_data.size()); }

    // Iteration — dense array is tightly packed
    std::span<T>        data()       { return m_dense_data; }
    std::span<const T>  data() const { return m_dense_data; }
    std::span<const u32> ids() const { return m_dense_ids; }

    void clear() {
        m_dense_data.clear();
        m_dense_ids.clear();
        m_sparse.clear();
    }

private:
    static constexpr u32 INVALID = UINT32_MAX;

    void ensure_sparse(u32 id) {
        if (id >= m_sparse.size()) {
            m_sparse.resize(id + 1, INVALID);
        }
    }

    std::vector<T>   m_dense_data;
    std::vector<u32> m_dense_ids;
    std::vector<u32> m_sparse;
};

} // namespace uldum::simulation
