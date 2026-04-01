#pragma once

#include "core/types.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/mat4x4.hpp>

namespace uldum::rhi { class VulkanRhi; }

namespace uldum::render {

static constexpr u32 SHADOW_MAP_SIZE = 2048;

// Shadow map resources: depth texture rendered from the light's perspective.
struct ShadowMap {
    VkImage       depth_image  = VK_NULL_HANDLE;
    VmaAllocation depth_alloc  = VK_NULL_HANDLE;
    VkImageView   depth_view   = VK_NULL_HANDLE;
    VkSampler     sampler      = VK_NULL_HANDLE;  // comparison sampler for PCF
    u32           size         = SHADOW_MAP_SIZE;
};

// Uniform buffer for shadow data passed to main-pass shaders.
struct ShadowUBO {
    glm::mat4 light_vp;  // light view-projection matrix
};

struct ShadowBuffer {
    VkBuffer      buffer = VK_NULL_HANDLE;
    VmaAllocation alloc  = VK_NULL_HANDLE;
    void*         mapped = nullptr;  // persistently mapped
};

bool create_shadow_map(rhi::VulkanRhi& rhi, ShadowMap& sm);
void destroy_shadow_map(rhi::VulkanRhi& rhi, ShadowMap& sm);

bool create_shadow_buffer(rhi::VulkanRhi& rhi, ShadowBuffer& sb);
void destroy_shadow_buffer(rhi::VulkanRhi& rhi, ShadowBuffer& sb);

// Compute the light view-projection matrix for a directional light.
// Fits an orthographic frustum around the given scene bounds.
glm::mat4 compute_light_vp(const glm::vec3& light_dir, const glm::vec3& scene_center, f32 scene_radius);

} // namespace uldum::render
