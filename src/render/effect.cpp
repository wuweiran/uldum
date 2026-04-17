#include "render/effect.h"
#include "render/particles.h"
#include "asset/asset.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace uldum::render {

static constexpr const char* TAG = "Effect";

// ── EffectRegistry ────────────────────────────────────────────────────────

void EffectRegistry::register_defaults() {
    // No engine-defined effects. Maps define all effects via Lua DefineEffect() or JSON.
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

    u32 count = 0;
    for (auto& [name, val] : j.items()) {
        EffectDef def;
        def.name      = name;
        def.count     = val.value("count", 10u);
        def.speed     = val.value("speed", 100.0f);
        def.life      = val.value("life", 0.5f);
        def.size      = val.value("size", 8.0f);
        def.gravity   = val.value("gravity", -200.0f);
        def.emit_rate = val.value("emit_rate", 0.0f);
        def.spread    = val.value("spread", 1.0f);

        def.texture = val.value("texture", "");
        if (val.contains("start_color")) {
            auto& c = val["start_color"];
            def.start_color = {c.value("r", 1.0f), c.value("g", 1.0f), c.value("b", 1.0f), c.value("a", 1.0f)};
        }
        if (val.contains("end_color")) {
            auto& c = val["end_color"];
            def.end_color = {c.value("r", 1.0f), c.value("g", 1.0f), c.value("b", 1.0f), c.value("a", 0.0f)};
        }

        define(name, def);
        count++;
    }

    if (count > 0) log::info(TAG, "Loaded {} effects from '{}'", count, path);
}

void EffectRegistry::resolve_textures(const ParticleSystem&) {
    for (auto& [name, def] : m_defs) {
        if (def.texture == "spark")        def.texture_id = ParticleSystem::SHAPE_SPARK;
        else if (def.texture == "blood")   def.texture_id = ParticleSystem::SHAPE_BLOOD;
        else if (def.texture == "glow")    def.texture_id = ParticleSystem::SHAPE_GLOW;
        else if (def.texture == "droplet") def.texture_id = ParticleSystem::SHAPE_DROPLET;
        else                               def.texture_id = ParticleSystem::SHAPE_DEFAULT;
    }
}

// ── EffectManager ─────────────────────────────────────────────────────────

u32 EffectManager::create(const std::string& name, glm::vec3 position) {
    auto* def = m_registry ? m_registry->get(name) : nullptr;
    if (!def) { log::warn(TAG, "Unknown effect '{}'", name); return 0; }

    EffectInstance inst;
    inst.id       = ++m_next_id;
    inst.def      = def;
    inst.position = position;
    inst.duration = -1;  // permanent

    // If burst-only, spawn initial burst
    if (def->count > 0 && def->emit_rate <= 0 && m_particles) {
        m_particles->burst(position, def->count, def->start_color, def->speed, def->life, def->size, def->gravity, def->texture_id, def->spread);
    }

    m_instances.push_back(std::move(inst));
    return m_instances.back().id;
}

u32 EffectManager::create_on_unit(const std::string& name, simulation::Unit unit, glm::vec3 unit_pos) {
    auto* def = m_registry ? m_registry->get(name) : nullptr;
    if (!def) { log::warn(TAG, "Unknown effect '{}'", name); return 0; }

    EffectInstance inst;
    inst.id            = ++m_next_id;
    inst.def           = def;
    inst.position      = unit_pos;
    inst.position.z   += 32.0f;
    inst.attached_unit = unit;
    inst.duration      = -1;

    if (def->count > 0 && def->emit_rate <= 0 && m_particles) {
        m_particles->burst(inst.position, def->count, def->start_color, def->speed, def->life, def->size, def->gravity, def->texture_id, def->spread);
    }

    m_instances.push_back(std::move(inst));
    return m_instances.back().id;
}

void EffectManager::play(const std::string& name, glm::vec3 position) {
    auto* def = m_registry ? m_registry->get(name) : nullptr;
    if (!def) { log::warn(TAG, "Unknown effect '{}'", name); return; }

    if (m_particles) {
        m_particles->burst(position, def->count, def->start_color, def->speed, def->life, def->size, def->gravity, def->texture_id, def->spread);
    }
    // No instance needed for fire-and-forget with no continuous emission
    if (def->emit_rate > 0) {
        EffectInstance inst;
        inst.id       = ++m_next_id;
        inst.def      = def;
        inst.position = position;
        inst.duration = def->life;
        m_instances.push_back(std::move(inst));
    }
}

void EffectManager::play_on_unit(const std::string& name, simulation::Unit unit, glm::vec3 unit_pos) {
    auto* def = m_registry ? m_registry->get(name) : nullptr;
    if (!def) { log::warn(TAG, "Unknown effect '{}'", name); return; }

    glm::vec3 pos = unit_pos;
    pos.z += 32.0f;
    if (m_particles) {
        m_particles->burst(pos, def->count, def->start_color, def->speed, def->life, def->size, def->gravity, def->texture_id, def->spread);
    }
    if (def->emit_rate > 0) {
        EffectInstance inst;
        inst.id            = ++m_next_id;
        inst.def           = def;
        inst.position      = pos;
        inst.attached_unit = unit;
        inst.duration      = def->life;
        m_instances.push_back(std::move(inst));
    }
}

void EffectManager::destroy(u32 id) {
    std::erase_if(m_instances, [id](const EffectInstance& e) { return e.id == id; });
}

void EffectManager::update(f32 dt, UnitPosFn get_pos, void* ctx) {
    for (auto& inst : m_instances) {
        if (!inst.alive) continue;

        // Follow attached unit
        if (inst.attached_unit.is_valid() && get_pos) {
            glm::vec3 pos = get_pos(inst.attached_unit, ctx);
            if (pos.x != 0 || pos.y != 0 || pos.z != 0) {
                inst.position = pos;
                inst.position.z += 32.0f;
            }
        }

        // Continuous emission
        if (inst.def && inst.def->emit_rate > 0 && m_particles) {
            inst.emit_accumulator += inst.def->emit_rate * dt;
            u32 to_spawn = static_cast<u32>(inst.emit_accumulator);
            if (to_spawn > 0) {
                inst.emit_accumulator -= static_cast<f32>(to_spawn);
                m_particles->burst(inst.position, to_spawn, inst.def->start_color,
                                   inst.def->speed, inst.def->life, inst.def->size, inst.def->gravity);
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
