#include "render/effect.h"
#include "render/particles.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace uldum::render {

static constexpr const char* TAG = "Effect";

// ── EffectRegistry ────────────────────────────────────────────────────────

void EffectRegistry::register_defaults() {
    define("hit_spark", {
        .name = "hit_spark", .count = 5, .speed = 80, .life = 0.3f, .size = 6, .gravity = -100,
        .start_color = {1, 0.7f, 0.2f, 1}, .end_color = {1, 0.3f, 0, 0}
    });
    define("death_burst", {
        .name = "death_burst", .count = 20, .speed = 150, .life = 0.6f, .size = 10, .gravity = -200,
        .start_color = {0.8f, 0.1f, 0.1f, 1}, .end_color = {0.3f, 0, 0, 0}
    });
    define("heal_glow", {
        .name = "heal_glow", .count = 12, .speed = 60, .life = 0.5f, .size = 8, .gravity = 50,
        .start_color = {0.2f, 1, 0.3f, 1}, .end_color = {0.1f, 0.8f, 0.2f, 0}
    });
    define("spell_cast", {
        .name = "spell_cast", .count = 15, .speed = 100, .life = 0.4f, .size = 7, .gravity = -50,
        .start_color = {0.3f, 0.5f, 1, 1}, .end_color = {0.1f, 0.2f, 0.8f, 0}
    });
    define("blood_splat", {
        .name = "blood_splat", .count = 8, .speed = 120, .life = 0.4f, .size = 5, .gravity = -300,
        .start_color = {0.6f, 0, 0, 1}, .end_color = {0.3f, 0, 0, 0}
    });
    define("aura_glow", {
        .name = "aura_glow", .count = 0, .speed = 30, .life = 0.8f, .size = 5, .gravity = 40,
        .emit_rate = 8,
        .start_color = {0.4f, 0.6f, 1, 0.6f}, .end_color = {0.2f, 0.3f, 0.8f, 0}
    });

    log::info(TAG, "Registered {} default effects", m_defs.size());
}

void EffectRegistry::define(const std::string& name, const EffectDef& def) {
    m_defs[name] = def;
}

const EffectDef* EffectRegistry::get(const std::string& name) const {
    auto it = m_defs.find(name);
    return it != m_defs.end() ? &it->second : nullptr;
}

void EffectRegistry::load_from_json(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;

    nlohmann::json j;
    try { file >> j; } catch (...) {
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
        m_particles->burst(position, def->count, def->start_color, def->speed, def->life, def->size);
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
        m_particles->burst(inst.position, def->count, def->start_color, def->speed, def->life, def->size);
    }

    m_instances.push_back(std::move(inst));
    return m_instances.back().id;
}

void EffectManager::play(const std::string& name, glm::vec3 position) {
    auto* def = m_registry ? m_registry->get(name) : nullptr;
    if (!def) { log::warn(TAG, "Unknown effect '{}'", name); return; }

    if (m_particles) {
        m_particles->burst(position, def->count, def->start_color, def->speed, def->life, def->size);
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
        m_particles->burst(pos, def->count, def->start_color, def->speed, def->life, def->size);
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
                                   inst.def->speed, inst.def->life, inst.def->size);
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
