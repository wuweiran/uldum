#pragma once

#include "render/gpu_texture.h"
#include "rhi/handles.h"

namespace uldum::rhi { class VulkanRhi; }

namespace uldum::render {

// Simple mesh material: one diffuse texture.
struct MeshMaterial {
    GpuTexture               diffuse{};
    rhi::DescriptorSetHandle descriptor_set{};
};

// Terrain material: layer textures as sampler2DArrays.
struct TerrainMaterial {
    static constexpr u32     MAX_LAYERS = 16;
    GpuTexture               layer_array{};   // sampler2DArray: all layer diffuse textures
    GpuTexture               normal_array{};  // sampler2DArray: all layer normal maps (optional)
    rhi::DescriptorSetHandle descriptor_set{};
    u32                      layer_count = 0;
    bool                     has_normals = false;
};

} // namespace uldum::render
