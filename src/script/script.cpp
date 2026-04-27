#include "script/script.h"
#include "simulation/simulation.h"
#include "simulation/world.h"
#include "simulation/ability_def.h"
#include "simulation/pathfinding.h"
#include "simulation/spatial_query.h"
#include "input/selection.h"
#include "input/command_system.h"
#include "input/command.h"
#include "map/map.h"
#include "asset/asset.h"
#include "render/effect.h"
#include "render/renderer.h"
#include "audio/audio.h"
#include "network/protocol.h"
#include "hud/hud.h"
#include "hud/node.h"
#include "hud/text_tag.h"
#include "core/log.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include <glm/geometric.hpp>

#include <cmath>
#include <filesystem>
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
                         audio::AudioEngine* audio,
                         render::Renderer* renderer) {
    m_sim = &sim;
    m_map = &map;
    m_effects = effects;
    m_audio = audio;
    m_effect_mgr = effect_mgr;
    m_renderer = renderer;

    m_lua = std::make_unique<sol::state>();
    auto& lua = *m_lua;


    // Open safe standard libraries (including package for require())
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                       sol::lib::table, sol::lib::coroutine, sol::lib::package);

    // Sandbox: remove dangerous globals but keep require()
    lua["os"] = sol::nil;
    lua["io"] = sol::nil;
    lua["loadfile"] = sol::nil;
    lua["dofile"] = sol::nil;

    // Disable native C module loading (security)
    lua["package"]["cpath"] = "";
    // Default empty package.path — set_script_paths() configures it before scripts run
    lua["package"]["path"] = "";

    // Replace the default file-based searcher with one that reads from mounted packages.
    // Slot 2 of package.searchers is Lua's io-based file searcher; we substitute ours
    // so require() resolves through the AssetManager instead of the filesystem.
    lua["package"]["searchers"][2] = [this](const std::string& modname) -> sol::object {
        auto& L = *m_lua;
        auto* mgr = asset::AssetManager::instance();
        if (!mgr) return sol::make_object(L, std::string("\n\tno AssetManager"));

        std::string modpath = modname;
        std::replace(modpath.begin(), modpath.end(), '.', '/');

        std::string path_str = L["package"]["path"].get_or<std::string>("");
        std::string tried;

        size_t start = 0;
        while (start <= path_str.size()) {
            size_t end = path_str.find(';', start);
            if (end == std::string::npos) end = path_str.size();
            std::string pattern = path_str.substr(start, end - start);
            start = end + 1;
            if (pattern.empty()) continue;

            size_t q = pattern.find('?');
            if (q == std::string::npos) continue;
            std::string candidate = pattern.substr(0, q) + modpath + pattern.substr(q + 1);

            auto bytes = mgr->read_file_bytes(candidate);
            if (bytes.empty()) {
                tried += "\n\tno package entry '" + candidate + "'";
                continue;
            }

            std::string chunk(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            sol::load_result lr = L.load(chunk, "@" + candidate, sol::load_mode::text);
            if (!lr.valid()) {
                sol::error err = lr;
                return sol::make_object(L, std::format("\n\tload error in '{}': {}", candidate, err.what()));
            }
            return sol::make_object(L, sol::function(lr));
        }
        return sol::make_object(L, tried);
    };

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
    bind_input_api();
    bind_save_api();
    bind_hud_api();

    // Hook the world's damage callback so all damage (combat + script) fires on_damage events
    sim.world().on_damage = [this](simulation::Unit source, simulation::Unit target, f32& amount, std::string_view damage_type) {
        set_context_unit(target.id);
        set_context_damage_source(source.id);
        set_context_damage_target(target.id);
        set_context_damage_amount(amount);
        set_context_damage_type(damage_type);
        fire_event("global_damage", target.id);
        fire_event("unit_damage", target.id);
        amount = get_context_damage_amount();
    };

    // Hook death callback so system_death fires on_death events
    sim.world().on_death = [this](simulation::Unit dying, simulation::Unit killer) {
        set_context_unit(dying.id);
        set_context_killer(killer.is_valid() ? killer.id : UINT32_MAX);
        fire_event("global_death", dying.id);
        fire_event("unit_death", dying.id);
    };

    // Hook ability effect callback — fires on_ability_effect when a cast reaches cast_point
    sim.world().on_ability_effect = [this, &sim](simulation::Unit caster, std::string_view ability_id,
                                                  simulation::Unit target_unit, glm::vec3 target_pos) {
        set_context_unit(caster.id);
        set_context_ability(std::string(ability_id));
        set_context_spell_target_unit(target_unit.is_valid() ? target_unit.id : UINT32_MAX);
        set_context_spell_target_x(target_pos.x);
        set_context_spell_target_y(target_pos.y);
        fire_event("global_ability_effect", caster.id, ability_id);
        fire_event("unit_ability_effect", caster.id, ability_id);
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
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) {
        log::warn(TAG, "Script load without AssetManager: '{}'", path);
        return false;
    }
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) {
        log::warn(TAG, "Script not found: '{}'", path);
        return false;
    }
    std::string script_src(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    auto result = m_lua->safe_script(script_src, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        log::error(TAG, "Lua error in '{}': {}", path, err.what());
        return false;
    }

    log::info(TAG, "Loaded script '{}'", path);
    return true;
}

