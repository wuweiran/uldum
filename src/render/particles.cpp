#include "render/particles.h"
#include "rhi/rhi.h"
#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace uldum::render {

static constexpr const char* TAG = "Particles";

static f32 randf() { return static_cast<f32>(std::rand()) / static_cast<f32>(RAND_MAX); }
static f32 randf_range(f32 lo, f32 hi) { return lo + randf() * (hi - lo); }

// Random direction on a unit sphere
static glm::vec3 random_sphere_dir() {
    f32 z = randf_range(-1, 1);
    f32 t = randf_range(0, 6.2831853f);
    f32 r = std::sqrt(1.0f - z * z);
    return {r * std::cos(t), r * std::sin(t), z};
}

// ── Init / Shutdown ───────────────────────────────────────────────────────

bool ParticleSystem::init(rhi::Rhi& rhi) {
    m_rhi = &rhi;

    // Create dynamic vertex buffer (persistently mapped)
    {
        rhi::BufferDesc d{};
        d.size   = MAX_PARTICLES * 6 * sizeof(ParticleVertex);  // 6 verts per quad
        d.usage  = rhi::BufferUsage::Vertex;
        d.memory = rhi::MemoryUsage::HostSequential;
        m_vertex_buffer = rhi.create_buffer(d);
        if (!m_vertex_buffer.is_valid()) {
            log::error(TAG, "Failed to create particle vertex buffer");
            return false;
        }
    }

    m_particles.reserve(1024);
    log::info(TAG, "Particle system initialized (max {} particles, procedural shapes)", MAX_PARTICLES);
    return true;
}

void ParticleSystem::shutdown() {
    if (m_rhi) m_rhi->destroy_buffer(m_vertex_buffer);
    m_vertex_buffer = {};
    m_particles.clear();
    m_emitters.clear();
    m_rhi = nullptr;
}

// ── Burst ────────────────────────────────────────────────────────────────

void ParticleSystem::burst(glm::vec3 position, u32 count, glm::vec4 color, f32 speed, f32 life, f32 size, f32 gravity, u32 texture_id, f32 spread, f32 radius) {
    ParticleEmitter e;
    e.position       = position;
    if (radius > 0)              e.shape = EmitterShape::Ring;
    else if (spread >= 0.9f)     e.shape = EmitterShape::Sphere;
    else                         e.shape = EmitterShape::Point;
    e.spread         = spread;
    e.radius         = radius;
    e.particle_speed = speed;
    e.particle_life  = life;
    e.particle_size  = size;
    e.color          = color;
    e.gravity        = gravity;
    e.burst_count    = count;
    e.active         = true;
    e.texture_id     = texture_id;
    m_emitters.push_back(e);
}

// ── Spawn particles ──────────────────────────────────────────────────────

void ParticleSystem::spawn_from_emitter(ParticleEmitter& emitter, u32 count) {
    for (u32 i = 0; i < count && m_particles.size() < MAX_PARTICLES; ++i) {
        Particle p;
        p.position    = emitter.position;
        p.color       = emitter.color;
        p.life        = emitter.particle_life;
        p.max_life    = emitter.particle_life;
        p.size        = emitter.particle_size;
        p.gravity     = emitter.gravity;

        glm::vec3 dir;
        if (emitter.shape == EmitterShape::Sphere) {
            dir = random_sphere_dir();
        } else if (emitter.shape == EmitterShape::Ring) {
            // Evenly distribute around a horizontal circle, all
            // particles share the same upward velocity so the ring
            // rises as a coherent unit.
            f32 theta = (count > 0)
                ? (static_cast<f32>(i) / static_cast<f32>(count)) * 6.28318530718f
                : 0.0f;
            p.position += glm::vec3{std::cos(theta) * emitter.radius,
                                    std::sin(theta) * emitter.radius,
                                    0};
            dir = glm::vec3{0, 0, 1};
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
    // Each emitter is a one-shot burst: spawn its particles, then retire it.
    for (auto& emitter : m_emitters) {
        if (!emitter.active) continue;
        spawn_from_emitter(emitter, emitter.burst_count);
        emitter.active = false;
    }
    std::erase_if(m_emitters, [](const ParticleEmitter& e) { return !e.active; });

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
    if (m_particles.empty()) return;
    auto* verts = static_cast<ParticleVertex*>(m_rhi->mapped_ptr(m_vertex_buffer));
    if (!verts) return;
    u32 max_quads = std::min(static_cast<u32>(m_particles.size()), MAX_PARTICLES);

    for (u32 i = 0; i < max_quads; ++i) {
        auto& p = m_particles[i];

        // Built-in disappearance: hold the tint, fade its alpha linearly to 0
        // over the particle's life (life/max_life goes 1→0).
        glm::vec4 color = p.color;
        color.a *= (p.max_life > 0) ? (p.life / p.max_life) : 0.0f;

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

void ParticleSystem::draw(rhi::CommandList& cmd) const {
    if (m_quad_count == 0 || !m_vertex_buffer.is_valid()) return;
    cmd.bind_vertex_buffer(0, m_vertex_buffer);
    cmd.draw(m_quad_count * 6);
}

} // namespace uldum::render
