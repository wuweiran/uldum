#include "script/script.h"
#include "simulation/simulation.h"
#include "simulation/world.h"
#include "simulation/ability_def.h"
#include "simulation/pathfinding.h"
#include "simulation/spatial_query.h"
#include "map/map.h"
#include "render/effect.h"
#include "audio/audio.h"
#include "core/log.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <glm/geometric.hpp>

#include <cmath>
#include <fstream>
#include <sstream>

namespace uldum::script {

static constexpr const char* TAG = "Script";

// ── Helpers ───────────────────────────────────────────────────────────────

static simulation::Unit make_unit(simulation::World& world, u32 id) {
    simulation::Unit u;
    u.id = id;
    auto* info = world.handle_infos.get(id);
    u.generation = info ? info->generation : 0;
    return u;
}

ScriptEngine::ScriptEngine() = default;
ScriptEngine::~ScriptEngine() = default;

// ── Init / Shutdown ───────────────────────────────────────────────────────

bool ScriptEngine::init(simulation::Simulation& sim, map::MapManager& map,
                         render::EffectRegistry* effects,
                         render::EffectManager* effect_mgr,
                         audio::AudioEngine* audio) {
    m_sim = &sim;
    m_map = &map;
    m_effects = effects;
    m_audio = audio;
    m_effect_mgr = effect_mgr;

    m_lua = std::make_unique<sol::state>();
    auto& lua = *m_lua;

    // Open safe standard libraries
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                       sol::lib::table, sol::lib::coroutine);

    // Sandbox: remove dangerous globals
    lua["os"] = sol::nil;
    lua["io"] = sol::nil;
    lua["loadfile"] = sol::nil;
    lua["dofile"] = sol::nil;
    lua["require"] = sol::nil;

    // Redirect print to engine log
    lua["print"] = [](sol::variadic_args va) {
        std::string msg;
        for (auto v : va) {
            if (!msg.empty()) msg += "\t";
            msg += v.as<std::string>();
        }
        log::info("Lua", "{}", msg);
    };

    // Register usertypes
    lua.new_usertype<simulation::Unit>("Unit",
        "id", &simulation::Unit::id,
        "is_valid", &simulation::Unit::is_valid,
        sol::meta_function::equal_to, [](const simulation::Unit& a, const simulation::Unit& b) { return a == b; }
    );
    lua.new_usertype<simulation::Player>("Player",
        "id", &simulation::Player::id,
        "is_valid", &simulation::Player::is_valid,
        sol::meta_function::equal_to, [](const simulation::Player& a, const simulation::Player& b) { return a == b; }
    );

    bind_api();
    bind_trigger_api();
    bind_timer_api();

    // Hook the world's damage callback so all damage (combat + script) fires on_damage events
    sim.world().on_damage = [this](simulation::Unit source, simulation::Unit target, f32& amount, std::string_view damage_type) {
        set_context_unit(target.id);
        set_context_damage_source(source.id);
        set_context_damage_target(target.id);
        set_context_damage_amount(amount);
        set_context_damage_type(damage_type);
        fire_event("on_damage", target.id);
        amount = get_context_damage_amount();
    };

    // Hook death callback so system_death fires on_death events
    sim.world().on_death = [this](simulation::Unit dying, simulation::Unit killer) {
        set_context_unit(dying.id);
        set_context_killer(killer.is_valid() ? killer.id : UINT32_MAX);
        fire_event("on_death", dying.id);
    };

    // Hook ability effect callback — fires on_ability_effect when a cast reaches cast_point
    sim.world().on_ability_effect = [this, &sim](simulation::Unit caster, std::string_view ability_id,
                                                  simulation::Unit target_unit, glm::vec3 target_pos) {
        set_context_unit(caster.id);
        set_context_ability(std::string(ability_id));
        set_context_spell_target_unit(target_unit.is_valid() ? target_unit.id : UINT32_MAX);
        set_context_spell_target_x(target_pos.x);
        set_context_spell_target_y(target_pos.y);
        fire_event("on_ability_effect", caster.id, ability_id);
    };

    log::info(TAG, "ScriptEngine initialized — Lua 5.4 + sol2");
    return true;
}

void ScriptEngine::shutdown() {
    m_triggers.clear();
    m_timers.clear();
    m_lua.reset();
    m_sim = nullptr;
    m_map = nullptr;
    log::info(TAG, "ScriptEngine shut down");
}

