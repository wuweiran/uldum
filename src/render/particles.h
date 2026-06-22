#pragma once

#include "core/types.h"
#include "rhi/handles.h"
#include "rhi/command_list.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

#include <vector>

namespace uldum::rhi { class Rhi; }

namespace uldum::render {

// GPU vertex for particle billboard quads
struct ParticleVertex {
    glm::vec3 position;
    glm::vec4 color;
    glm::vec2 texcoord;
    u32       texture_id = 0;  // procedural shape id (SHAPE_*), not a texture index
};

// Single particle state (CPU)
struct Particle {
    glm::vec3 position{0};
    glm::vec3 velocity{0};
    glm::vec4 color{1};      // tint; alpha fades to 0 over life (built-in disappearance)
    f32 life     = 0;
    f32 max_life = 1;
    f32 size     = 8;
    f32 gravity  = -200.0f;
    u32 texture_id = 0;
};

// Emitter shapes
enum class EmitterShape : u8 { Point, Sphere, Ring };

// A particle emitter — a one-shot burst. `burst()` pushes one of these per
// call; update() spawns its particles on the next frame, then drops it.
// Continuous emission is driven at the effect layer (EffectManager::update
// calls burst() repeatedly), not here.
struct ParticleEmitter {
    glm::vec3    position{0};
    EmitterShape shape        = EmitterShape::Point;
    f32          particle_life  = 1.0f;
    f32          particle_speed = 100.0f;
    f32          particle_size  = 8.0f;
    glm::vec4    color{1.0f, 0.8f, 0.2f, 1.0f};   // tint; alpha fades to 0 over life
    f32          spread       = 1.0f;      // 0 = up only, 1 = full sphere
    f32          gravity      = -200.0f;   // Z acceleration

    bool         active       = true;
    u32          burst_count  = 0;         // particles to spawn, then deactivate

    u32          texture_id   = 0;         // procedural shape id (SHAPE_*)
    f32          radius       = 0;         // Ring shape: radius of the circle
};

// The particle system — manages emitters, particles, and GPU buffer
class ParticleSystem {
public:
    static constexpr u32 MAX_PARTICLES = 8192;

    bool init(rhi::Rhi& rhi);
    void shutdown();

    // Spawn a one-shot burst at a position. If `radius > 0`, the emitter
    // uses Ring shape; otherwise shape is chosen from `spread`.
    void burst(glm::vec3 position, u32 count, glm::vec4 color, f32 speed = 150, f32 life = 0.5f, f32 size = 10, f32 gravity = -200.0f, u32 texture_id = 0, f32 spread = 1.0f, f32 radius = 0);

    // Update particles (spawn, simulate, kill dead). Call once per frame.
    void update(f32 dt);

    // Generate billboard quads and upload to GPU. Call after update.
    // camera_right and camera_up are needed for billboarding.
    void upload(glm::vec3 camera_right, glm::vec3 camera_up);

    // Draw all particles. Caller must have bound the particle pipeline.
    void draw(rhi::CommandList& cmd) const;

    // Drop every live particle and emitter. Used on scene switch /
    // session end so previous-scene particles don't bleed into the
    // next scene.
    void clear() { m_particles.clear(); m_emitters.clear(); m_quad_count = 0; }

    // Procedural shape IDs (computed in fragment shader, no textures needed).
    // The engine owns the mapping from effect type → shape (see effect.cpp
    // shape_for); these are the two sprites that mapping actually uses.
    static constexpr u32 SHAPE_ORB     = 0;  // soft gaussian orb (default) — sparks
    static constexpr u32 SHAPE_DROPLET = 1;  // teardrop — spray / water

private:
    void spawn_from_emitter(ParticleEmitter& emitter, u32 count);

    std::vector<Particle>         m_particles;
    std::vector<ParticleEmitter>  m_emitters;

    // GPU vertex buffer (dynamic, updated every frame)
    rhi::BufferHandle m_vertex_buffer{};
    u32               m_quad_count = 0;

    rhi::Rhi* m_rhi = nullptr;
};

} // namespace uldum::render