bool ScriptEngine::call_function(std::string_view name) {
    std::string name_str(name);
    sol::function fn = (*m_lua)[name_str];
    if (!fn.valid()) {
        log::warn(TAG, "Lua function '{}' not found", name);
        return false;
    }
    auto result = fn();
    if (!result.valid()) {
        sol::error err = result;
        log::error(TAG, "Error calling '{}': {}", name, err.what());
        return false;
    }
    return true;
}

// ── Script paths (require) ────────────────────────────────────────────────

void ScriptEngine::set_script_paths(std::string_view scene_scripts,
                                     std::string_view shared_scripts,
                                     std::string_view engine_scripts) {
    // Build Lua package.path: scene → shared → engine
    // Lua searches these in order when require() is called.
    std::string path;
    auto append = [&](std::string_view dir) {
        if (dir.empty()) return;
        if (!path.empty()) path += ";";
        path += dir;
        path += "/?.lua";
    };
    append(scene_scripts);
    append(shared_scripts);
    append(engine_scripts);

    (*m_lua)["package"]["path"] = path;
    log::info(TAG, "Script package.path: {}", path);
}

// ── Save data persistence ────────────────────────────────────────────────

void ScriptEngine::set_save_path(std::string_view save_dir) {
    m_save_path = std::string(save_dir) + "/save_data.json";

    // Create directory if needed. Android crashes here if the CWD is the
    // read-only system partition (current fallback). Catch the error and
    // disable save persistence for this session rather than terminating —
    // Android saves need the app's internal data path from GameActivity,
    // which isn't yet plumbed through game_server.cpp.
    std::filesystem::path dir(save_dir);
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            log::warn(TAG, "Save directory '{}' not writable ({}); save/load disabled",
                      save_dir, ec.message());
            m_save_path.clear();  // downstream save/load ops check for empty
            return;
        }
    }

    // Load existing save data
    std::ifstream file(m_save_path);
    if (file.is_open()) {
        try {
            m_save_data = nlohmann::json::parse(file);
            log::info(TAG, "Loaded save data from '{}'", m_save_path);
        } catch (const nlohmann::json::exception& e) {
            log::warn(TAG, "Failed to parse save data: {}", e.what());
            m_save_data = nlohmann::json::object();
        }
    } else {
        m_save_data = nlohmann::json::object();
    }
}

void ScriptEngine::flush_save_data() {
    if (!m_save_dirty || m_save_path.empty()) return;

    std::ofstream file(m_save_path);
    if (file.is_open()) {
        file << m_save_data.dump(2);
        m_save_dirty = false;
        log::trace(TAG, "Save data flushed to '{}'", m_save_path);
    } else {
        log::error(TAG, "Failed to write save data to '{}'", m_save_path);
    }
}

