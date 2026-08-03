#pragma once

#include "core/types.h"
#include "simulation/entity_types.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace uldum::render {

class ParticleSystem;
class GlowSystem;

// ── Effect definition ─────────────────────────────────────────────────────
// A named, parameterized visual preset. `type` names an orthogonal visual
// *phenomenon* the author picks — NOT a particle shape. The engine maps each
// type to a rendering backend internally; particle shape (when the backend is
// particles) is a hidden detail, decoupled from this vocabulary.
//
//   • Spark — energetic bright bits (impacts, magic, soft auras). Particle.
//   • Spray — liquid arc (blood OR water — color decides). Particle.
//   • Fire  — sustained flame: additive orb plume that lerps hot→cool over
//             life AND casts a flickering point light. Particle + light.
//   • Glow  — engine-owned light visual (volumetric Tyndall shafts today;
//             "hero glow" and other light effects in future). Glow backend.
//
// Engine, not authors, decides the particle shape per kind (see effect.cpp
// shape_for()). One effect = exactly one kind; no composition.

enum class EffectKind : u8 { Spark, Spray, Fire, Glow };

// Particle-backend params (Spark / Spray).
struct ParticleParams {
    u32 count     = 10;
    f32 speed     = 100.0f;
    f32 life      = 0.5f;
    f32 size      = 8.0f;
    f32 gravity   = -200.0f;
    f32 emit_rate = 0.0f;    // >0: continuous emission (particles/sec)
    f32 spread    = 1.0f;    // 0 = straight up, 1 = full sphere
    f32 radius    = 0.0f;    // >0: Ring emitter
};

// Glow-backend params. Engine-owned light visual — no texture knob; the look
// is procedural. A single STATIC vertical Tyndall light shaft that fades in
// and back out over `life` (like a door opening then closing in the dark) —
// no motion. Emits a point light (reach + brightness derived from `radius` +
// `intensity`).
struct GlowParams {
    f32 height    = 220.0f;  // shaft height (world units)
    f32 radius    = 24.0f;   // beam width (world units) — also scales light reach
    f32 life      = 1.2f;    // total fade-in→hold→fade-out duration (seconds)
    f32 fade_in   = 0.0f;    // seconds to ramp 0→1 (0 = half of life)
    f32 fade_out  = 0.0f;    // seconds to ramp 1→0 (0 = half of life)
    f32 tyndall   = 0.6f;    // volumetric striation strength (0 = clean shaft)
    f32 intensity = 1.0f;    // brightness of the beam and the light it casts
};

// Point light cast by a Fire effect. Sits above the emitter and flickers.
// Reach/brightness are independent of the particle plume so a small flame can
// still throw a wide warm pool. Radius 0 disables the light (pure visual fire).
struct FireLightParams {
    f32 radius    = 190.0f;  // light reach (world units)
    f32 intensity = 1.4f;    // base brightness
    f32 height    = 30.0f;   // lift above the emitter anchor (world units)
    f32 flicker   = 0.3f;    // brightness wobble amplitude (0 = steady)
};

struct EffectDef {
    std::string    name;
    EffectKind     kind = EffectKind::Spark;
    glm::vec4      color{1, 0.8f, 0.2f, 1};   // tint; particles fade its alpha→0 over life
    glm::vec4      color_end{1, 0.2f, 0.05f, 1}; // end-of-life tint if has_color_end
    bool           has_color_end = false;      // lerp color→color_end over particle life
    ParticleParams particle{};   // meaningful for Spark / Spray / Fire
    GlowParams     glow{};       // meaningful for Glow
    FireLightParams light{};     // meaningful for Fire
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

    // Glow backend: stable handle of this instance's live shaft (0 = none).
    // A unit-attached glow follows the unit by pushing inst.position into the
    // glow each frame (set_base); the instance retires when the glow fades.
    u32               glow_id = 0;

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

// One point light emitted by a live Fire effect this frame. Rebuilt every
// update(); the Renderer drains it right after the glow lights (same pattern),
// so fire lights ride the engine's normal point-light path.
struct FireLight {
    glm::vec3 position{0};
    glm::vec3 color{1};
    f32       radius    = 0;
    f32       intensity = 0;
};

class EffectManager {
public:
    void set_particles(ParticleSystem* ps) { m_particles = ps; }
    void set_glow(GlowSystem* gs) { m_glow = gs; }
    void set_registry(EffectRegistry* reg) { m_registry = reg; }

    // Resolve a (unit, attach_point) pair to a world position. Used by
    // create_on_unit to spawn the initial burst at the bone rather than
    // the unit's feet — so `attach_point = "overhead"` actually puts
    // particles overhead. Returns zero vector if the unit or bone can't
    // be resolved (the manager then falls back to the unit's transform
    // position). Set once at engine init.
    using UnitPosResolver = std::function<glm::vec3(simulation::Unit unit,
                                                     std::string_view attach_point)>;
    void set_unit_pos_resolver(UnitPosResolver r) { m_resolve = std::move(r); }

    // Create a persistent effect. Returns handle for later destruction.
    u32 create(const std::string& name, glm::vec3 position);
    u32 create_on_unit(const std::string& name, simulation::Unit unit,
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

    // Point lights from live Fire effects this frame (rebuilt each update()).
    // The Renderer drains this the same way it does GlowSystem::glow_data().
    std::span<const FireLight> fire_lights() const { return m_fire_lights; }

private:
    // Spawn an effect's initial visual at `pos`: particle burst (Spark/Spray)
    // or a glow (Glow). The one place that maps EffectKind → backend. Returns
    // the glow handle for Glow effects (so a unit-attached glow can be tracked
    // and moved each frame); 0 for particles.
    u32 emit(const EffectDef& def, glm::vec3 pos);

    ParticleSystem*  m_particles = nullptr;
    GlowSystem*      m_glow      = nullptr;
    EffectRegistry*  m_registry  = nullptr;
    UnitPosResolver  m_resolve;
    std::vector<EffectInstance> m_instances;
    std::vector<FireLight>      m_fire_lights;   // rebuilt each update()
    f32                         m_time = 0;      // accumulated seconds (flicker phase)
    u32 m_next_id = 0;
};

} // namespace uldum::render
