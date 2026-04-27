#include "render/gpu_texture.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/texture.h"
#include "core/log.h"

#include <cstring>
#include <vector>

namespace uldum::render {

static constexpr const char* TAG = "GpuTexture";

GpuTexture upload_texture_rgba(rhi::VulkanRhi& rhi, const u8* pixels, u32 width, u32 height, bool srgb, bool clamp) {
    VkDevice device = rhi.device();
    VmaAllocator allocator = rhi.allocator();
    VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * 4;

    // Create staging buffer
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VmaAllocation staging_alloc = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size  = image_size;
        buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        if (vmaCreateBuffer(allocator, &buf_ci, &alloc_ci, &staging_buffer, &staging_alloc, nullptr) != VK_SUCCESS) {
            log::error(TAG, "Failed to create staging buffer");
            return {};
        }

        void* mapped = nullptr;
        vmaMapMemory(allocator, staging_alloc, &mapped);
        std::memcpy(mapped, pixels, image_size);
        vmaUnmapMemory(allocator, staging_alloc);
    }

    // Create GPU image
    GpuTexture tex{};
    tex.width  = width;
    tex.height = height;

    VkImageCreateInfo img_ci{};
    img_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType     = VK_IMAGE_TYPE_2D;
    img_ci.format        = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    img_ci.extent        = {width, height, 1};
    img_ci.mipLevels     = 1;
    img_ci.arrayLayers   = 1;
    img_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc_ci{};
    img_alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(allocator, &img_ci, &img_alloc_ci, &tex.image, &tex.alloc, nullptr) != VK_SUCCESS) {
        log::error(TAG, "Failed to create GPU image");
        vmaDestroyBuffer(allocator, staging_buffer, staging_alloc);
        return {};
    }

    // Copy staging → image via one-shot command buffer
    VkCommandBuffer cmd = rhi.begin_oneshot();

    // Transition UNDEFINED → TRANSFER_DST
    VkImageMemoryBarrier2 to_transfer{};
    to_transfer.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_transfer.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    to_transfer.srcAccessMask = VK_ACCESS_2_NONE;
    to_transfer.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.image         = tex.image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &to_transfer;
    vkCmdPipelineBarrier2(cmd, &dep);

    // Copy buffer → image
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging_buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST → SHADER_READ_ONLY
    VkImageMemoryBarrier2 to_shader{};
    to_shader.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_shader.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_shader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_shader.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_shader.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_shader.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.image         = tex.image;
    to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    dep.pImageMemoryBarriers = &to_shader;
    vkCmdPipelineBarrier2(cmd, &dep);

    rhi.end_oneshot(cmd);

    // Destroy staging buffer
    vmaDestroyBuffer(allocator, staging_buffer, staging_alloc);

    // Create image view
    VkImageViewCreateInfo view_ci{};
    view_ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image    = tex.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format   = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device, &view_ci, nullptr, &tex.view) != VK_SUCCESS) {
        log::error(TAG, "Failed to create image view");
        vmaDestroyImage(allocator, tex.image, tex.alloc);
        return {};
    }

    // Create sampler
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter    = VK_FILTER_LINEAR;
    sampler_ci.minFilter    = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                    : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_ci.addressModeV = sampler_ci.addressModeU;
    sampler_ci.addressModeW = sampler_ci.addressModeU;
    sampler_ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_ci.maxLod       = 0.0f;

    if (vkCreateSampler(device, &sampler_ci, nullptr, &tex.sampler) != VK_SUCCESS) {
        log::error(TAG, "Failed to create sampler");
        vkDestroyImageView(device, tex.view, nullptr);
        vmaDestroyImage(allocator, tex.image, tex.alloc);
        return {};
    }

    log::info(TAG, "Uploaded texture {}x{}", width, height);
    return tex;
}

GpuTexture upload_texture(rhi::VulkanRhi& rhi, const asset::TextureData& data) {
    if (data.pixels.empty() || data.width == 0 || data.height == 0) {
        log::error(TAG, "Invalid texture data");
        return {};
    }

    // Convert to RGBA if needed
    if (data.channels == 4) {
        return upload_texture_rgba(rhi, data.pixels.data(), data.width, data.height);
    }

    // Expand to RGBA
    std::vector<u8> rgba(data.width * data.height * 4);
    for (u32 i = 0; i < data.width * data.height; ++i) {
        u8 r = 0, g = 0, b = 0, a = 255;
        if (data.channels >= 1) r = data.pixels[i * data.channels];
        if (data.channels >= 2) g = data.pixels[i * data.channels + 1];
        if (data.channels >= 3) b = data.pixels[i * data.channels + 2];
        if (data.channels >= 4) a = data.pixels[i * data.channels + 3];
        // For 1-2 channels, replicate R to G and B for grayscale
        if (data.channels == 1) { g = r; b = r; }
        if (data.channels == 2) { b = 0; a = g; g = r; }
        rgba[i * 4]     = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = a;
    }

    return upload_texture_rgba(rhi, rgba.data(), data.width, data.height);
}

