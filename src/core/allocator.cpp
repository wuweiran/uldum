#include "core/allocator.h"
#include "core/log.h"

namespace uldum {

// ── LinearAllocator ────────────────────────────────────────────────────────

LinearAllocator::LinearAllocator(usize capacity)
    : m_capacity(capacity)
{
    m_buffer = static_cast<u8*>(std::malloc(capacity));
    assert(m_buffer && "LinearAllocator: allocation failed");
}

LinearAllocator::~LinearAllocator() {
    std::free(m_buffer);
}

void* LinearAllocator::allocate(usize size, usize alignment) {
    // Align the current offset
    usize aligned = (m_offset + alignment - 1) & ~(alignment - 1);
    if (aligned + size > m_capacity) {
        log::error("LinearAlloc", "Out of memory: requested {} bytes, {} / {} used",
                   size, m_offset, m_capacity);
        return nullptr;
    }
    void* ptr = m_buffer + aligned;
    m_offset = aligned + size;
    return ptr;
}

void LinearAllocator::reset() {
    m_offset = 0;
}

// ── PoolAllocator ──────────────────────────────────────────────────────────

PoolAllocator::PoolAllocator(usize block_size, usize block_count)
    : m_block_size(block_size < sizeof(void*) ? sizeof(void*) : block_size)
    , m_block_count(block_count)
{
    m_buffer = static_cast<u8*>(std::malloc(m_block_size * block_count));
    assert(m_buffer && "PoolAllocator: allocation failed");
    reset();
}

PoolAllocator::~PoolAllocator() {
    std::free(m_buffer);
}

void PoolAllocator::reset() {
    m_used = 0;
    m_free_head = nullptr;

    // Build the free list (each free block stores a pointer to the next)
    for (usize i = 0; i < m_block_count; ++i) {
        void* block = m_buffer + i * m_block_size;
        *static_cast<void**>(block) = m_free_head;
        m_free_head = block;
    }
}

void* PoolAllocator::allocate() {
    if (!m_free_head) {
        log::error("PoolAlloc", "Out of blocks: {} / {} used", m_used, m_block_count);
        return nullptr;
    }
    void* block = m_free_head;
    m_free_head = *static_cast<void**>(m_free_head);
    ++m_used;
    return block;
}

void PoolAllocator::free(void* ptr) {
    assert(ptr >= m_buffer && ptr < m_buffer + m_block_size * m_block_count);
    *static_cast<void**>(ptr) = m_free_head;
    m_free_head = ptr;
    --m_used;
}

} // namespace uldum
