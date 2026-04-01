#include "render/shadow.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "core/log.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace uldum::render {

static constexpr const char* TAG = "Shadow";

bool create_shadow_map(rhi::VulkanRhi& rhi, ShadowMap& sm) {
    VkDevice device = rhi.device();
    VmaAllocator allocator = rhi.allocator();
    sm.size = SHADOW_MAP_SIZE;

    // Create depth image
    VkImageCreateInfo img_ci{};
    img_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType     = VK_IMAGE_TYPE_2D;
    img_ci.format        = VK_FORMAT_D32_SFLOAT;
    img_ci.extent        = {sm.size, sm.size, 1};
    img_ci.mipLevels     = 1;
    img_ci.arrayLayers   = 1;
    img_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    if (vmaCreateImage(allocator, &img_ci, &alloc_ci, &sm.depth_image, &sm.depth_alloc, nullptr) != VK_SUCCESS) {
        log::error(TAG, "Failed to create shadow map image");
        return false;
    }

    // Create depth image view
    VkImageViewCreateInfo view_ci{};
    view_ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image    = sm.depth_image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format   = VK_FORMAT_D32_SFLOAT;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    if (vkCreateImageView(device, &view_ci, nullptr, &sm.depth_view) != VK_SUCCESS) {
        log::error(TAG, "Failed to create shadow map image view");
        return false;
    }

    // Create comparison sampler for PCF shadow sampling
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter     = VK_FILTER_LINEAR;
    sampler_ci.minFilter     = VK_FILTER_LINEAR;
    sampler_ci.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_ci.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_ci.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_ci.borderColor   = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;  // no shadow outside map
    sampler_ci.compareEnable = VK_TRUE;
    sampler_ci.compareOp     = VK_COMPARE_OP_LESS;

    if (vkCreateSampler(device, &sampler_ci, nullptr, &sm.sampler) != VK_SUCCESS) {
        log::error(TAG, "Failed to create shadow map sampler");
        return false;
    }

    log::info(TAG, "Shadow map created: {}x{}", sm.size, sm.size);
    return true;
}

void destroy_shadow_map(rhi::VulkanRhi& rhi, ShadowMap& sm) {
    VkDevice device = rhi.device();
    VmaAllocator allocator = rhi.allocator();

    if (sm.sampler)     vkDestroySampler(device, sm.sampler, nullptr);
    if (sm.depth_view)  vkDestroyImageView(device, sm.depth_view, nullptr);
    if (sm.depth_image) vmaDestroyImage(allocator, sm.depth_image, sm.depth_alloc);
    sm = {};
}

bool create_shadow_buffer(rhi::VulkanRhi& rhi, ShadowBuffer& sb) {
    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size  = sizeof(ShadowUBO);
    buf_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo alloc_info{};
    if (vmaCreateBuffer(rhi.allocator(), &buf_ci, &alloc_ci, &sb.buffer, &sb.alloc, &alloc_info) != VK_SUCCESS) {
        log::error(TAG, "Failed to create shadow UBO");
        return false;
    }
    sb.mapped = alloc_info.pMappedData;
    return true;
}

void destroy_shadow_buffer(rhi::VulkanRhi& rhi, ShadowBuffer& sb) {
    if (sb.buffer) vmaDestroyBuffer(rhi.allocator(), sb.buffer, sb.alloc);
    sb = {};
}

glm::mat4 compute_light_vp(const glm::vec3& light_dir, const glm::vec3& scene_center, f32 scene_radius) {
    // Light "position" far away along the light direction (light_dir points toward the light)
    glm::vec3 light_pos = scene_center + glm::normalize(light_dir) * scene_radius;

    // Determine up vector (avoid parallel with light_dir)
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    if (std::abs(glm::dot(glm::normalize(light_dir), up)) > 0.99f) {
        up = {0.0f, 1.0f, 0.0f};
    }

    glm::mat4 light_view = glm::lookAt(light_pos, scene_center, up);
    // Use ZO (zero-to-one) depth range for Vulkan
    glm::mat4 light_proj = glm::orthoRH_ZO(
        -scene_radius, scene_radius,
        -scene_radius, scene_radius,
        0.1f, scene_radius * 2.5f
    );
    // Flip Y for Vulkan
    light_proj[1][1] *= -1.0f;

    return light_proj * light_view;
}

} // namespace uldum::render
