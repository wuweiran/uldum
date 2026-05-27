#pragma once

#include "core/types.h"
#include "rhi/handles.h"

#include <glm/mat4x4.hpp>

namespace uldum::rhi { class Rhi; }

namespace uldum::render {

static constexpr u32 SHADOW_MAP_SIZE = 2048;

// Shadow map resources: depth texture rendered from the light's perspective.
struct ShadowMap {
    rhi::TextureHandle depth_image{};
    rhi::SamplerHandle sampler{};  // comparison sampler for PCF
    u32                size = SHADOW_MAP_SIZE;
};

// Uniform buffer for shadow data passed to main-pass shaders.
struct ShadowUBO {
    glm::mat4 light_vp;  // light view-projection matrix
};

struct ShadowBuffer {
    rhi::BufferHandle buffer{};
    // Persistent map lives inside the RHI's record; query via
    // `rhi.mapped_ptr(buffer)` when writing.
};

bool create_shadow_map(rhi::Rhi& rhi, ShadowMap& sm);
void destroy_shadow_map(rhi::Rhi& rhi, ShadowMap& sm);

bool create_shadow_buffer(rhi::Rhi& rhi, ShadowBuffer& sb);
void destroy_shadow_buffer(rhi::Rhi& rhi, ShadowBuffer& sb);

// Compute the light view-projection matrix for a directional light.
// Fits an orthographic frustum around the given scene bounds.
glm::mat4 compute_light_vp(const glm::vec3& light_dir, const glm::vec3& scene_center, f32 scene_radius);

} // namespace uldum::render
