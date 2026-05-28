#include "render/shadow.h"
#include "rhi/rhi.h"
#include "core/log.h"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace uldum::render {

static constexpr const char* TAG = "Shadow";

bool create_shadow_map(rhi::Rhi& rhi, ShadowMap& sm) {
    sm.size = SHADOW_MAP_SIZE;

    rhi::TextureDesc td{};
    td.width  = sm.size;
    td.height = sm.size;
    td.format = rhi::TextureFormat::D32_SFLOAT;
    td.usage  = rhi::TextureUsage::DepthAttachment | rhi::TextureUsage::Sampled;
    sm.depth_image = rhi.create_texture(td);
    if (!sm.depth_image.is_valid()) {
        log::error(TAG, "Failed to create shadow map image");
        return false;
    }

    // Comparison sampler for PCF shadow sampling. ClampToBorder + opaque
    // white = fragments outside the light's frustum read as "fully lit"
    // (1.0 compare-LESS ref_depth is true for any ref_depth < 1).
    {
        rhi::SamplerDesc sd{};
        sd.address_u      = rhi::AddressMode::ClampToBorder;
        sd.address_v      = rhi::AddressMode::ClampToBorder;
        sd.address_w      = rhi::AddressMode::ClampToBorder;
        sd.compare_enable = true;
        sd.compare_op     = rhi::CompareOp::Less;
        sd.border_color   = rhi::BorderColor::OpaqueWhite;
        sm.sampler = rhi.create_sampler(sd);
        if (!sm.sampler.is_valid()) {
            log::error(TAG, "Failed to create shadow map sampler");
            return false;
        }
    }

    log::info(TAG, "Shadow map created: {}x{}", sm.size, sm.size);
    return true;
}

void destroy_shadow_map(rhi::Rhi& rhi, ShadowMap& sm) {
    rhi.destroy_sampler(sm.sampler);
    rhi.destroy_texture(sm.depth_image);
    sm = {};
}

bool create_shadow_buffer(rhi::Rhi& rhi, ShadowBuffer& sb) {
    rhi::BufferDesc d{};
    d.size   = sizeof(ShadowUBO);
    d.usage  = rhi::BufferUsage::Uniform;
    d.memory = rhi::MemoryUsage::HostSequential;
    sb.buffer = rhi.create_buffer(d);
    if (!sb.buffer.is_valid()) {
        log::error(TAG, "Failed to create shadow UBO");
        return false;
    }
    return true;
}

void destroy_shadow_buffer(rhi::Rhi& rhi, ShadowBuffer& sb) {
    rhi.destroy_buffer(sb.buffer);
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
#if defined(ULDUM_BACKEND_GLES)
    // GLES NDC: y-up, z in [-1, +1]. NO (-1..+1) variant, no Y flip.
    glm::mat4 light_proj = glm::orthoRH_NO(
        -scene_radius, scene_radius,
        -scene_radius, scene_radius,
        0.1f, scene_radius * 2.5f
    );
#else
    // Vulkan: ZO (0..1) depth, Y flip.
    glm::mat4 light_proj = glm::orthoRH_ZO(
        -scene_radius, scene_radius,
        -scene_radius, scene_radius,
        0.1f, scene_radius * 2.5f
    );
    light_proj[1][1] *= -1.0f;
#endif

    return light_proj * light_view;
}

} // namespace uldum::render