bool ScriptEngine::load_script(std::string_view path) {
    std::string path_str(path);
    std::ifstream file(path_str);
    if (!file.is_open()) {
        log::warn(TAG, "Script not found: '{}'", path);
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();

    auto result = m_lua->safe_script(ss.str(), sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        log::error(TAG, "Lua error in '{}': {}", path, err.what());
        return false;
    }

    log::info(TAG, "Loaded script '{}'", path);
    return true;
}

void ScriptEngine::call_function(std::string_view name) {
    std::string name_str(name);
    sol::function fn = (*m_lua)[name_str];
    if (!fn.valid()) {
        log::warn(TAG, "Lua function '{}' not found", name);
        return;
    }
    auto result = fn();
    if (!result.valid()) {
        sol::error err = result;
        log::error(TAG, "Error calling '{}': {}", name, err.what());
    }
}

// ── Timer tick ────────────────────────────────────────────────────────────

void ScriptEngine::update(float dt) {
    // Advance all timer clocks
    for (auto& [id, timer] : m_timers) {
        if (timer.alive) timer.remaining -= dt;
    }

    // Fire expired timers in chronological order (interleaved).
    // A 0.005s timer and a 0.005s timer alternate rather than one batch then the other.
    for (;;) {
        // Find the timer with the earliest (most negative) remaining time
        u32 best_id = 0;
        f32 best_remaining = 1.0f; // anything > 0 means "none found"
        for (auto& [id, timer] : m_timers) {
            if (!timer.alive || timer.remaining > 0) continue;
            if (timer.remaining < best_remaining || best_remaining > 0) {
                best_remaining = timer.remaining;
                best_id = id;
            }
        }
        if (best_remaining > 0) break; // no more expired timers

        auto it = m_timers.find(best_id);
        if (it == m_timers.end()) break;
        auto& timer = it->second;

        if (timer.callback) {
            timer.callback();
        }
        // Re-check alive — callback may have destroyed this timer
        if (timer.repeating && timer.alive) {
            timer.remaining += timer.interval;
        } else {
            timer.alive = false;
        }
    }

    // Clean up dead timers
    std::vector<u32> expired;
    for (auto& [id, timer] : m_timers) {
        if (!timer.alive) expired.push_back(id);
    }
    for (u32 id : expired) {
        m_timers.erase(id);
    }
}

// ── Event firing ──────────────────────────────────────────────────────────

void ScriptEngine::fire_event(std::string_view event_name, u32 unit_id,
                               std::string_view ability_id, u32 player_id) {
    m_ctx_event = std::string(event_name);

    auto matches = [&](const Trigger& trig) -> bool {
        for (auto& eb : trig.events) {
            if (eb.event_name != event_name) continue;
            if (eb.unit_id != UINT32_MAX && eb.unit_id != unit_id) continue;
            if (!eb.ability_id.empty() && eb.ability_id != ability_id) continue;
            if (eb.player_id != UINT32_MAX && eb.player_id != player_id) continue;
            return true;
        }
        return false;
    };

    // Iterate from highest priority to lowest — no sorting needed
    for (i32 p = static_cast<i32>(TriggerPriority::Count) - 1; p >= 0; --p) {
        auto prio = static_cast<TriggerPriority>(p);
        for (auto& [trig_id, trig] : m_triggers) {
            if (!trig.alive || trig.priority != prio) continue;
            if (!matches(trig)) continue;

            bool pass = true;
            for (auto& cond : trig.conditions) {
                if (!cond()) { pass = false; break; }
            }
            if (!pass) continue;

            for (auto& action : trig.actions) {
                action();
            }
        }
    }
}

// ── API Bindings ──────────────────────────────────────────────────────────

void ScriptEngine::bind_api() {
    auto& lua = *m_lua;
    auto& sim = *m_sim;

    // Helper: return nil for invalid units
    auto unit_or_nil = [&](simulation::Unit u) -> sol::object {
        return u.is_valid() ? sol::make_object(*m_lua, u) : sol::make_object(*m_lua, sol::nil);
    };

    auto player_or_nil = [&](simulation::Player p) -> sol::object {
        return p.is_valid() ? sol::make_object(*m_lua, p) : sol::make_object(*m_lua, sol::nil);
    };

    // ── Unit API ──────────────────────────────────────────────────────

    lua["CreateUnit"] = [&](const std::string& type_id, simulation::Player owner, f32 x, f32 y, sol::optional<f32> facing) -> sol::object {
        auto& world = sim.world();
        auto unit = simulation::create_unit(world, type_id, owner, x, y, facing.value_or(0));
        if (unit.is_valid()) {
            auto* t = world.transforms.get(unit.id);
            if (t) t->position.z = sim.pathfinder().sample_height(x, y);
            auto* mov = world.movements.get(unit.id);
            if (mov) mov->cliff_level = sim.pathfinder().cliff_level_at(x, y);
            set_context_unit(unit.id);
            fire_event("on_unit_created", unit.id);
            return sol::make_object(*m_lua, unit);
        }
        return sol::make_object(*m_lua, sol::nil);
    };

    lua["RemoveUnit"] = [&](simulation::Unit unit) {
        if (!sim.world().validate(unit)) return;
        set_context_unit(unit.id);
        fire_event("on_unit_removed", unit.id);
        simulation::destroy(sim.world(), unit);
    };

    lua["IsUnitAlive"] = [&](simulation::Unit unit) -> bool {
        return simulation::is_alive(sim.world(), unit);
    };

    lua["IsUnitDead"] = [&](simulation::Unit unit) -> bool {
        return simulation::is_dead(sim.world(), unit);
    };

    lua["IsUnitHero"] = [&](simulation::Unit unit) -> bool {
        return simulation::is_hero(sim.world(), unit);
    };

    lua["IsUnitBuilding"] = [&](simulation::Unit unit) -> bool {
        return simulation::is_building(sim.world(), unit);
    };

    lua["GetUnitTypeId"] = [&](simulation::Unit unit) -> std::string {
        auto* info = sim.world().handle_infos.get(unit.id);
        return info ? info->type_id : "";
    };

    // Position
    lua["GetUnitX"] = [&](simulation::Unit u) -> f32 {
        auto* t = sim.world().transforms.get(u.id); return t ? t->position.x : 0;
    };
    lua["GetUnitY"] = [&](simulation::Unit u) -> f32 {
        auto* t = sim.world().transforms.get(u.id); return t ? t->position.y : 0;
    };
    lua["GetUnitZ"] = [&](simulation::Unit u) -> f32 {
        auto* t = sim.world().transforms.get(u.id); return t ? t->position.z : 0;
    };
    lua["SetUnitPosition"] = [&](simulation::Unit u, f32 x, f32 y) {
        auto* t = sim.world().transforms.get(u.id);
        if (t) { t->position.x = x; t->position.y = y; t->position.z = sim.pathfinder().sample_height(x, y); }
    };
    lua["GetUnitFacing"] = [&](simulation::Unit u) -> f32 {
        auto* t = sim.world().transforms.get(u.id);
        return t ? t->facing * 57.2957795f : 0;  // rad → deg
    };
    lua["SetUnitFacing"] = [&](simulation::Unit u, f32 degrees) {
        auto* t = sim.world().transforms.get(u.id);
        if (t) t->facing = degrees * 0.0174532925f;  // deg → rad
    };

    // Health
    lua["GetUnitHealth"] = [&](simulation::Unit u) -> f32 {
        return simulation::get_health(sim.world(), u);
    };
    lua["GetUnitMaxHealth"] = [&](simulation::Unit u) -> f32 {
        auto* h = sim.world().healths.get(u.id); return h ? h->max : 0;
    };
    lua["SetUnitHealth"] = [&](simulation::Unit u, f32 hp) {
        simulation::set_health(sim.world(), u, hp);
    };

    // Attributes
    lua["GetUnitAttribute"] = [&](simulation::Unit u, const std::string& name) -> f32 {
        auto* ab = sim.world().attribute_blocks.get(u.id);
        if (!ab) return 0;
        auto it = ab->numeric.find(name);
        return it != ab->numeric.end() ? it->second : 0;
    };

    lua["SetUnitAttribute"] = [&](simulation::Unit u, const std::string& name, f32 value) {
        auto* ab = sim.world().attribute_blocks.get(u.id);
        if (ab) ab->numeric[name] = value;
    };

    lua["GetUnitStringAttribute"] = [&](simulation::Unit u, const std::string& name) -> std::string {
        auto* ab = sim.world().attribute_blocks.get(u.id);
        if (!ab) return "";
        auto it = ab->string_attrs.find(name);
        return it != ab->string_attrs.end() ? it->second : "";
    };

    // Classification
    lua["HasClassification"] = [&](simulation::Unit u, const std::string& flag) -> bool {
        auto* cls = sim.world().classifications.get(u.id);
        return cls && simulation::has_classification(cls->flags, flag);
    };

    // Owner
    lua["GetUnitOwner"] = [&, player_or_nil](simulation::Unit u) -> sol::object {
        auto* o = sim.world().owners.get(u.id);
        return o ? player_or_nil(o->player) : sol::make_object(*m_lua, sol::nil);
    };

    // ── Order API ─────────────────────────────────────────────────────

    lua["IssueOrder"] = [&](simulation::Unit unit, const std::string& order_type, sol::variadic_args va) {
        auto& world = sim.world();
        if (!world.validate(unit)) return;

        simulation::Order order;
        if (order_type == "move" && va.size() >= 2) {
            order.payload = simulation::orders::Move{glm::vec3{va[0].as<f32>(), va[1].as<f32>(), 0}};
        } else if (order_type == "attack" && va.size() >= 1) {
            // attack(unit) = attack target, attack(x, y) = attack-move to point
            if (va[0].is<simulation::Unit>()) {
                order.payload = simulation::orders::Attack{va[0].as<simulation::Unit>()};
            } else if (va[0].is<f32>() && va.size() >= 2) {
                order.payload = simulation::orders::AttackMove{glm::vec3{va[0].as<f32>(), va[1].as<f32>(), 0}};
            } else {
                return;
            }
        } else if (order_type == "stop") {
            order.payload = simulation::orders::Stop{};
        } else if (order_type == "hold") {
            order.payload = simulation::orders::HoldPosition{};
        } else if (order_type == "cast" && va.size() >= 1) {
            simulation::orders::Cast cast;
            cast.ability_id = va[0].as<std::string>();
            if (va.size() >= 2) {
                // Second arg: target unit or target point (x, y)
                if (va[1].is<simulation::Unit>()) {
                    cast.target_unit = va[1].as<simulation::Unit>();
                } else if (va[1].is<f32>() && va.size() >= 3) {
                    cast.target_pos = glm::vec3{va[1].as<f32>(), va[2].as<f32>(), 0};
                }
            }
            order.payload = std::move(cast);
        } else {
            return;
        }
        simulation::issue_order(world, unit, std::move(order));
    };

    // ── Ability API ───────────────────────────────────────────────────

    lua["AddAbility"] = [&](simulation::Unit unit, const std::string& ability_id, sol::optional<u32> level) -> bool {
        bool ok = simulation::add_ability(sim.world(), sim.abilities(), unit, ability_id, level.value_or(1));
        if (ok) { set_context_unit(unit.id); set_context_ability(ability_id); fire_event("on_ability_added", unit.id, ability_id); }
        return ok;
    };

    lua["RemoveAbility"] = [&](simulation::Unit unit, const std::string& ability_id) -> bool {
        bool ok = simulation::remove_ability(sim.world(), unit, ability_id);
        if (ok) { set_context_unit(unit.id); set_context_ability(ability_id); fire_event("on_ability_removed", unit.id, ability_id); }
        return ok;
    };

    lua["ApplyPassiveAbility"] = [&](simulation::Unit target, const std::string& ability_id, simulation::Unit source, f32 duration) -> bool {
        bool ok = simulation::apply_passive_ability(sim.world(), sim.abilities(), target, ability_id, source, duration);
        if (ok) { set_context_unit(target.id); set_context_ability(ability_id); fire_event("on_ability_added", target.id, ability_id); }
        return ok;
    };

    lua["HasAbility"] = [&](simulation::Unit u, const std::string& ability_id) -> bool {
        return simulation::has_ability(sim.world(), u, ability_id);
    };

    lua["GetAbilityLevel"] = [&](simulation::Unit u, const std::string& ability_id) -> u32 {
        return simulation::get_ability_level(sim.world(), u, ability_id);
    };

    lua["GetAbilityStackCount"] = [&](simulation::Unit u, const std::string& ability_id) -> u32 {
        return simulation::get_ability_stack_count(sim.world(), u, ability_id);
    };

    // ── Damage API ────────────────────────────────────────────────────

    lua["DamageUnit"] = [&](simulation::Unit source, simulation::Unit target, f32 amount, sol::optional<std::string> damage_type) {
        simulation::deal_damage(sim.world(), source, target, amount, damage_type.value_or("spell"));
    };

    lua["HealUnit"] = [&](simulation::Unit source, simulation::Unit target, f32 amount) {
        auto* hp = sim.world().healths.get(target.id);
        if (!hp) return;
        hp->current = std::min(hp->current + amount, hp->max);
    };

    lua["KillUnit"] = [&](simulation::Unit unit, sol::optional<simulation::Unit> killer) {
        auto* hp = sim.world().healths.get(unit.id);
        if (hp) hp->current = 0;
        set_context_unit(unit.id);
        set_context_killer(killer ? killer->id : UINT32_MAX);
        fire_event("on_death", unit.id);
    };

    // ── Spatial Query API ─────────────────────────────────────────────

    lua["GetUnitsInRange"] = [&](f32 x, f32 y, f32 radius, sol::optional<sol::table> filter_table) -> sol::table {
        simulation::UnitFilter filter;
        if (filter_table) {
            auto& ft = *filter_table;
            if (ft["owner"].valid()) filter.owner = ft["owner"].get<simulation::Player>();
            if (ft["enemy_of"].valid()) filter.enemy_of = ft["enemy_of"].get<simulation::Player>();
            if (ft["alive_only"].valid()) filter.alive_only = ft["alive_only"].get<bool>();
        }
        auto units = sim.spatial_grid().units_in_range(sim.world(), {x, y, 0}, radius, filter);
        sol::table result = m_lua->create_table();
        for (size_t i = 0; i < units.size(); ++i) {
            result[i + 1] = units[i];
        }
        return result;
    };

    // ── Hero API ──────────────────────────────────────────────────────

    lua["GetHeroLevel"] = [&](simulation::Unit u) -> u32 {
        return simulation::hero_get_level(sim.world(), u);
    };
    lua["AddHeroXP"] = [&](simulation::Unit u, u32 xp) {
        simulation::hero_add_xp(sim.world(), u, xp);
    };

    // ── Player API ────────────────────────────────────────────────────

    lua["GetPlayer"] = [](u32 slot) -> simulation::Player { return simulation::Player{slot}; };

    // ── Alliance API ─────────────────────────────────────────────────

    lua["SetAlliance"] = [&](simulation::Player a, simulation::Player b, bool allied, sol::optional<bool> passive) {
        sim.set_alliance(a, b, allied, passive.value_or(false));
    };

    lua["IsPlayerAlly"] = [&](simulation::Player a, simulation::Player b) -> bool {
        return sim.is_allied(a, b);
    };

    lua["IsPlayerEnemy"] = [&](simulation::Player a, simulation::Player b) -> bool {
        return sim.is_enemy(a, b);
    };

    // ── Utility API ───────────────────────────────────────────────────

    lua["Log"] = [](const std::string& msg) { log::info("Lua", "{}", msg); };

    lua["GetGameTime"] = [&]() -> f32 { return 0; };

    lua["GetDistanceBetween"] = [&](simulation::Unit u1, simulation::Unit u2) -> f32 {
        auto* t1 = sim.world().transforms.get(u1.id);
        auto* t2 = sim.world().transforms.get(u2.id);
        if (!t1 || !t2) return 0;
        auto d = t1->position - t2->position; d.z = 0;
        return glm::length(d);
    };

    lua["RandomInt"] = [](i32 min, i32 max) -> i32 { return min + (std::rand() % (max - min + 1)); };
    lua["RandomFloat"] = [](f32 min, f32 max) -> f32 { return min + static_cast<f32>(std::rand()) / RAND_MAX * (max - min); };

    // ── Effect API ─────────────────────────────────────────────────────

    // Helper: read color from Lua table (supports {r=, g=, b=, a=} or {1, 2, 3, 4})
    auto read_color = [](sol::table c, glm::vec4 fallback) -> glm::vec4 {
        if (c["r"].valid()) {
            return {c.get_or("r", fallback.r), c.get_or("g", fallback.g),
                    c.get_or("b", fallback.b), c.get_or("a", fallback.a)};
        }
        return {c.get_or(1, fallback.r), c.get_or(2, fallback.g),
                c.get_or(3, fallback.b), c.get_or(4, fallback.a)};
    };

    lua["DefineEffect"] = [&, read_color](const std::string& name, sol::table def) {
        if (!m_effects) return;
        render::EffectDef edef;
        edef.name      = name;
        edef.count     = def.get_or("count", 10u);
        edef.speed     = def.get_or("speed", 100.0f);
        edef.life      = def.get_or("life", 0.5f);
        edef.size      = def.get_or("size", 8.0f);
        edef.gravity   = def.get_or("gravity", -200.0f);
        edef.emit_rate = def.get_or("emit_rate", 0.0f);
        if (def["start_color"].valid()) {
            edef.start_color = read_color(def["start_color"], {1, 1, 1, 1});
        }
        if (def["end_color"].valid()) {
            edef.end_color = read_color(def["end_color"], {1, 1, 1, 0});
        }
        m_effects->define(name, edef);
    };

    lua["CreateEffect"] = [&](const std::string& name, f32 x, f32 y, f32 z) -> u32 {
        if (!m_effect_mgr) return 0;
        return m_effect_mgr->create(name, {x, y, z});
    };

    lua["CreateEffectOnUnit"] = [this, &sim](const std::string& name, simulation::Unit unit,
                                              sol::optional<std::string> attach_point) -> u32 {
        if (!m_effect_mgr) return 0;
        auto* t = sim.world().transforms.get(unit.id);
        if (!t) return 0;
        glm::vec3 pos = t->position;
        if (attach_point && m_attach_fn) {
            pos += m_attach_fn(unit.id, *attach_point) * t->scale;
        }
        return m_effect_mgr->create_on_unit(name, unit, pos);
    };

    lua["DestroyEffect"] = [&](u32 id) {
        if (m_effect_mgr) m_effect_mgr->destroy(id);
    };

    lua["PlayEffect"] = [&](const std::string& name, f32 x, f32 y, f32 z) {
        if (m_effect_mgr) m_effect_mgr->play(name, {x, y, z});
    };

    lua["PlayEffectOnUnit"] = [this, &sim](const std::string& name, simulation::Unit unit,
                                            sol::optional<std::string> attach_point) {
        if (!m_effect_mgr) return;
        auto* t = sim.world().transforms.get(unit.id);
        if (!t) return;
        glm::vec3 pos = t->position;
        if (attach_point && m_attach_fn) {
            pos += m_attach_fn(unit.id, *attach_point) * t->scale;
        }
        m_effect_mgr->play_on_unit(name, unit, pos);
    };
    // ── Audio API ──────────────────────────────────────────────────────

    lua["PlaySound"] = [this](std::string_view path, simulation::Unit unit) {
        if (!m_audio) return;
        auto& sim = *m_sim;
        auto* t = sim.world().transforms.get(unit.id);
        if (t) m_audio->play_sfx(path, t->position);
    };

    lua["PlaySoundAtPoint"] = [this](std::string_view path, f32 x, f32 y) {
        if (!m_audio) return;
        m_audio->play_sfx(path, {x, y, 0});
    };

    lua["PlaySound2D"] = [this](std::string_view path) {
        if (!m_audio) return;
        m_audio->play_sfx_2d(path);
    };

    lua["PlayMusic"] = [this](std::string_view path, sol::optional<f32> fade_in) {
        if (!m_audio) return;
        m_audio->play_music(path, fade_in.value_or(1.0f));
    };

    lua["StopMusic"] = [this](sol::optional<f32> fade_out) {
        if (!m_audio) return;
        m_audio->stop_music(fade_out.value_or(1.0f));
    };

    lua["PlayAmbientLoop"] = [this](std::string_view path, f32 x, f32 y) -> u32 {
        if (!m_audio) return 0;
        return m_audio->play_ambient(path, {x, y, 0}).id;
    };

    lua["StopAmbientLoop"] = [this](u32 handle_id, sol::optional<f32> fade_out) {
        if (!m_audio) return;
        m_audio->stop_ambient({handle_id}, fade_out.value_or(0.5f));
    };

    lua["SetVolume"] = [this](std::string_view channel, f32 volume) {
        if (!m_audio) return;
        audio::Channel ch = audio::Channel::Master;
        if (channel == "sfx")     ch = audio::Channel::SFX;
        else if (channel == "music")   ch = audio::Channel::Music;
        else if (channel == "ambient") ch = audio::Channel::Ambient;
        else if (channel == "voice")   ch = audio::Channel::Voice;
        m_audio->set_volume(ch, volume);
    };
}

// ── Trigger API Bindings ──────────────────────────────────────────────────

void ScriptEngine::bind_trigger_api() {
    auto& lua = *m_lua;

    // Expose priority constants
    lua["TRIGGER_PRIORITY_LOW"]    = static_cast<i32>(TriggerPriority::Low);
    lua["TRIGGER_PRIORITY_NORMAL"] = static_cast<i32>(TriggerPriority::Normal);
    lua["TRIGGER_PRIORITY_HIGH"]   = static_cast<i32>(TriggerPriority::High);

    lua["CreateTrigger"] = [&](sol::optional<i32> priority) -> sol::table {
        u32 id = next_trigger_id();
        Trigger trig;
        trig.id = id;
        i32 p = priority.value_or(static_cast<i32>(TriggerPriority::Normal));
        p = std::clamp(p, 0, static_cast<i32>(TriggerPriority::Count) - 1);
        trig.priority = static_cast<TriggerPriority>(p);
        m_triggers[id] = std::move(trig);

        sol::table t = lua.create_table();
        t["_id"] = id;
        t["data"] = lua.create_table();
        return t;
    };

    lua["DestroyTrigger"] = [&](sol::table t) {
        u32 id = t["_id"].get<u32>();
        auto it = m_triggers.find(id);
        if (it != m_triggers.end()) {
            // Destroy owned timers
            for (u32 timer_id : it->second.owned_timers) {
                m_timers.erase(timer_id);
            }
            m_triggers.erase(it);
        }
    };

    lua["TriggerRegisterEvent"] = [&](sol::table t, const std::string& event_name) {
        u32 id = t["_id"].get<u32>();
        auto it = m_triggers.find(id);
        if (it != m_triggers.end()) {
            it->second.events.push_back({event_name});
        }
    };

    lua["TriggerRegisterUnitEvent"] = [&](sol::table t, simulation::Unit unit, const std::string& event_name) {
        u32 id = t["_id"].get<u32>();
        auto it = m_triggers.find(id);
        if (it != m_triggers.end()) {
            Trigger::EventBinding eb;
            eb.event_name = event_name;
            eb.unit_id = unit.id;
            it->second.events.push_back(std::move(eb));
        }
    };

    lua["TriggerRegisterUnitAbilityEvent"] = [&](sol::table t, simulation::Unit unit, const std::string& ability_id, const std::string& event_name) {
        u32 id = t["_id"].get<u32>();
        auto it = m_triggers.find(id);
        if (it != m_triggers.end()) {
            Trigger::EventBinding eb;
            eb.event_name = event_name;
            eb.unit_id = unit.id;
            eb.ability_id = ability_id;
            it->second.events.push_back(std::move(eb));
        }
    };

    lua["TriggerRegisterPlayerEvent"] = [&](sol::table t, simulation::Player player, const std::string& event_name) {
        u32 id = t["_id"].get<u32>();
        auto it = m_triggers.find(id);
        if (it != m_triggers.end()) {
            Trigger::EventBinding eb;
            eb.event_name = event_name;
            eb.player_id = player.id;
            it->second.events.push_back(std::move(eb));
        }
    };

    lua["TriggerRegisterTimerEvent"] = [&](sol::table t, f32 interval, bool repeating) {
        u32 trig_id = t["_id"].get<u32>();
        auto trig_it = m_triggers.find(trig_id);
        if (trig_it == m_triggers.end()) return;

        u32 timer_id = next_timer_id();
        Timer timer;
        timer.id = timer_id;
        timer.interval = interval;
        timer.remaining = interval;
        timer.repeating = repeating;
        timer.callback = [this, trig_id]() {
            auto it = m_triggers.find(trig_id);
            if (it == m_triggers.end() || !it->second.alive) return;
            m_ctx_event = "timer";
            bool pass = true;
            for (auto& cond : it->second.conditions) {
                if (!cond()) { pass = false; break; }
            }
            if (pass) {
                for (auto& action : it->second.actions) {
                    action();
                }
            }
        };
        m_timers[timer_id] = std::move(timer);
        trig_it->second.owned_timers.push_back(timer_id);

        // Also register "timer" as an event for matching
        trig_it->second.events.push_back({"timer"});
    };

    lua["TriggerAddCondition"] = [&](sol::table t, sol::function fn) {
        u32 id = t["_id"].get<u32>();
        auto it = m_triggers.find(id);
        if (it != m_triggers.end()) {
            it->second.conditions.push_back([fn]() -> bool {
                auto result = fn();
                return result.valid() && result.get<bool>();
            });
        }
    };

    lua["TriggerAddAction"] = [&](sol::table t, sol::function fn) {
        u32 id = t["_id"].get<u32>();
        auto it = m_triggers.find(id);
        if (it != m_triggers.end()) {
            it->second.actions.push_back([fn]() {
                auto result = fn();
                if (!result.valid()) {
                    sol::error err = result;
                    log::error("Lua", "Trigger action error: {}", err.what());
                }
            });
        }
    };


    // ── Context functions ─────────────────────────────────────────────

    auto& sim = *m_sim;

    auto unit_or_nil = [&](simulation::Unit u) -> sol::object {
        return u.is_valid() ? sol::make_object(*m_lua, u) : sol::make_object(*m_lua, sol::nil);
    };

    auto player_or_nil = [&](simulation::Player p) -> sol::object {
        return p.is_valid() ? sol::make_object(*m_lua, p) : sol::make_object(*m_lua, sol::nil);
    };

    // Context validation helper
    auto warn_wrong_event = [&](const char* fn, std::initializer_list<const char*> expected) {
        for (auto* e : expected) {
            if (m_ctx_event == e) return false;
        }
        log::warn(TAG, "{}() called during '{}' event — expected one of: {}", fn, m_ctx_event,
                  [&]{ std::string s; for (auto* e : expected) { if (!s.empty()) s += ", "; s += e; } return s; }());
        return true;
    };

    lua["GetTriggerEvent"] = [&]() -> std::string { return m_ctx_event; };
    lua["GetTriggerUnit"] = [&, unit_or_nil]() -> sol::object {
        return unit_or_nil(make_unit(sim.world(), m_ctx_unit));
    };
    lua["GetTriggerPlayer"] = [&, player_or_nil]() -> sol::object {
        return player_or_nil(simulation::Player{m_ctx_player});
    };
    lua["GetTriggerAbilityId"] = [&]() -> std::string {
        if (m_ctx_ability.empty()) {
            log::warn(TAG, "GetTriggerAbilityId() called but no ability in context (event: '{}')", m_ctx_event);
        }
        return m_ctx_ability;
    };
    lua["GetDamageSource"] = [&, unit_or_nil, warn_wrong_event]() -> sol::object {
        warn_wrong_event("GetDamageSource", {"on_damage"});
        return unit_or_nil(make_unit(sim.world(), m_ctx_damage_source));
    };
    lua["GetDamageTarget"] = [&, unit_or_nil, warn_wrong_event]() -> sol::object {
        warn_wrong_event("GetDamageTarget", {"on_damage"});
        return unit_or_nil(make_unit(sim.world(), m_ctx_damage_target));
    };
    lua["GetDamageAmount"] = [&, warn_wrong_event]() -> f32 {
        warn_wrong_event("GetDamageAmount", {"on_damage"});
        return m_ctx_damage_amount;
    };
    lua["SetDamageAmount"] = [&, warn_wrong_event](f32 v) {
        if (warn_wrong_event("SetDamageAmount", {"on_damage"})) return;
        m_ctx_damage_amount = v;
    };
    lua["GetDamageType"] = [&, warn_wrong_event]() -> std::string {
        warn_wrong_event("GetDamageType", {"on_damage"});
        return m_ctx_damage_type;
    };
    lua["GetKillingUnit"] = [&, unit_or_nil, warn_wrong_event]() -> sol::object {
        warn_wrong_event("GetKillingUnit", {"on_death"});
        return unit_or_nil(make_unit(sim.world(), m_ctx_killer));
    };
    lua["GetSpellTargetUnit"] = [&, unit_or_nil, warn_wrong_event]() -> sol::object {
        warn_wrong_event("GetSpellTargetUnit", {"on_ability_effect", "on_ability_cast"});
        return unit_or_nil(make_unit(sim.world(), m_ctx_spell_target_unit));
    };

    // ── RegisterEvent shorthand ───────────────────────────────────────

    lua["RegisterEvent"] = [&](const std::string& event_name, sol::function fn) -> u32 {
        u32 id = next_trigger_id();
        Trigger trig;
        trig.id = id;
        trig.events.push_back({event_name});
        trig.actions.push_back([fn]() {
            auto result = fn();
            if (!result.valid()) {
                sol::error err = result;
                log::error("Lua", "Event handler error: {}", err.what());
            }
        });
        m_triggers[id] = std::move(trig);
        return id;
    };

    lua["UnregisterEvent"] = [&](u32 handle) {
        m_triggers.erase(handle);
    };
}

// ── Timer API Bindings ────────────────────────────────────────────────────

void ScriptEngine::bind_timer_api() {
    auto& lua = *m_lua;

    lua["CreateTimer"] = [&](f32 delay, bool repeating, sol::function callback) -> u32 {
        u32 id = next_timer_id();
        Timer timer;
        timer.id = id;
        timer.interval = delay;
        timer.remaining = delay;
        timer.repeating = repeating;
        timer.callback = [callback]() {
            auto result = callback();
            if (!result.valid()) {
                sol::error err = result;
                log::error("Lua", "Timer callback error: {}", err.what());
            }
        };
        m_timers[id] = std::move(timer);
        return id;
    };

    lua["DestroyTimer"] = [&](u32 id) {
        m_timers.erase(id);
    };
}

} // namespace uldum::script
