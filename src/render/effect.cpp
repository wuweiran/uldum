#include "render/effect.h"
#include "render/particles.h"
#include "render/glow_system.h"
#include "asset/asset.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace uldum::render {

static constexpr const char* TAG = "Effect";

// Engine-internal: which particle shape draws a given particle effect type.
// Authors never see this — they pick a phenomenon (Spark/Spray), the engine
// picks the shape. Swapping a row here is a no-schema visual tweak.
static u32 shape_for(EffectType t) {
    switch (t) {
        case EffectType::Spark: return ParticleSystem::SHAPE_ORB;      // gaussian orb
        case EffectType::Spray: return ParticleSystem::SHAPE_DROPLET;  // teardrop
        default:                return ParticleSystem::SHAPE_ORB;
    }
}

static EffectType parse_type(const std::string& s) {
    if (s == "spray") return EffectType::Spray;
    if (s == "glow")  return EffectType::Glow;
    if (s == "spark" || s.empty()) return EffectType::Spark;
    log::warn(TAG, "Unknown effect type '{}', defaulting to spark", s);
    return EffectType::Spark;
}

// ── EffectRegistry ────────────────────────────────────────────────────────

void EffectRegistry::register_defaults() {
    // No engine-defined effects. Maps define all effects via JSON.
}

void EffectRegistry::define(const std::string& name, const EffectDef& def) {
    m_defs[name] = def;
}

const EffectDef* EffectRegistry::get(const std::string& name) const {
    auto it = m_defs.find(name);
    return it != m_defs.end() ? &it->second : nullptr;
}

void EffectRegistry::load_from_json(const std::string& path) {
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) return;
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) return;

    nlohmann::json j;
    try { j = nlohmann::json::parse(bytes.begin(), bytes.end()); } catch (...) {
        log::warn(TAG, "Failed to parse effects file '{}'", path);
        return;
    }

    auto read_color = [](const nlohmann::json& c, f32 da) -> glm::vec4 {
        return {c.value("r", 1.0f), c.value("g", 1.0f), c.value("b", 1.0f), c.value("a", da)};
    };

    u32 count = 0;
    for (auto& [name, val] : j.items()) {
        EffectDef def;
        def.name = name;
        def.type = parse_type(val.value("type", std::string("spark")));
        // One `color` for every type. Glow uses it as the shaft/light tint;
        // particles spawn at this color and fade its alpha → 0 over their life
        // (built-in disappearance — no second color needed).
        if (val.contains("color")) def.color = read_color(val["color"], 1.0f);

        if (def.type == EffectType::Glow) {
            auto& b = def.glow;
            b.height    = val.value("height", b.height);
            b.radius    = val.value("radius", b.radius);
            b.life      = val.value("life", b.life);
            b.fade_in   = val.value("fade_in", b.fade_in);
            b.fade_out  = val.value("fade_out", b.fade_out);
            b.tyndall   = val.value("tyndall", b.tyndall);
            b.intensity = val.value("intensity", b.intensity);
        } else {
            auto& p = def.particle;
            p.count     = val.value("count", p.count);
            p.speed     = val.value("speed", p.speed);
            p.life      = val.value("life", p.life);
            p.size      = val.value("size", p.size);
            p.gravity   = val.value("gravity", p.gravity);
            p.emit_rate = val.value("emit_rate", p.emit_rate);
            p.spread    = val.value("spread", p.spread);
            p.radius    = val.value("radius", p.radius);
        }

        define(name, def);
        count++;
    }

    if (count > 0) log::info(TAG, "Loaded {} effects from '{}'", count, path);
}

// ── EffectManager ─────────────────────────────────────────────────────────

u32 EffectManager::emit(const EffectDef& def, glm::vec3 pos) {
    if (def.type == EffectType::Glow) {
        return m_glow ? m_glow->spawn(pos, def.glow, def.color) : 0;
    }
    // Particle types (Spark / Spray). Burst-only here; continuous emission is
    // handled by update() for emitters with emit_rate > 0.
    const auto& p = def.particle;
    if (p.count > 0 && p.emit_rate <= 0 && m_particles) {
        m_particles->burst(pos, p.count, def.color, p.speed, p.life,
                           p.size, p.gravity, shape_for(def.type), p.spread, p.radius);
    }
    return 0;
}



