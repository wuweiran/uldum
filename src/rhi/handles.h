#pragma once

// Opaque, backend-agnostic resource handles. Two u32s — fits in a
// register pair; safe to pass by value. `index` selects a record slot
// inside the active backend; `generation` is bumped each time the slot
// is allocated, so a handle whose `generation` no longer matches the
// slot's current generation is detectably stale.
//
// A default-constructed handle has generation == 0 and is invalid;
// freshly-created handles always have generation >= 1. Resource tables
// track free slots separately and retain their retired generation.
//
// Tag types make each handle kind distinct (BufferHandle can't be
// implicitly converted to TextureHandle, etc.) at zero runtime cost.

#include "core/types.h"

namespace uldum::rhi {

template <typename Tag>
struct Handle {
    u32 index = 0;
    u32 generation = 0;

    bool is_valid() const { return generation != 0; }
    bool operator==(const Handle&) const = default;
};

namespace tags {
    struct Buffer;
    struct Texture;
    struct Sampler;
    struct ShaderModule;
    struct DescriptorSetLayout;
    struct DescriptorSet;
    struct PipelineLayout;
    struct Pipeline;
}

using BufferHandle              = Handle<tags::Buffer>;
using TextureHandle             = Handle<tags::Texture>;
using SamplerHandle             = Handle<tags::Sampler>;
using ShaderModuleHandle        = Handle<tags::ShaderModule>;
using DescriptorSetLayoutHandle = Handle<tags::DescriptorSetLayout>;
using DescriptorSetHandle       = Handle<tags::DescriptorSet>;
using PipelineLayoutHandle      = Handle<tags::PipelineLayout>;
using PipelineHandle            = Handle<tags::Pipeline>;

} // namespace uldum::rhi
