#pragma once

#include "core/types.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

#include <vector>

// Forward declare Vulkan types to avoid pulling vulkan.h into script module
struct VkBuffer_T; typedef VkBuffer_T* VkBuffer;
struct VkCommandBuffer_T; typedef VkCommandBuffer_T* VkCommandBuffer;
struct VmaAllocation_T; typedef VmaAllocation_T* VmaAllocation;

namespace uldum::rhi { class VulkanRhi; }

namespace uldum::render {

// GPU vertex for particle billboard quads
struct ParticleVertex {
    glm::vec3 position;
    glm::vec4 color;
    glm::vec2 texcoord;
};

// Single particle state (CPU)
struct Particle {
    glm::vec3 position{0};
    glm::vec3 velocity{0};
    glm::vec4 start_color{1};
    glm::vec4 end_color{1, 1, 1, 0};
    f32 life     = 0;
    f32 max_life = 1;
    f32 size     = 8;
};

// Emitter shapes
enum class EmitterShape : u8 { Point, Sphere };

// A particle emitter — spawns particles over time or as a burst
struct ParticleEmitter {
    glm::vec3    position{0};
    EmitterShape shape        = EmitterShape::Point;
    f32          emit_rate    = 0;         // particles per second (0 = burst only)
    f32          accumulator  = 0;
    f32          particle_life  = 1.0f;
    f32          particle_speed = 100.0f;
    f32          particle_size  = 8.0f;
    glm::vec4    start_color{1.0f, 0.8f, 0.2f, 1.0f};
    glm::vec4    end_color{1.0f, 0.2f, 0.0f, 0.0f};
    f32          spread       = 1.0f;      // 0 = up only, 1 = full sphere
    f32          gravity      = -200.0f;   // Z acceleration

    bool         active       = true;
    f32          duration     = -1;        // -1 = infinite, >0 = auto-destroy
    f32          elapsed      = 0;
    u32          burst_count  = 0;         // >0: spawn this many immediately, then deactivate

    u32          id           = 0;         // for external tracking
};

// The particle system — manages emitters, particles, and GPU buffer
class ParticleSystem {
public:
    static constexpr u32 MAX_PARTICLES = 8192;

    bool init(rhi::VulkanRhi& rhi);
    void shutdown();

    // Add an emitter. Returns its id for later removal.
    u32 add_emitter(const ParticleEmitter& emitter);
    void remove_emitter(u32 id);

    // Spawn a one-shot burst at a position
    void burst(glm::vec3 position, u32 count, glm::vec4 color, f32 speed = 150, f32 life = 0.5f, f32 size = 10);

    // Update particles (spawn, simulate, kill dead). Call once per frame.
    void update(f32 dt);

    // Generate billboard quads and upload to GPU. Call after update.
    // camera_right and camera_up are needed for billboarding.
    void upload(glm::vec3 camera_right, glm::vec3 camera_up);

    // Draw all particles. Caller must have bound the particle pipeline.
    void draw(VkCommandBuffer cmd) const;

    u32 particle_count() const { return static_cast<u32>(m_particles.size()); }
    u32 quad_count()     const { return m_quad_count; }

private:
    void spawn_from_emitter(ParticleEmitter& emitter, u32 count);

    std::vector<Particle>         m_particles;
    std::vector<ParticleEmitter>  m_emitters;
    u32 m_next_emitter_id = 0;

    // GPU vertex buffer (dynamic, updated every frame)
    VkBuffer      m_vertex_buffer = nullptr;
    VmaAllocation m_vertex_alloc  = nullptr;
    void*         m_vertex_mapped = nullptr;
    u32           m_quad_count    = 0;

    rhi::VulkanRhi* m_rhi = nullptr;
};

} // namespace uldum::render
