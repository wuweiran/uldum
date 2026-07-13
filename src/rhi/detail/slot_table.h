#pragma once

// Shared slot-table helpers for the RHI backends (Vulkan + GLES). Both
// backends store resources in parallel vectors of "records" indexed by a
// generational Handle{index, generation}: a free list recycles indices,
// and each record carries a `generation` that's bumped on (re)allocation
// so a stale handle (whose generation no longer matches the slot's) is
// detectably invalid.
//
// These three helpers were previously copy-pasted, verbatim, into each
// backend's anonymous namespace (the GLES copy's comment literally read
// "mirror the Vulkan backend"). Centralizing them means the allocation /
// generation / lookup logic lives in exactly one place.
//
// Contract assumed of `Rec`: a `u32 generation` member.
// Contract assumed of `H`: `bool is_valid() const`, `u32 index`,
// `u32 generation` (see rhi/handles.h).

#include "core/types.h"

#include <vector>

namespace uldum::rhi::detail {

// Pick a free index from the recycled list, or push a fresh slot.
// Returns the index; the caller fills the record and bumps its
// generation.
template <typename Rec>
u32 acquire_slot(std::vector<Rec>& records, std::vector<u32>& free_list) {
    if (!free_list.empty()) {
        u32 idx = free_list.back();
        free_list.pop_back();
        return idx;
    }
    u32 idx = static_cast<u32>(records.size());
    records.emplace_back();
    return idx;
}

// Bump a record's generation, skipping 0 (generation 0 means "slot
// free / never allocated"). Returns the new generation.
template <typename Rec>
u32 bump_generation(Rec& rec) {
    rec.generation += 1;
    if (rec.generation == 0) rec.generation = 1;
    return rec.generation;
}

// Retire a slot on destroy: advance the generation instead of resetting
// it to 0. Generation MUST stay monotonic across a slot's whole lifetime —
// resetting to 0 and letting the next allocation bump 0→1 makes every
// allocation of a given index carry generation 1, so an old handle
// re-validates against an unrelated recycled resource (stale-handle
// detection defeated). Advancing here means the freed slot holds a
// generation that was never handed out, and the next allocation advances
// again to yet another fresh value — so no live handle can ever match a
// slot it doesn't own. The free list (not generation==0) is the source of
// truth for "is this index reusable".
template <typename Rec>
void retire_generation(Rec& rec) {
    bump_generation(rec);
}

// Generational lookup: validate the handle against the table and return
// a pointer to the live record, or nullptr if the handle is invalid,
// out of range, or stale (generation mismatch). The const overload
// mirrors it for read-only access. This is the single building block
// the backends' resolve()/*_record()/destroy_* prologues use instead of
// re-writing the three-line bounds+generation check ~50 times.
template <typename Rec, typename H>
Rec* lookup(std::vector<Rec>& records, H h) {
    if (!h.is_valid() || h.index >= records.size()) return nullptr;
    Rec& rec = records[h.index];
    return rec.generation == h.generation ? &rec : nullptr;
}

template <typename Rec, typename H>
const Rec* lookup(const std::vector<Rec>& records, H h) {
    if (!h.is_valid() || h.index >= records.size()) return nullptr;
    const Rec& rec = records[h.index];
    return rec.generation == h.generation ? &rec : nullptr;
}

} // namespace uldum::rhi::detail
