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

// Terrain material: up to 4 ground layer textures. Splatmap weights are per-vertex attributes.
struct TerrainMaterial {
    static constexpr u32 MAX_LAYERS = 4;
    GpuTexture    layers[MAX_LAYERS]{};  // ground textures (grass, dirt, stone, sand)
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    u32           layer_count = 0;
};

} // namespace uldum::render
