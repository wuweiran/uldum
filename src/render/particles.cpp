#include "render/particles.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace uldum::render {

static constexpr const char* TAG = "Particles";

static f32 randf() { return static_cast<f32>(std::rand()) / RAND_MAX; }
static f32 randf_range(f32 lo, f32 hi) { return lo + randf() * (hi - lo); }

// Random direction on a unit sphere
static glm::vec3 random_sphere_dir() {
    f32 z = randf_range(-1, 1);
    f32 t = randf_range(0, 6.2831853f);
    f32 r = std::sqrt(1.0f - z * z);
    return {r * std::cos(t), r * std::sin(t), z};
}

// ── Init / Shutdown ───────────────────────────────────────────────────────

bool ParticleSystem::init(rhi::VulkanRhi& rhi) {
    m_rhi = &rhi;

    // Create dynamic vertex buffer (persistently mapped)
    usize buf_size = MAX_PARTICLES * 6 * sizeof(ParticleVertex);  // 6 verts per quad (2 triangles)

    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size  = buf_size;
    buf_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo info{};
    if (vmaCreateBuffer(rhi.allocator(), &buf_ci, &alloc_ci,
                        &m_vertex_buffer, &m_vertex_alloc, &info) != VK_SUCCESS) {
        log::error(TAG, "Failed to create particle vertex buffer");
        return false;
    }
    m_vertex_mapped = info.pMappedData;

    m_particles.reserve(1024);
    log::info(TAG, "Particle system initialized (max {} particles, procedural shapes)", MAX_PARTICLES);
    return true;
}

void ParticleSystem::shutdown() {
    if (m_vertex_buffer && m_rhi) {
        vmaDestroyBuffer(m_rhi->allocator(), m_vertex_buffer, m_vertex_alloc);
    }
    m_vertex_buffer = VK_NULL_HANDLE;
    m_particles.clear();
    m_emitters.clear();
    m_rhi = nullptr;
}

// ── Emitter management ───────────────────────────────────────────────────

u32 ParticleSystem::add_emitter(const ParticleEmitter& emitter) {
    ParticleEmitter e = emitter;
    e.id = ++m_next_emitter_id;
    m_emitters.push_back(e);
    return e.id;
}

void ParticleSystem::remove_emitter(u32 id) {
    std::erase_if(m_emitters, [id](const ParticleEmitter& e) { return e.id == id; });
}

void ParticleSystem::burst(glm::vec3 position, u32 count, glm::vec4 color, f32 speed, f32 life, f32 size, f32 gravity, u32 texture_id, f32 spread) {
    ParticleEmitter e;
    e.position       = position;
    e.shape          = spread >= 0.9f ? EmitterShape::Sphere : EmitterShape::Point;
    e.spread         = spread;
    e.particle_speed = speed;
    e.particle_life  = life;
    e.particle_size  = size;
    e.start_color    = color;
    e.end_color      = glm::vec4(color.r, color.g, color.b, 0);
    e.gravity        = gravity;
    e.burst_count    = count;
    e.active         = true;
    e.duration       = 0;
    e.texture_id     = texture_id;
    m_emitters.push_back(e);
}

// ── Spawn particles ──────────────────────────────────────────────────────

void ParticleSystem::spawn_from_emitter(ParticleEmitter& emitter, u32 count) {
    for (u32 i = 0; i < count && m_particles.size() < MAX_PARTICLES; ++i) {
        Particle p;
        p.position    = emitter.position;
        p.start_color = emitter.start_color;
        p.end_color   = emitter.end_color;
        p.life        = emitter.particle_life;
        p.max_life    = emitter.particle_life;
        p.size        = emitter.particle_size;
        p.gravity     = emitter.gravity;

        glm::vec3 dir;
        if (emitter.shape == EmitterShape::Sphere) {
            dir = random_sphere_dir();
        } else {
            // Point: mostly upward with some spread
            dir = glm::normalize(glm::vec3{
                randf_range(-emitter.spread, emitter.spread),
                randf_range(-emitter.spread, emitter.spread),
                1.0f
            });
        }
        p.velocity = dir * emitter.particle_speed;
        p.texture_id = emitter.texture_id;

        m_particles.push_back(p);
    }
}

// ── Update ───────────────────────────────────────────────────────────────

void ParticleSystem::update(f32 dt) {
    // Process emitters
    for (auto& emitter : m_emitters) {
        if (!emitter.active) continue;

        // Burst
        if (emitter.burst_count > 0) {
            spawn_from_emitter(emitter, emitter.burst_count);
            emitter.burst_count = 0;
            emitter.active = false;
            continue;
        }

        // Continuous emission
        if (emitter.emit_rate > 0) {
            emitter.accumulator += emitter.emit_rate * dt;
            u32 to_spawn = static_cast<u32>(emitter.accumulator);
            if (to_spawn > 0) {
                emitter.accumulator -= static_cast<f32>(to_spawn);
                spawn_from_emitter(emitter, to_spawn);
            }
        }

        // Duration
        if (emitter.duration >= 0) {
            emitter.elapsed += dt;
            if (emitter.elapsed >= emitter.duration) {
                emitter.active = false;
            }
        }
    }

    // Remove dead emitters
    std::erase_if(m_emitters, [](const ParticleEmitter& e) {
        return !e.active && e.burst_count == 0;
    });

    // Simulate particles
    for (auto& p : m_particles) {
        p.velocity.z += p.gravity * dt;
        p.position += p.velocity * dt;
        p.life -= dt;
    }

    // Remove dead particles (swap-and-pop)
    std::erase_if(m_particles, [](const Particle& p) { return p.life <= 0; });
}

// ── Billboard generation + GPU upload ────────────────────────────────────

void ParticleSystem::upload(glm::vec3 camera_right, glm::vec3 camera_up) {
    m_quad_count = 0;
    if (m_particles.empty() || !m_vertex_mapped) return;

    auto* verts = static_cast<ParticleVertex*>(m_vertex_mapped);
    u32 max_quads = std::min(static_cast<u32>(m_particles.size()), MAX_PARTICLES);

    for (u32 i = 0; i < max_quads; ++i) {
        auto& p = m_particles[i];

        // Lerp color over lifetime
        f32 t = 1.0f - (p.life / p.max_life);
        glm::vec4 color = glm::mix(p.start_color, p.end_color, t);

        // Billboard quad corners
        f32 half = p.size * 0.5f;
        glm::vec3 right = camera_right * half;
        glm::vec3 up    = camera_up * half;

        glm::vec3 bl = p.position - right - up;
        glm::vec3 br = p.position + right - up;
        glm::vec3 tr = p.position + right + up;
        glm::vec3 tl = p.position - right + up;

        // Two triangles (6 vertices)
        u32 base = i * 6;
        u32 tid = p.texture_id;
        verts[base + 0] = {bl, color, {0, 0}, tid};
        verts[base + 1] = {br, color, {1, 0}, tid};
        verts[base + 2] = {tr, color, {1, 1}, tid};
        verts[base + 3] = {bl, color, {0, 0}, tid};
        verts[base + 4] = {tr, color, {1, 1}, tid};
        verts[base + 5] = {tl, color, {0, 1}, tid};
    }

    m_quad_count = max_quads;
}

void ParticleSystem::draw(VkCommandBuffer cmd) const {
    if (m_quad_count == 0 || !m_vertex_buffer) return;

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertex_buffer, &offset);
    vkCmdDraw(cmd, m_quad_count * 6, 1, 0, 0);
}

} // namespace uldum::render