void ScriptEngine::bind_save_api() {
    auto& lua = *m_lua;

    lua["SaveData"] = [this](const std::string& key, sol::object value) {
        if (m_save_path.empty()) {
            log::warn(TAG, "SaveData called but no save path configured");
            return;
        }
        if (value.is<bool>()) {
            m_save_data[key] = value.as<bool>();
        } else if (value.is<i64>()) {
            m_save_data[key] = value.as<i64>();
        } else if (value.is<f64>()) {
            m_save_data[key] = value.as<f64>();
        } else if (value.is<std::string>()) {
            m_save_data[key] = value.as<std::string>();
        } else {
            log::warn(TAG, "SaveData: unsupported type for key '{}'", key);
            return;
        }
        m_save_dirty = true;
        flush_save_data();
    };

    lua["LoadData"] = [this](const std::string& key, sol::optional<sol::object> default_val) -> sol::object {
        auto it = m_save_data.find(key);
        if (it == m_save_data.end()) {
            return default_val.has_value() ? default_val.value() : sol::make_object(*m_lua, sol::nil);
        }
        auto& v = *it;
        if (v.is_boolean())        return sol::make_object(*m_lua, v.get<bool>());
        if (v.is_number_integer()) return sol::make_object(*m_lua, v.get<i64>());
        if (v.is_number_float())   return sol::make_object(*m_lua, v.get<f64>());
        if (v.is_string())         return sol::make_object(*m_lua, v.get<std::string>());
        return default_val.has_value() ? default_val.value() : sol::make_object(*m_lua, sol::nil);
    };

    lua["ClearSaveData"] = [this]() {
        m_save_data = nlohmann::json::object();
        m_save_dirty = true;
        flush_save_data();
        log::info(TAG, "Save data cleared");
    };
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

void ScriptEngine::fire_node_event(std::string_view event_name, u32 player_id,
                                    std::string_view node_id) {
    set_context_node_id(std::string(node_id));
    set_context_player(player_id);
    fire_event(event_name, UINT32_MAX, "", player_id);
    m_ctx_node_id.clear();
}

void ScriptEngine::fire_event(std::string_view event_name, u32 unit_id,
                               std::string_view /*ability_id*/, u32 player_id) {
    m_ctx_event = std::string(event_name);

    auto matches = [&](const Trigger& trig) -> bool {
        for (auto& eb : trig.events) {
            if (eb.event_name != event_name) continue;
            if (eb.unit_id != UINT32_MAX && eb.unit_id != unit_id) continue;
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
    const auto& terrain_ref = m_map->terrain();

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
            if (t) t->position.z = ::uldum::map::sample_height(terrain_ref,x, y);
            auto* mov = world.movements.get(unit.id);
            if (mov) mov->cliff_level = sim.pathfinder().cliff_level_at(x, y);
            set_context_unit(unit.id);
            fire_event("global_unit_created", unit.id);
            return sol::make_object(*m_lua, unit);
        }
        return sol::make_object(*m_lua, sol::nil);
    };

    lua["RemoveUnit"] = [&](simulation::Unit unit) {
        if (!sim.world().validate(unit)) return;
        set_context_unit(unit.id);
        fire_event("global_unit_removed", unit.id);
        simulation::destroy(sim.world(), unit);
    };

    lua["IsUnitAlive"] = [&](simulation::Unit unit) -> bool {
        return simulation::is_alive(sim.world(), unit);
    };

    lua["IsUnitDead"] = [&](simulation::Unit unit) -> bool {
        return simulation::is_dead(sim.world(), unit);
    };

    lua["IsUnitHero"] = [&](simulation::Unit unit) -> bool {
        auto& world = sim.world();
        if (!world.validate(unit)) return false;
        auto* cls = world.classifications.get(unit.id);
        return cls && simulation::has_classification(cls->flags, "hero");
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
        if (t) { t->position.x = x; t->position.y = y; t->position.z = ::uldum::map::sample_height(terrain_ref,x, y); }
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
        if (ab) {
            ab->numeric[name] = value;
            if (m_unit_update_fn) {
                auto pkt = network::build_update_attr(u.id, name, value);
                m_unit_update_fn(u.id, pkt);
            }
        }
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
        u32 lvl = level.value_or(1);
        bool ok = simulation::add_ability(sim.world(), sim.abilities(), unit, ability_id, lvl);
        if (ok) {
            set_context_unit(unit.id); set_context_ability(ability_id);
            fire_event("global_ability_added", unit.id, ability_id);
            fire_event("unit_ability_added", unit.id, ability_id);
            if (m_unit_update_fn) {
                auto pkt = network::build_update_ability_add(unit.id, ability_id, lvl);
                m_unit_update_fn(unit.id, pkt);
            }
        }
        return ok;
    };

    lua["RemoveAbility"] = [&](simulation::Unit unit, const std::string& ability_id) -> bool {
        bool ok = simulation::remove_ability(sim.world(), unit, ability_id);
        if (ok) {
            set_context_unit(unit.id); set_context_ability(ability_id);
            fire_event("global_ability_removed", unit.id, ability_id);
            fire_event("unit_ability_removed", unit.id, ability_id);
            if (m_unit_update_fn) {
                auto pkt = network::build_update_ability_remove(unit.id, ability_id);
                m_unit_update_fn(unit.id, pkt);
            }
        }
        return ok;
    };

    lua["ApplyPassiveAbility"] = [&](simulation::Unit target, const std::string& ability_id, simulation::Unit source, f32 duration) -> bool {
        bool ok = simulation::apply_passive_ability(sim.world(), sim.abilities(), target, ability_id, source, duration);
        if (ok) {
            set_context_unit(target.id); set_context_ability(ability_id);
            fire_event("global_ability_added", target.id, ability_id);
            fire_event("unit_ability_added", target.id, ability_id);
        }
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
        if (amount <= 0) return;
        simulation::deal_damage(sim.world(), source, target, amount, damage_type.value_or("spell"));
    };

    lua["HealUnit"] = [&](simulation::Unit source, simulation::Unit target, f32 amount) {
        if (amount <= 0) return;
        auto* hp = sim.world().healths.get(target.id);
        if (!hp) return;
        set_context_unit(target.id);
        set_context_heal_source(source.id);
        set_context_heal_target(target.id);
        set_context_heal_amount(amount);
        fire_event("global_heal", target.id);
        fire_event("unit_heal", target.id);
        amount = get_context_heal_amount();  // trigger may have modified it
        if (amount <= 0) return;
        hp->current = std::min(hp->current + amount, hp->max);
    };

    lua["KillUnit"] = [&](simulation::Unit unit, sol::optional<simulation::Unit> killer) {
        auto* hp = sim.world().healths.get(unit.id);
        if (hp) hp->current = 0;
        set_context_unit(unit.id);
        set_context_killer(killer ? killer->id : UINT32_MAX);
        fire_event("global_death", unit.id);
        fire_event("unit_death", unit.id);
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

    // ── Player API ────────────────────────────────────────────────────

    lua["GetPlayer"] = [](u32 slot) -> simulation::Player { return simulation::Player{slot}; };

    // Lobby-assigned name for a player. Humans get the peer's `player_name`
    // from C_JOIN; Computer slots get the manifest placeholder ("Creeps" etc.);
    // unclaimed slots get "Player N". Empty string for unknown ids.
    lua["GetPlayerName"] = [&](simulation::Player p) -> std::string {
        return std::string{sim.get_player_name(p)};
    };

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

    // ── Fog of War API ────────────────────────────────────────────────────

    auto fog_table = lua.create_named_table("FogOfWar");

    fog_table["reveal_all"] = [&sim](u32 player_id) {
        sim.fog().reveal_all(simulation::Player{player_id});
    };

    fog_table["unexplore_all"] = [&sim](u32 player_id) {
        sim.fog().unexplore_all(simulation::Player{player_id});
    };

    fog_table["is_visible"] = [&sim](u32 player_id, f32 x, f32 y) -> bool {
        auto& fog = sim.fog();
        if (!fog.enabled()) return true;
        auto& terrain = *sim.terrain();
        auto tile = terrain.world_to_tile(x, y);
        return fog.is_visible(simulation::Player{player_id},
                              static_cast<u32>(tile.x), static_cast<u32>(tile.y));
    };

    fog_table["is_explored"] = [&sim](u32 player_id, f32 x, f32 y) -> bool {
        auto& fog = sim.fog();
        if (!fog.enabled()) return true;
        auto& terrain = *sim.terrain();
        auto tile = terrain.world_to_tile(x, y);
        return fog.is_explored(simulation::Player{player_id},
                               static_cast<u32>(tile.x), static_cast<u32>(tile.y));
    };

    fog_table["is_enabled"] = [&sim]() -> bool {
        return sim.fog().enabled();
    };

    // ── Environment API ────────────────────────────────────────────────────

    lua["SetSunDirection"] = [this](f32 x, f32 y, f32 z) {
        if (!m_renderer) return;
        map::EnvironmentConfig env;
        env.sun_direction = glm::normalize(glm::vec3{x, y, z});
        // Preserve existing settings, just update sun direction
        // (full env config update — simple approach)
        m_renderer->set_environment(env);
    };

    lua["AddPointLight"] = [this](f32 x, f32 y, f32 z, f32 r, f32 g, f32 b, f32 radius, f32 intensity) {
        if (!m_renderer) return;
        m_renderer->add_point_light({x, y, z}, {r, g, b}, radius, intensity);
    };

    // ── Session API ───────────────────────────────────────────────────────

    lua["EndGame"] = [this](u32 winner_id, sol::optional<std::string> stats_json) {
        std::string stats = stats_json.value_or("{}");
        log::info("Script", "EndGame called — winner: {}", winner_id);
        fire_event("global_game_end");
        if (m_end_game_fn) m_end_game_fn(winner_id, stats);
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

    lua["TriggerRegisterEvent"] = [&](sol::table t, sol::object event_obj) {
        if (!event_obj.is<std::string>() || event_obj.as<std::string>().empty()) {
            log::warn(TAG, "TriggerRegisterEvent: event name is nil or empty");
            return;
        }
        std::string event_name = event_obj.as<std::string>();
        u32 id = t["_id"].get<u32>();
        auto it = m_triggers.find(id);
        if (it != m_triggers.end()) {
            it->second.events.push_back({event_name});
        }
    };

    lua["TriggerRegisterUnitEvent"] = [&](sol::table t, simulation::Unit unit, sol::object event_obj) {
        if (!event_obj.is<std::string>() || event_obj.as<std::string>().empty()) {
            log::warn(TAG, "TriggerRegisterUnitEvent: event name is nil or empty");
            return;
        }
        std::string event_name = event_obj.as<std::string>();
        u32 id = t["_id"].get<u32>();
        auto it = m_triggers.find(id);
        if (it != m_triggers.end()) {
            Trigger::EventBinding eb;
            eb.event_name = event_name;
            eb.unit_id = unit.id;
            it->second.events.push_back(std::move(eb));
        }
    };

    lua["TriggerRegisterPlayerEvent"] = [&](sol::table t, simulation::Player player, sol::object event_obj) {
        if (!event_obj.is<std::string>() || event_obj.as<std::string>().empty()) {
            log::warn(TAG, "TriggerRegisterPlayerEvent: event name is nil or empty");
            return;
        }
        std::string event_name = event_obj.as<std::string>();
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
    // GetTriggerNode() — id of the HUD node that fired a button_pressed
    // (or other node-event) trigger. Empty string outside a node-event
    // action.
    lua["GetTriggerNode"] = [this]() -> std::string { return m_ctx_node_id; };
    lua["GetTriggerAbilityId"] = [&]() -> std::string {
        if (m_ctx_ability.empty()) {
            log::warn(TAG, "GetTriggerAbilityId() called but no ability in context (event: '{}')", m_ctx_event);
        }
        return m_ctx_ability;
    };
    lua["GetDamageSource"] = [&, unit_or_nil, warn_wrong_event]() -> sol::object {
        warn_wrong_event("GetDamageSource", {"global_damage", "unit_damage"});
        return unit_or_nil(make_unit(sim.world(), m_ctx_damage_source));
    };
    lua["GetDamageTarget"] = [&, unit_or_nil, warn_wrong_event]() -> sol::object {
        warn_wrong_event("GetDamageTarget", {"global_damage", "unit_damage"});
        return unit_or_nil(make_unit(sim.world(), m_ctx_damage_target));
    };
    lua["GetDamageAmount"] = [&, warn_wrong_event]() -> f32 {
        warn_wrong_event("GetDamageAmount", {"global_damage", "unit_damage"});
        return m_ctx_damage_amount;
    };
    lua["SetDamageAmount"] = [&, warn_wrong_event](f32 v) {
        if (warn_wrong_event("SetDamageAmount", {"global_damage", "unit_damage"})) return;
        if (v < 0) v = 0;
        m_ctx_damage_amount = v;
    };
    lua["GetDamageType"] = [&, warn_wrong_event]() -> std::string {
        warn_wrong_event("GetDamageType", {"global_damage", "unit_damage"});
        return m_ctx_damage_type;
    };
    lua["GetKillingUnit"] = [&, unit_or_nil, warn_wrong_event]() -> sol::object {
        warn_wrong_event("GetKillingUnit", {"global_death", "unit_death"});
        return unit_or_nil(make_unit(sim.world(), m_ctx_killer));
    };

    // Heal context
    lua["GetHealSource"] = [&, unit_or_nil, warn_wrong_event]() -> sol::object {
        warn_wrong_event("GetHealSource", {"global_heal", "unit_heal"});
        return unit_or_nil(make_unit(sim.world(), m_ctx_heal_source));
    };
    lua["GetHealTarget"] = [&, unit_or_nil, warn_wrong_event]() -> sol::object {
        warn_wrong_event("GetHealTarget", {"global_heal", "unit_heal"});
        return unit_or_nil(make_unit(sim.world(), m_ctx_heal_target));
    };
    lua["GetHealAmount"] = [&, warn_wrong_event]() -> f32 {
        warn_wrong_event("GetHealAmount", {"global_heal", "unit_heal"});
        return m_ctx_heal_amount;
    };
    lua["SetHealAmount"] = [&, warn_wrong_event](f32 v) {
        if (warn_wrong_event("SetHealAmount", {"global_heal", "unit_heal"})) return;
        if (v < 0) v = 0;
        m_ctx_heal_amount = v;
    };
    lua["GetSpellTargetUnit"] = [&, unit_or_nil, warn_wrong_event]() -> sol::object {
        warn_wrong_event("GetSpellTargetUnit", {"global_ability_effect", "unit_ability_effect"});
        return unit_or_nil(make_unit(sim.world(), m_ctx_spell_target_unit));
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

// ── Input API ─────────────────────────────────────────────────────────────

void ScriptEngine::set_input(input::SelectionState* selection, input::CommandSystem* commands) {
    m_selection = selection;
    m_commands = commands;

    // Wire selection change → on_select event
    if (m_selection) {
        m_selection->on_change = [this]() {
            fire_event("global_select");
        };
    }

    // Wire order observer → on_order event. Fires AFTER the command
    // has been issued; handlers can read the order's context but
    // cannot cancel — cast validity is data-driven by ability
    // target_filter, not script-overridable from this hook.
    if (m_commands) {
        m_commands->set_order_observer([this](const input::GameCommand& cmd) {
            // Decompose command into context
            m_ctx_order_player = cmd.player.id;
            m_ctx_order_queued = cmd.queued;
            m_ctx_order_target_x = 0;
            m_ctx_order_target_y = 0;
            m_ctx_order_target_unit = UINT32_MAX;

            std::visit([this](auto&& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, simulation::orders::Move>) {
                    m_ctx_order_type = "move";
                    m_ctx_order_target_x = payload.target.x;
                    m_ctx_order_target_y = payload.target.y;
                } else if constexpr (std::is_same_v<T, simulation::orders::AttackMove>) {
                    m_ctx_order_type = "attack_move";
                    m_ctx_order_target_x = payload.target.x;
                    m_ctx_order_target_y = payload.target.y;
                } else if constexpr (std::is_same_v<T, simulation::orders::Attack>) {
                    m_ctx_order_type = "attack";
                    m_ctx_order_target_unit = payload.target.id;
                } else if constexpr (std::is_same_v<T, simulation::orders::Stop>) {
                    m_ctx_order_type = "stop";
                } else if constexpr (std::is_same_v<T, simulation::orders::HoldPosition>) {
                    m_ctx_order_type = "hold";
                } else if constexpr (std::is_same_v<T, simulation::orders::Patrol>) {
                    m_ctx_order_type = "patrol";
                    if (!payload.waypoints.empty()) {
                        m_ctx_order_target_x = payload.waypoints[0].x;
                        m_ctx_order_target_y = payload.waypoints[0].y;
                    }
                } else if constexpr (std::is_same_v<T, simulation::orders::Cast>) {
                    m_ctx_order_type = "cast";
                    m_ctx_order_target_unit = payload.target_unit.id;
                    m_ctx_order_target_x = payload.target_pos.x;
                    m_ctx_order_target_y = payload.target_pos.y;
                } else {
                    m_ctx_order_type = "other";
                }
            }, cmd.order);

            fire_event("global_order", UINT32_MAX, "", cmd.player.id);
            fire_event("player_order", UINT32_MAX, "", cmd.player.id);
        });
    }
}

void ScriptEngine::bind_input_api() {
    auto& lua = *m_lua;

    // ── Selection API ────────────────────────────────────────────────────

    lua["GetSelectedUnits"] = [this]() -> sol::table {
        auto& lua = *m_lua;
        sol::table result = lua.create_table();
        if (m_selection) {
            u32 idx = 1;
            for (auto& u : m_selection->selected()) {
                result[idx++] = u.id;
            }
        }
        return result;
    };

    lua["GetSelectedUnitCount"] = [this]() -> u32 {
        return m_selection ? m_selection->count() : 0;
    };

    lua["SelectUnit"] = [this](simulation::Unit unit) {
        if (!m_selection) return;
        m_selection->select(unit);
    };

    // SetControlledUnit(unit) — Action-preset semantic alias for
    // SelectUnit. The Action preset locks selection to this unit
    // (doesn't mutate it further), and the HUD action bar reads the
    // unit's abilities for its slots.
    lua["SetControlledUnit"] = [this](simulation::Unit unit) {
        if (!m_selection) return;
        m_selection->select(unit);
    };

    lua["SelectUnits"] = [this](sol::table tbl) {
        if (!m_selection || !m_sim) return;
        std::vector<simulation::Unit> units;
        for (auto& [k, v] : tbl) {
            u32 id = v.as<u32>();
            auto* info = m_sim->world().handle_infos.get(id);
            if (info) {
                simulation::Unit u;
                u.id = id;
                u.generation = info->generation;
                units.push_back(u);
            }
        }
        m_selection->select_multiple(std::move(units));
    };

    lua["ClearSelection"] = [this]() {
        if (m_selection) m_selection->clear();
    };

    lua["IsUnitSelected"] = [this](u32 unit_id) -> bool {
        if (!m_selection || !m_sim) return false;
        auto* info = m_sim->world().handle_infos.get(unit_id);
        if (!info) return false;
        simulation::Unit u;
        u.id = unit_id;
        u.generation = info->generation;
        return m_selection->is_selected(u);
    };

    // ── Order event context ──────────────────────────────────────────────

    lua["GetOrderType"] = [this]() -> std::string { return m_ctx_order_type; };
    lua["GetOrderTargetX"] = [this]() -> f32 { return m_ctx_order_target_x; };
    lua["GetOrderTargetY"] = [this]() -> f32 { return m_ctx_order_target_y; };
    lua["GetOrderTargetUnit"] = [this]() -> u32 { return m_ctx_order_target_unit; };
    lua["GetOrderPlayer"] = [this]() -> u32 { return m_ctx_order_player; };
    lua["IsOrderQueued"] = [this]() -> bool { return m_ctx_order_queued; };
}

// ── HUD bindings ─────────────────────────────────────────────────────────
// Atom state setters + text tag create/destroy + setters. All of these
// no-op cleanly if the HUD pointer was never wired (e.g., in a pure
// server build) or the referenced node id doesn't exist.

void ScriptEngine::bind_hud_api() {
    auto& lua = *m_lua;

    // Parse "#RRGGBB" / "#RRGGBBAA" color string → packed u32. Matches the
    // hud.json loader's expectation so Lua authors can use the same color
    // literals they see in JSON.
    auto parse_color = [](const std::string& s) -> hud::Color {
        auto hex_nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        auto hex_byte = [&](char a, char b) -> u8 {
            int hi = hex_nibble(a), lo = hex_nibble(b);
            if (hi < 0 || lo < 0) return 0;
            return static_cast<u8>((hi << 4) | lo);
        };
        if (s.size() == 7 && s[0] == '#') {
            return hud::rgba(hex_byte(s[1], s[2]), hex_byte(s[3], s[4]), hex_byte(s[5], s[6]), 255);
        }
        if (s.size() == 9 && s[0] == '#') {
            return hud::rgba(hex_byte(s[1], s[2]), hex_byte(s[3], s[4]),
                             hex_byte(s[5], s[6]), hex_byte(s[7], s[8]));
        }
        return hud::rgba(255, 255, 255, 255);
    };

    // ── Node lookup + state setters ──────────────────────────────────────

    // GetNode(id) — returns the id string if the node exists, nil otherwise.
    // Idiom: `if GetNode("my_label") then SetLabelText("my_label", "...") end`.
    lua["GetNode"] = [this](const std::string& id) -> sol::object {
        auto& lua = *m_lua;
        if (!m_hud) return sol::make_object(lua, sol::nil);
        auto* n = m_hud->find_node_by_id(id);
        return n ? sol::make_object(lua, id) : sol::make_object(lua, sol::nil);
    };

    // State setters route through Hud's by-id API so both local mutation
    // and MP sync emission happen in one call.
    lua["SetNodeVisible"]   = [this](const std::string& id, bool visible)          { if (m_hud) m_hud->set_node_visible(id, visible); };
    lua["SetLabelText"]     = [this](const std::string& id, const std::string& t)  { if (m_hud) m_hud->set_label_text(id, t); };
    lua["SetBarFill"]       = [this](const std::string& id, f32 fill)              { if (m_hud) m_hud->set_bar_fill(id, fill); };
    lua["SetImageSource"]   = [this](const std::string& id, const std::string& p)  { if (m_hud) m_hud->set_image_source(id, p); };
    lua["SetButtonEnabled"] = [this](const std::string& id, bool enabled)          { if (m_hud) m_hud->set_button_enabled(id, enabled); };

    // CreateNode(template_id, { anchor, x, y, w, h, owner }): instantiate
    // a template defined in the map's hud.json `nodes` block at the
    // requested placement. Templates define what the node is (type,
    // style, content, children); Lua decides where it goes and who
    // sees it. `owner` (optional) = GetPlayer(N) → only that player's
    // client sees this node. Omit for a broadcast (all clients).
    // Returns the id string on success, nil on failure.
    lua["CreateNode"] = [this](const std::string& template_id,
                               sol::table placement) -> sol::object {
        auto& lua = *m_lua;
        if (!m_hud) return sol::make_object(lua, sol::nil);

        std::string anchor_str = placement["anchor"].get_or(std::string("tl"));
        hud::Hud::Placement pl{};
        pl.anchor = anchor_str;
        pl.x      = placement["x"].get_or(0.0f);
        pl.y      = placement["y"].get_or(0.0f);
        pl.w      = placement["w"].get_or(0.0f);
        pl.h      = placement["h"].get_or(0.0f);
        if (auto o = placement["owner"]; o.valid()) {
            pl.owner_player = o.get<simulation::Player>().id;
        }
        if (pl.w <= 0.0f || pl.h <= 0.0f) {
            log::warn("HUD", "CreateNode '{}': w/h must be > 0 (got {}, {})",
                      template_id, pl.w, pl.h);
        }

        bool ok = m_hud->instantiate_template(template_id, pl);
        return ok ? sol::make_object(lua, template_id)
                  : sol::make_object(lua, sol::nil);
    };

    // DestroyNode(id): remove an instantiated node (and its entire
    // subtree) from the HUD. No-op if the id isn't in the current tree.
    lua["DestroyNode"] = [this](const std::string& id) -> bool {
        if (!m_hud) return false;
        return m_hud->remove_node_by_id(id);
    };

    // ── Text tags ────────────────────────────────────────────────────────
    // Handles: a single Lua integer packing (generation << 32) | index.
    // Generation 0 = invalid sentinel.

    auto pack_id = [](hud::TextTagId id) -> u64 {
        return (static_cast<u64>(id.generation) << 32) | static_cast<u64>(id.index);
    };
    auto unpack_id = [](u64 h) -> hud::TextTagId {
        return hud::TextTagId{ static_cast<u32>(h & 0xFFFFFFFFu),
                               static_cast<u32>(h >> 32) };
    };

    // CreateTextTag{ ... }. See docs/ui.md for the full list of fields.
    lua["CreateTextTag"] = [this, parse_color, pack_id](sol::table args) -> u64 {
        if (!m_hud) return 0;
        hud::TextTagCreateInfo info{};
        // `proxy.get_or(default)` avoids ambiguity with sol2's template
        // overloads; type is deduced from the default value.
        info.text      = args["text"].get_or(std::string{});
        info.px_size   = args["size"].get_or(14.0f);

        // Position: either `pos = { x, y, z }` world point or `unit = U`.
        if (args["unit"].valid()) {
            info.unit = args["unit"].get<simulation::Unit>();
        } else if (args["pos"].valid()) {
            sol::table p = args["pos"];
            info.pos = { p[1].get_or(0.0f), p[2].get_or(0.0f), p[3].get_or(0.0f) };
        }
        info.z_offset = args["z_offset"].get_or(0.0f);

        if (auto c = args["color"]; c.valid()) info.color = parse_color(c.get<std::string>());

        if (auto v = args["velocity"]; v.valid()) {
            sol::table vt = v;
            info.velocity_x = vt[1].get_or(0.0f);
            info.velocity_y = vt[2].get_or(0.0f);
        }
        info.lifespan  = args["lifespan"].get_or(0.0f);
        info.fadepoint = args["fadepoint"].get_or(0.0f);

        // owner (optional) — if set, only that player's client sees the tag.
        if (auto o = args["owner"]; o.valid()) {
            info.owner_player = o.get<simulation::Player>().id;
        }

        return pack_id(m_hud->create_text_tag(info));
    };

    lua["DestroyTextTag"] = [this, unpack_id](u64 h) {
        if (!m_hud) return;
        m_hud->destroy_text_tag(unpack_id(h));
    };

    lua["SetTextTagText"] = [this, unpack_id](u64 h, const std::string& text) {
        if (!m_hud) return;
        m_hud->set_text_tag_text(unpack_id(h), text);
    };
    lua["SetTextTagPos"] = [this, unpack_id](u64 h, f32 x, f32 y, f32 z) {
        if (!m_hud) return;
        m_hud->set_text_tag_pos(unpack_id(h), x, y, z);
    };
    lua["SetTextTagPosUnit"] = [this, unpack_id](u64 h, simulation::Unit unit, f32 z_offset) {
        if (!m_hud) return;
        m_hud->set_text_tag_pos_unit(unpack_id(h), unit.id, z_offset);
    };
    lua["SetTextTagColor"] = [this, unpack_id, parse_color](u64 h, const std::string& color) {
        if (!m_hud) return;
        m_hud->set_text_tag_color(unpack_id(h), parse_color(color));
    };
    lua["SetTextTagVelocity"] = [this, unpack_id](u64 h, f32 vx, f32 vy) {
        if (!m_hud) return;
        m_hud->set_text_tag_velocity(unpack_id(h), vx, vy);
    };
    lua["SetTextTagVisible"] = [this, unpack_id](u64 h, bool visible) {
        if (!m_hud) return;
        m_hud->set_text_tag_visible(unpack_id(h), visible);
    };

    // ── Action-bar composite ─────────────────────────────────────────────
    // The bar is declared in hud.json (`composites.action_bar`). Two
    // binding modes determine how slots fill with abilities:
    //
    //   auto   — slot contents come from the local player's selection,
    //            resolved by the global hotkey-mode setting. No Lua
    //            wiring needed. Default for RTS-style maps.
    //   manual — each slot is explicitly bound to an ability by Lua
    //            (ActionBarSetSlot). The slot renders + fires only
    //            when the selected unit actually owns that ability.
    //            Intended for action / MOBA-style maps where the
    //            author curates the skill lineup.
    //
    // Passive abilities can be bound in manual mode but shouldn't —
    // nothing fires when the slot is triggered (convention, not
    // enforced). Slot indices are 1-based in Lua (WC3 convention),
    // 0-based internally.

    lua["ActionBarSetVisible"] = [this](bool visible) {
        if (!m_hud) return;
        m_hud->action_bar_set_visible(visible);
    };
    lua["ActionBarSetSlotVisible"] = [this](u32 slot_1based, bool visible) {
        if (!m_hud || slot_1based == 0) return;
        m_hud->action_bar_set_slot_visible(slot_1based - 1, visible);
    };

    // ActionBarSetSlot(slot, ability_id) — manual-mode binding. A log
    // warning is issued if the ability is passive, but the binding
    // still takes effect (no-op at cast time).
    lua["ActionBarSetSlot"] = [this](u32 slot_1based, const std::string& ability_id) {
        if (!m_hud || slot_1based == 0) return;
        if (m_sim) {
            if (const auto* def = m_sim->abilities().get(ability_id)) {
                if (def->form == simulation::AbilityForm::Passive ||
                    def->form == simulation::AbilityForm::Aura) {
                    log::warn("HUD", "ActionBarSetSlot({}, '{}'): ability is passive/aura, nothing will fire",
                              slot_1based, ability_id);
                }
            } else {
                log::warn("HUD", "ActionBarSetSlot({}, '{}'): ability not in registry",
                          slot_1based, ability_id);
            }
        }
        m_hud->action_bar_set_slot(slot_1based - 1, ability_id);
    };

    lua["ActionBarClearSlot"] = [this](u32 slot_1based) {
        if (!m_hud || slot_1based == 0) return;
        m_hud->action_bar_clear_slot(slot_1based - 1);
    };
}

} // namespace uldum::script