GpuTexture upload_texture_array(rhi::VulkanRhi& rhi, const u8** layers_data, u32 layer_count,
                                u32 width, u32 height, bool srgb) {
    VkDevice device = rhi.device();
    VmaAllocator allocator = rhi.allocator();
    VkDeviceSize layer_size = static_cast<VkDeviceSize>(width) * height * 4;
    VkDeviceSize total_size = layer_size * layer_count;

    // Create staging buffer with all layers packed
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VmaAllocation staging_alloc = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size  = total_size;
        buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        if (vmaCreateBuffer(allocator, &buf_ci, &alloc_ci, &staging_buffer, &staging_alloc, nullptr) != VK_SUCCESS) {
            log::error(TAG, "Failed to create array texture staging buffer");
            return {};
        }

        void* mapped = nullptr;
        vmaMapMemory(allocator, staging_alloc, &mapped);
        for (u32 i = 0; i < layer_count; ++i) {
            std::memcpy(static_cast<u8*>(mapped) + i * layer_size, layers_data[i], layer_size);
        }
        vmaUnmapMemory(allocator, staging_alloc);
    }

    // Create GPU array image
    GpuTexture tex{};
    tex.width  = width;
    tex.height = height;

    VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo img_ci{};
    img_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType     = VK_IMAGE_TYPE_2D;
    img_ci.format        = fmt;
    img_ci.extent        = {width, height, 1};
    img_ci.mipLevels     = 1;
    img_ci.arrayLayers   = layer_count;
    img_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc_ci{};
    img_alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(allocator, &img_ci, &img_alloc_ci, &tex.image, &tex.alloc, nullptr) != VK_SUCCESS) {
        log::error(TAG, "Failed to create array texture image");
        vmaDestroyBuffer(allocator, staging_buffer, staging_alloc);
        return {};
    }

    VkCommandBuffer cmd = rhi.begin_oneshot();

    // Transition UNDEFINED → TRANSFER_DST (all layers)
    VkImageMemoryBarrier2 to_transfer{};
    to_transfer.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_transfer.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    to_transfer.srcAccessMask = VK_ACCESS_2_NONE;
    to_transfer.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.image         = tex.image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count};

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &to_transfer;
    vkCmdPipelineBarrier2(cmd, &dep);

    // Copy each layer from staging buffer to image array layer
    std::vector<VkBufferImageCopy> regions(layer_count);
    for (u32 i = 0; i < layer_count; ++i) {
        regions[i] = {};
        regions[i].bufferOffset     = i * layer_size;
        regions[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, i, 1};
        regions[i].imageExtent      = {width, height, 1};
    }
    vkCmdCopyBufferToImage(cmd, staging_buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           layer_count, regions.data());

    // Transition TRANSFER_DST → SHADER_READ_ONLY (all layers)
    VkImageMemoryBarrier2 to_shader{};
    to_shader.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_shader.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_shader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_shader.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_shader.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_shader.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.image         = tex.image;
    to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count};

    dep.pImageMemoryBarriers = &to_shader;
    vkCmdPipelineBarrier2(cmd, &dep);

    rhi.end_oneshot(cmd);
    vmaDestroyBuffer(allocator, staging_buffer, staging_alloc);

    // Create image view (2D_ARRAY)
    VkImageViewCreateInfo view_ci{};
    view_ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image    = tex.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_ci.format   = fmt;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count};

    if (vkCreateImageView(device, &view_ci, nullptr, &tex.view) != VK_SUCCESS) {
        log::error(TAG, "Failed to create array image view");
        vmaDestroyImage(allocator, tex.image, tex.alloc);
        return {};
    }

    // Create sampler
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter    = VK_FILTER_LINEAR;
    sampler_ci.minFilter    = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_ci.maxLod       = 0.0f;

    if (vkCreateSampler(device, &sampler_ci, nullptr, &tex.sampler) != VK_SUCCESS) {
        log::error(TAG, "Failed to create array sampler");
        vkDestroyImageView(device, tex.view, nullptr);
        vmaDestroyImage(allocator, tex.image, tex.alloc);
        return {};
    }

    log::info(TAG, "Uploaded texture array {}x{} x {} layers", width, height, layer_count);
    return tex;
}

