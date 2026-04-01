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

void destroy_texture(rhi::VulkanRhi& rhi, GpuTexture& tex);

} // namespace uldum::render
