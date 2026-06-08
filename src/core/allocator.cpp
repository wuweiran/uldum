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

} // namespace uldum