GpuTexture upload_texture_cubemap(rhi::VulkanRhi& rhi, const u8* faces[6],
                                  u32 width, u32 height) {
    VkDevice device = rhi.device();
    VmaAllocator allocator = rhi.allocator();
    VkDeviceSize face_size = static_cast<VkDeviceSize>(width) * height * 4;
    VkDeviceSize total_size = face_size * 6;

    // Staging buffer with all 6 faces packed
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VmaAllocation staging_alloc = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size  = total_size;
        buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        if (vmaCreateBuffer(allocator, &buf_ci, &alloc_ci, &staging_buffer, &staging_alloc, nullptr) != VK_SUCCESS) {
            log::error(TAG, "Failed to create cubemap staging buffer");
            return {};
        }

        void* mapped = nullptr;
        vmaMapMemory(allocator, staging_alloc, &mapped);
        for (u32 i = 0; i < 6; ++i) {
            std::memcpy(static_cast<u8*>(mapped) + i * face_size, faces[i], face_size);
        }
        vmaUnmapMemory(allocator, staging_alloc);
    }

    GpuTexture tex{};
    tex.width  = width;
    tex.height = height;

    VkImageCreateInfo img_ci{};
    img_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    img_ci.imageType     = VK_IMAGE_TYPE_2D;
    img_ci.format        = VK_FORMAT_R8G8B8A8_SRGB;
    img_ci.extent        = {width, height, 1};
    img_ci.mipLevels     = 1;
    img_ci.arrayLayers   = 6;
    img_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc_ci{};
    img_alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(allocator, &img_ci, &img_alloc_ci, &tex.image, &tex.alloc, nullptr) != VK_SUCCESS) {
        log::error(TAG, "Failed to create cubemap image");
        vmaDestroyBuffer(allocator, staging_buffer, staging_alloc);
        return {};
    }

    VkCommandBuffer cmd = rhi.begin_oneshot();

    VkImageMemoryBarrier2 to_transfer{};
    to_transfer.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_transfer.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    to_transfer.srcAccessMask = VK_ACCESS_2_NONE;
    to_transfer.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.image         = tex.image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &to_transfer;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy regions[6]{};
    for (u32 i = 0; i < 6; ++i) {
        regions[i].bufferOffset     = i * face_size;
        regions[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, i, 1};
        regions[i].imageExtent      = {width, height, 1};
    }
    vkCmdCopyBufferToImage(cmd, staging_buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, regions);

    VkImageMemoryBarrier2 to_shader{};
    to_shader.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_shader.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_shader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_shader.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_shader.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_shader.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.image         = tex.image;
    to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};

    dep.pImageMemoryBarriers = &to_shader;
    vkCmdPipelineBarrier2(cmd, &dep);

    rhi.end_oneshot(cmd);
    vmaDestroyBuffer(allocator, staging_buffer, staging_alloc);

    VkImageViewCreateInfo view_ci{};
    view_ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image    = tex.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    view_ci.format   = VK_FORMAT_R8G8B8A8_SRGB;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};

    if (vkCreateImageView(device, &view_ci, nullptr, &tex.view) != VK_SUCCESS) {
        log::error(TAG, "Failed to create cubemap image view");
        vmaDestroyImage(allocator, tex.image, tex.alloc);
        return {};
    }

    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter    = VK_FILTER_LINEAR;
    sampler_ci.minFilter    = VK_FILTER_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_ci.maxLod       = 0.0f;

    if (vkCreateSampler(device, &sampler_ci, nullptr, &tex.sampler) != VK_SUCCESS) {
        log::error(TAG, "Failed to create cubemap sampler");
        vkDestroyImageView(device, tex.view, nullptr);
        vmaDestroyImage(allocator, tex.image, tex.alloc);
        return {};
    }

    log::info(TAG, "Uploaded cubemap {}x{}", width, height);
    return tex;
}

void destroy_texture(rhi::VulkanRhi& rhi, GpuTexture& tex) {
    VkDevice device = rhi.device();
    VmaAllocator allocator = rhi.allocator();

    if (tex.sampler) vkDestroySampler(device, tex.sampler, nullptr);
    if (tex.view)    vkDestroyImageView(device, tex.view, nullptr);
    if (tex.image)   vmaDestroyImage(allocator, tex.image, tex.alloc);
    tex = {};
}

} // namespace uldum::render
