#pragma once

#include "render/gpu_texture.h"

#include <vulkan/vulkan.h>

namespace uldum::rhi { class VulkanRhi; }

namespace uldum::render {

// Simple mesh material: one diffuse texture.
struct MeshMaterial {
    GpuTexture    diffuse{};
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
};

// Terrain material: layer textures as a sampler2DArray.
struct TerrainMaterial {
    static constexpr u32 MAX_LAYERS = 16;
    GpuTexture    layer_array{};    // sampler2DArray with all layer diffuse textures
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    u32           layer_count = 0;
};

} // namespace uldum::render
