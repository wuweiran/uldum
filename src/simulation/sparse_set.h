#pragma once

#include "core/types.h"

#include <array>
#include <cassert>
#include <memory>
#include <span>
#include <vector>

namespace uldum::simulation {

template <typename T>
class SparseSet {
public:
    T& add(u32 id, T&& value) {
        assert(!has(id) && "SparseSet: duplicate add");

        u32 dense_index = static_cast<u32>(m_dense_data.size());
        sparse_slot(id) = dense_index;
        m_dense_ids.push_back(id);
        m_dense_data.push_back(std::move(value));
        return m_dense_data.back();
    }

    void remove(u32 id) {
        u32* sparse = sparse_slot_if_present(id);
        if (!sparse || *sparse == INVALID) return;

        u32 dense_index = *sparse;
        u32 last_index  = static_cast<u32>(m_dense_data.size()) - 1;

        if (dense_index != last_index) {
            u32 last_id = m_dense_ids[last_index];
            m_dense_data[dense_index] = std::move(m_dense_data[last_index]);
            m_dense_ids[dense_index]  = last_id;
            sparse_slot(last_id)      = dense_index;
        }

        m_dense_data.pop_back();
        m_dense_ids.pop_back();
        *sparse = INVALID;
    }

    T* get(u32 id) {
        u32* sparse = sparse_slot_if_present(id);
        if (!sparse || *sparse == INVALID) return nullptr;
        return &m_dense_data[*sparse];
    }

    const T* get(u32 id) const {
        const u32* sparse = sparse_slot_if_present(id);
        if (!sparse || *sparse == INVALID) return nullptr;
        return &m_dense_data[*sparse];
    }

    bool has(u32 id) const {
        const u32* sparse = sparse_slot_if_present(id);
        return sparse && *sparse != INVALID;
    }

    u32 count() const { return static_cast<u32>(m_dense_data.size()); }

    std::span<T>        data()       { return m_dense_data; }
    std::span<const T>  data() const { return m_dense_data; }
    std::span<const u32> ids() const { return m_dense_ids; }

    void clear() {
        m_dense_data.clear();
        m_dense_ids.clear();
        m_sparse_pages.clear();
    }

private:
    static constexpr u32 INVALID = UINT32_MAX;
    static constexpr u32 PAGE_SIZE = 4096;
    using SparsePage = std::array<u32, PAGE_SIZE>;

    u32& sparse_slot(u32 id) {
        usize page_index = id / PAGE_SIZE;
        if (page_index >= m_sparse_pages.size()) {
            m_sparse_pages.resize(page_index + 1);
        }
        auto& page = m_sparse_pages[page_index];
        if (!page) {
            page = std::make_unique<SparsePage>();
            page->fill(INVALID);
        }
        return (*page)[id % PAGE_SIZE];
    }

    u32* sparse_slot_if_present(u32 id) {
        usize page_index = id / PAGE_SIZE;
        if (page_index >= m_sparse_pages.size() || !m_sparse_pages[page_index]) {
            return nullptr;
        }
        return &(*m_sparse_pages[page_index])[id % PAGE_SIZE];
    }

    const u32* sparse_slot_if_present(u32 id) const {
        usize page_index = id / PAGE_SIZE;
        if (page_index >= m_sparse_pages.size() || !m_sparse_pages[page_index]) {
            return nullptr;
        }
        return &(*m_sparse_pages[page_index])[id % PAGE_SIZE];
    }

    std::vector<T> m_dense_data;
    std::vector<u32> m_dense_ids;
    std::vector<std::unique_ptr<SparsePage>> m_sparse_pages;
};

} // namespace uldum::simulation
