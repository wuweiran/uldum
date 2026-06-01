#pragma once

#include "core/types.h"
#include "simulation/handle_types.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <functional>
#include <string>
#include <string_view>
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
    f32       spread      = 1.0f;   // 0 = straight up, 1 = full sphere
    f32       radius      = 0;      // >0: Ring emitter, particles arranged on horizontal circle
    glm::vec4 start_color{1, 0.8f, 0.2f, 1};
    glm::vec4 end_color{1, 0.2f, 0, 0};
    std::string texture;       // texture name: "spark", "blood", "glow", "droplet" (empty = default)
    u32       texture_id = 0;  // resolved particle texture ID (0 = default soft circle)
};

// ── Effect instance ───────────────────────────────────────────────────────
// A live effect in the world. Can be fire-and-forget or persistent.

struct EffectInstance {
    u32               id = 0;
    const EffectDef*  def = nullptr;
    glm::vec3         position{0};
    simulation::Unit  attached_unit;       // if valid, follows this unit
    std::string       attach_point;        // bone name; empty = follow unit center
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

    // Resolve texture names to particle system texture IDs
    void resolve_textures(const ParticleSystem& ps);

private:
    std::unordered_map<std::string, EffectDef> m_defs;
};

// ── Effect manager ────────────────────────────────────────────────────────
// Owns live effect instances. Updated each frame by the renderer.

class EffectManager {
public:
    void set_particles(ParticleSystem* ps) { m_particles = ps; }
    void set_registry(EffectRegistry* reg) { m_registry = reg; }

    // Resolve a (unit, attach_point) pair to a world position. Used by
    // play_on_unit / create_on_unit to spawn the initial burst at the
    // bone rather than the unit's feet — so `attach_point = "overhead"`
    // actually puts particles overhead. Returns zero vector if the
    // unit or bone can't be resolved (the manager then falls back to
    // the unit's transform position). Set once at engine init.
    using UnitPosResolver = std::function<glm::vec3(simulation::Unit unit,
                                                     std::string_view attach_point)>;
    void set_unit_pos_resolver(UnitPosResolver r) { m_resolve = std::move(r); }

    // Create a persistent effect. Returns handle for later destruction.
    u32 create(const std::string& name, glm::vec3 position);
    u32 create_on_unit(const std::string& name, simulation::Unit unit,
                       glm::vec3 spawn_pos, std::string attach_point = "");

    // Fire-and-forget: plays once then auto-destroys.
    void play(const std::string& name, glm::vec3 position);
    void play_on_unit(const std::string& name, simulation::Unit unit,
                      glm::vec3 spawn_pos, std::string attach_point = "");

    // Destroy a persistent effect by handle.
    void destroy(u32 id);

    // Drop every live effect instance and any in-flight particles.
    // Called on scene switch / session end so emitters from the
    // previous scene don't keep spawning particles into the next one.
    void clear();

    // Update all live effects (spawn particles, follow units, expire).
    using UnitPosFn = glm::vec3(*)(simulation::Unit unit,
                                    std::string_view attach_point,
                                    void* ctx);
    void update(f32 dt, UnitPosFn get_pos, void* ctx);

    // Access live instances for point light collection
    const std::vector<EffectInstance>& instances() const { return m_instances; }

private:
    ParticleSystem*  m_particles = nullptr;
    EffectRegistry*  m_registry  = nullptr;
    UnitPosResolver  m_resolve;
    std::vector<EffectInstance> m_instances;
    u32 m_next_id = 0;
};

} // namespace uldum::render