u32 EffectManager::create(const std::string& name, glm::vec3 position) {
    auto* def = m_registry ? m_registry->get(name) : nullptr;
    if (!def) { log::warn(TAG, "Unknown effect '{}'", name); return 0; }

    EffectInstance inst;
    inst.id       = ++m_next_id;
    inst.def      = def;
    inst.position = position;
    inst.duration = -1;  // permanent

    // Initial spawn (burst-only particles, or a glow). Continuous particle
    // emitters (emit_rate>0) are driven by update().
    if (def->type != EffectType::Glow && def->particle.emit_rate > 0) {
        // continuous — update() spawns; no initial burst
    } else {
        inst.glow_id = emit(*def, position);
    }

    m_instances.push_back(std::move(inst));
    return m_instances.back().id;
}

u32 EffectManager::create_on_unit(const std::string& name, simulation::Unit unit,
                                   glm::vec3 spawn_pos, std::string attach_point) {
    auto* def = m_registry ? m_registry->get(name) : nullptr;
    if (!def) { log::warn(TAG, "Unknown effect '{}'", name); return 0; }

    EffectInstance inst;
    inst.id            = ++m_next_id;
    inst.def           = def;
    inst.position      = spawn_pos;
    if (!attach_point.empty() && m_resolve) {
        // Honor the attach point for the initial burst too — without this
        // the burst spawns at the unit's feet regardless of what the
        // author asked for. Per-frame update() also runs the resolver to
        // track the bone as the unit moves. If the bone doesn't exist
        // the resolver returns zero and we fall through to spawn_pos.
        glm::vec3 resolved = m_resolve(unit, attach_point);
        if (resolved.x != 0 || resolved.y != 0 || resolved.z != 0) {
            inst.position = resolved;
        }
    }
    inst.attached_unit = unit;
    inst.attach_point  = std::move(attach_point);
    inst.duration      = -1;

    if (def->type != EffectType::Glow && def->particle.emit_rate > 0) {
        // continuous — update() spawns
    } else {
        inst.glow_id = emit(*def, inst.position);
    }

    m_instances.push_back(std::move(inst));
    return m_instances.back().id;
}

void EffectManager::destroy(u32 id) {
    // A glow's visual lifetime is owned by GlowSystem (the shaft self-expires
    // over glow.life), and while it lives the instance must keep following its
    // unit. The fire-and-forget idiom — PlayEffectOnUnit = create + immediate
    // destroy in the SAME tick — would otherwise erase the follower before it
    // ever updates, freezing the shaft at its spawn point. So for a live glow
    // instance, detach instead of erasing: it keeps tracking the unit and
    // retires itself when the glow fades (set_base → false in update()).
    for (const auto& inst : m_instances) {
        if (inst.id == id && inst.glow_id != 0 && inst.alive) return;
    }
    std::erase_if(m_instances, [id](const EffectInstance& e) { return e.id == id; });
}

void EffectManager::clear() {
    m_instances.clear();
    if (m_particles) m_particles->clear();
    if (m_glow) m_glow->clear();
}

void EffectManager::update(f32 dt, UnitPosFn get_pos, void* ctx) {
    for (auto& inst : m_instances) {
        if (!inst.alive) continue;

        if (inst.attached_unit.is_valid() && get_pos) {
            glm::vec3 pos = get_pos(inst.attached_unit, inst.attach_point, ctx);
            if (pos.x != 0 || pos.y != 0 || pos.z != 0) {
                inst.position = pos;
            }
        }

        // Glow backend: keep the live shaft anchored to the (possibly moving)
        // instance. set_base returns false once the glow has faded out and been
        // erased — that's our signal to retire the owning instance.
        if (inst.glow_id != 0 && m_glow) {
            if (!m_glow->set_base(inst.glow_id, inst.position)) {
                inst.alive = false;
                continue;
            }
        }

        // Continuous emission (particle types with emit_rate > 0 only).
        if (inst.def && inst.def->type != EffectType::Glow &&
            inst.def->particle.emit_rate > 0 && m_particles) {
            const auto& p = inst.def->particle;
            inst.emit_accumulator += p.emit_rate * dt;
            u32 to_spawn = static_cast<u32>(inst.emit_accumulator);
            if (to_spawn > 0) {
                inst.emit_accumulator -= static_cast<f32>(to_spawn);
                m_particles->burst(inst.position, to_spawn, inst.def->color,
                                   p.speed, p.life, p.size, p.gravity,
                                   shape_for(inst.def->type), p.spread, p.radius);
            }
        }

        // Duration expiry
        if (inst.duration >= 0) {
            inst.elapsed += dt;
            if (inst.elapsed >= inst.duration) {
                inst.alive = false;
            }
        }
    }

    // Remove dead instances
    std::erase_if(m_instances, [](const EffectInstance& e) { return !e.alive; });
}

} // namespace uldum::render
