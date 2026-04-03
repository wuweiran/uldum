#pragma once

#include "core/types.h"
#include "simulation/handle_types.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace uldum::render {

class ParticleSystem;

// ── Effect definition ─────────────────────────────────────────────────────
// Describes what an effect looks like. Engine provides defaults, maps can
// add or override. Currently only particle-based; future: model, sprite, etc.

struct EffectDef {
    std::string name;

    // Particle settings (the only renderer type for now)
    u32       count       = 10;
    f32       speed       = 100;
    f32       life        = 0.5f;
    f32       size        = 8;
    f32       gravity     = -200;
    f32       emit_rate   = 0;       // >0: continuous emission (particles/sec), 0: burst only
    glm::vec4 start_color{1, 0.8f, 0.2f, 1};
    glm::vec4 end_color{1, 0.2f, 0, 0};
};

// ── Effect instance ───────────────────────────────────────────────────────
// A live effect in the world. Can be fire-and-forget or persistent.

struct EffectInstance {
    u32               id = 0;
    const EffectDef*  def = nullptr;
    glm::vec3         position{0};
    simulation::Unit  attached_unit;       // if valid, follows this unit
    f32               elapsed    = 0;
    f32               duration   = -1;     // -1 = permanent until destroyed, >0 = auto-destroy
    bool              alive      = true;

    // For continuous emitters
    f32               emit_accumulator = 0;
};

// ── Effect registry ───────────────────────────────────────────────────────

class EffectRegistry {
public:
    void register_defaults();
    void define(const std::string& name, const EffectDef& def);
    const EffectDef* get(const std::string& name) const;
    void load_from_json(const std::string& path);

private:
    std::unordered_map<std::string, EffectDef> m_defs;
};

// ── Effect manager ────────────────────────────────────────────────────────
// Owns live effect instances. Updated each frame by the renderer.

class EffectManager {
public:
    void set_particles(ParticleSystem* ps) { m_particles = ps; }
    void set_registry(EffectRegistry* reg) { m_registry = reg; }

    // Create a persistent effect. Returns handle for later destruction.
    u32 create(const std::string& name, glm::vec3 position);
    u32 create_on_unit(const std::string& name, simulation::Unit unit, glm::vec3 unit_pos);

    // Fire-and-forget: plays once then auto-destroys.
    void play(const std::string& name, glm::vec3 position);
    void play_on_unit(const std::string& name, simulation::Unit unit, glm::vec3 unit_pos);

    // Destroy a persistent effect by handle.
    void destroy(u32 id);

    // Update all live effects (spawn particles, follow units, expire).
    // unit_pos_fn: callback to get current unit position (avoids depending on World).
    using UnitPosFn = glm::vec3(*)(simulation::Unit, void* ctx);
    void update(f32 dt, UnitPosFn get_pos, void* ctx);

private:
    ParticleSystem*  m_particles = nullptr;
    EffectRegistry*  m_registry  = nullptr;
    std::vector<EffectInstance> m_instances;
    u32 m_next_id = 0;
};

} // namespace uldum::render
