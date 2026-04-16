#pragma once

#include "core/types.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace uldum::asset { struct TextureData; }
namespace uldum::rhi { class VulkanRhi; }

namespace uldum::render {

struct GpuTexture {
    VkImage       image  = VK_NULL_HANDLE;
    VmaAllocation alloc  = VK_NULL_HANDLE;
    VkImageView   view   = VK_NULL_HANDLE;
    VkSampler     sampler = VK_NULL_HANDLE;
    u32           width  = 0;
    u32           height = 0;
};

// Upload CPU texture data to GPU. Creates image, view, and sampler.
GpuTexture upload_texture(rhi::VulkanRhi& rhi, const asset::TextureData& tex);

// Upload raw RGBA pixel data to GPU.
GpuTexture upload_texture_rgba(rhi::VulkanRhi& rhi, const u8* pixels, u32 width, u32 height);

// Upload an array of RGBA layers as a sampler2DArray. Each layer must be width*height*4 bytes.
// layers_data: array of pointers to RGBA pixel data, one per layer.
// srgb: true for color textures (default), false for data textures (normal maps).
GpuTexture upload_texture_array(rhi::VulkanRhi& rhi, const u8** layers_data, u32 layer_count,
                                u32 width, u32 height, bool srgb = true);

// Upload 6 RGBA faces as a samplerCube. Face order: +X, -X, +Y, -Y, +Z, -Z.
// Each face must be width*height*4 bytes.
GpuTexture upload_texture_cubemap(rhi::VulkanRhi& rhi, const u8* faces[6],
                                  u32 width, u32 height);

void destroy_texture(rhi::VulkanRhi& rhi, GpuTexture& tex);

} // namespace uldum::render
