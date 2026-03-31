#pragma once

#include "core/types.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>

namespace uldum {

// ── LinearAllocator ────────────────────────────────────────────────────────
// Fast bump allocator. Allocations are O(1). No individual free — reset all
// at once. Ideal for per-frame scratch memory.
class LinearAllocator {
public:
    explicit LinearAllocator(usize capacity);
    ~LinearAllocator();

    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    void* allocate(usize size, usize alignment = alignof(std::max_align_t));
    void  reset();

    usize capacity() const { return m_capacity; }
    usize used()     const { return m_offset; }

private:
    u8*   m_buffer   = nullptr;
    usize m_capacity = 0;
    usize m_offset   = 0;
};

// ── PoolAllocator ──────────────────────────────────────────────────────────
// Fixed-size block allocator. O(1) alloc and free. Good for uniform objects
// like components, entities, particles.
class PoolAllocator {
public:
    PoolAllocator(usize block_size, usize block_count);
    ~PoolAllocator();

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    void* allocate();
    void  free(void* ptr);
    void  reset();

    usize block_size()  const { return m_block_size; }
    usize block_count() const { return m_block_count; }
    usize used_blocks() const { return m_used; }

private:
    u8*   m_buffer      = nullptr;
    void* m_free_head   = nullptr;
    usize m_block_size  = 0;
    usize m_block_count = 0;
    usize m_used        = 0;
};

} // namespace uldum
