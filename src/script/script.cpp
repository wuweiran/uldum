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

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <tuple>

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
    lua.new_usertype<simulation::Item>("Item",
        "id", &simulation::Item::id,
        "is_valid", &simulation::Item::is_valid,
        sol::meta_function::equal_to, [](const simulation::Item& a, const simulation::Item& b) { return a == b; }
    );

    bind_api();
    bind_trigger_api();
    bind_timer_api();
    bind_input_api();
    bind_save_api();
    bind_hud_api();

    // Hook the world's damage callback so all damage (combat + script) fires on_damage events
    sim.world().on_damage = [this](simulation::Unit source, simulation::Unit target, f32& amount, std::string_view damage_type) {
        // Save outer damage context — a trigger's action can call
        // DamageUnit(), which recurses into on_damage and overwrites the
        // global m_ctx_damage_* fields with the nested event's values.
        // Without save/restore, when control returns to the outer
        // fire_event loop and the NEXT trigger's condition runs (e.g.
        // unveiling strike running after cleave), GetDamageType() reads
        // the nested event's "cleave" instead of the original "attack",
        // and the trigger silently misfires.
        u32 saved_unit          = m_ctx_unit;
        u32 saved_damage_source = m_ctx_damage_source;
        u32 saved_damage_target = m_ctx_damage_target;
        f32 saved_damage_amount = m_ctx_damage_amount;
        std::string saved_damage_type = m_ctx_damage_type;

        set_context_unit(target.id);
        set_context_damage_source(source.id);
        set_context_damage_target(target.id);
        set_context_damage_amount(amount);
        set_context_damage_type(damage_type);
        fire_event("global_damage", target.id);
        fire_event("unit_damage", target.id);
        amount = get_context_damage_amount();

        m_ctx_unit          = saved_unit;
        m_ctx_damage_source = saved_damage_source;
        m_ctx_damage_target = saved_damage_target;
        m_ctx_damage_amount = saved_damage_amount;
        m_ctx_damage_type   = std::move(saved_damage_type);
    };

    // Hook dying callback — runs while HP is at 0 but BEFORE reap.
    // Handlers can SetUnitHealth back up to save the unit (Reincarnation
    // / Phoenix Fire / Cheat Death). If any handler leaves HP > 0 the
    // sim cancels the death entirely; on_death does NOT fire in that
    // case. Mirror of the death pair below.
    sim.world().on_dying = [this](simulation::Unit dying, simulation::Unit killer) {
        set_context_unit(dying.id);
        set_context_killer(killer.is_valid() ? killer.id : UINT32_MAX);
        fire_event("global_dying", dying.id);
        fire_event("unit_dying", dying.id);
    };

    // Hook death callback so system_death fires on_death events
    sim.world().on_death = [this](simulation::Unit dying, simulation::Unit killer) {
        set_context_unit(dying.id);
        set_context_killer(killer.is_valid() ? killer.id : UINT32_MAX);
        fire_event("global_death", dying.id);
        fire_event("unit_death", dying.id);
    };

    // Hook order callback — fires every time a new order survives the
    // sim's admission checks. std::visit unpacks the order variant to
    // a string tag + target unit + target point + ability id (for Cast
    // orders), all surfaced through trigger context accessors:
    //   GetTriggerUnit()          — the unit getting the order
    //   GetTriggerOrderType()     — "move" / "attack" / "cast" / ...
    //   GetOrderTargetUnit()      — target unit handle (nil if none)
    //   GetOrderTargetX/Y()       — target ground point
    //   GetTriggerAbilityId()     — for "cast" orders (existing context)
    sim.world().on_order = [this](simulation::Unit unit, const simulation::Order& order) {
        std::string type_tag;
        u32  target_uid = UINT32_MAX;
        f32  target_x = 0, target_y = 0;
        std::string ability_id;
        std::visit([&](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, simulation::orders::Move>) {
                type_tag = "move";
                if (p.target_unit.is_valid()) target_uid = p.target_unit.id;
                target_x = p.target.x; target_y = p.target.y;
            } else if constexpr (std::is_same_v<T, simulation::orders::AttackMove>) {
                type_tag = "attack_move";
                target_x = p.target.x; target_y = p.target.y;
            } else if constexpr (std::is_same_v<T, simulation::orders::Attack>) {
                type_tag = "attack";
                if (p.target.is_valid()) target_uid = p.target.id;
            } else if constexpr (std::is_same_v<T, simulation::orders::Stop>) {
                type_tag = "stop";
            } else if constexpr (std::is_same_v<T, simulation::orders::HoldPosition>) {
                type_tag = "hold";
            } else if constexpr (std::is_same_v<T, simulation::orders::Patrol>) {
                type_tag = "patrol";
                if (!p.waypoints.empty()) {
                    target_x = p.waypoints.front().x;
                    target_y = p.waypoints.front().y;
                }
            } else if constexpr (std::is_same_v<T, simulation::orders::Cast>) {
                type_tag = "cast";
                ability_id = p.ability_id;
                if (p.target_unit.is_valid()) target_uid = p.target_unit.id;
                target_x = p.target_pos.x; target_y = p.target_pos.y;
            } else if constexpr (std::is_same_v<T, simulation::orders::Train>) {
                type_tag = "train";
            } else if constexpr (std::is_same_v<T, simulation::orders::Research>) {
                type_tag = "research";
            } else if constexpr (std::is_same_v<T, simulation::orders::Build>) {
                type_tag = "build";
                target_x = p.pos.x; target_y = p.pos.y;
            } else if constexpr (std::is_same_v<T, simulation::orders::PickupItem>) {
                type_tag = "pickup";
            } else if constexpr (std::is_same_v<T, simulation::orders::DropItem>) {
                type_tag = "drop";
                target_x = p.pos.x; target_y = p.pos.y;
            } else if constexpr (std::is_same_v<T, simulation::orders::SwapInventorySlot>) {
                type_tag = "swap_inventory_slot";
            } else if constexpr (std::is_same_v<T, simulation::orders::MoveDirection>) {
                type_tag = "move_direction";
                target_x = p.dir.x; target_y = p.dir.y;
            }
        }, order.payload);

        set_context_unit(unit.id);
        set_context_order_type(std::move(type_tag));
        set_context_order_target_unit(target_uid);
        set_context_order_target_x(target_x);
        set_context_order_target_y(target_y);
        set_context_ability(ability_id);
        fire_event("global_issued_order", unit.id);
        fire_event("unit_issued_order", unit.id);
    };

    // Ability cast-lifecycle event fan-out. All three callbacks share
    // the same args and produce the same trigger context (caster + ability
    // + spell target). They fire at distinct moments — channel start,
    // channel end (natural OR interrupt), and effect resolution. See
    // World's on_ability_* docs for the ordering vs Foreswing /
    // Channeling / Backswing.
    auto fan_out_cast = [this](std::string_view global, std::string_view scoped,
                               simulation::Unit caster, std::string_view ability_id,
                               simulation::Unit target_unit, glm::vec3 target_pos,
                               simulation::Item source_item) {
        set_context_unit(caster.id);
        set_context_ability(std::string(ability_id));
        set_context_spell_target_unit(target_unit.is_valid() ? target_unit.id : UINT32_MAX);
        set_context_spell_target_x(target_pos.x);
        set_context_spell_target_y(target_pos.y);
        set_context_item(source_item);
        fire_event(global, caster.id, ability_id);
        fire_event(scoped, caster.id, ability_id);
    };

    sim.world().on_ability_channel = [fan_out_cast](simulation::Unit caster, std::string_view ability_id,
                                                     simulation::Unit target_unit, glm::vec3 target_pos,
                                                     simulation::Item source_item) {
        fan_out_cast("global_ability_channel", "unit_ability_channel",
                     caster, ability_id, target_unit, target_pos, source_item);
    };

    sim.world().on_ability_endcast = [fan_out_cast](simulation::Unit caster, std::string_view ability_id,
                                                     simulation::Unit target_unit, glm::vec3 target_pos,
                                                     simulation::Item source_item) {
        fan_out_cast("global_ability_endcast", "unit_ability_endcast",
                     caster, ability_id, target_unit, target_pos, source_item);
    };

    sim.world().on_ability_effect = [fan_out_cast](simulation::Unit caster, std::string_view ability_id,
                                                    simulation::Unit target_unit, glm::vec3 target_pos,
                                                    simulation::Item source_item) {
        fan_out_cast("global_ability_effect", "unit_ability_effect",
                     caster, ability_id, target_unit, target_pos, source_item);
    };

    // Item events fired by system_items.
    sim.world().on_item_picked_up = [this](simulation::Unit unit, simulation::Item item, i32 /*slot*/) {
        set_context_unit(unit.id);
        set_context_item(item);
        fire_event("global_item_picked_up", unit.id);
        fire_event("unit_item_picked_up", unit.id);
    };
    sim.world().on_item_dropped = [this](simulation::Unit unit, simulation::Item item) {
        set_context_unit(unit.id);
        set_context_item(item);
        fire_event("global_item_dropped", unit.id);
        fire_event("unit_item_dropped", unit.id);
    };

    // Region events fired by system_regions per tick. Region id is
    // passed both as the event filter (so a trigger registered for one
    // specific region fires only for it) and via the context, so the
    // action body can read GetTriggerRegion() if it needs to.
    sim.world().on_region_enter = [this](u32 region_id, simulation::Unit unit) {
        set_context_unit(unit.id);
        set_context_region_id(region_id);
        fire_event("region_enter", unit.id, "", UINT32_MAX, region_id);
    };
    sim.world().on_region_leave = [this](u32 region_id, simulation::Unit unit) {
        set_context_unit(unit.id);
        set_context_region_id(region_id);
        fire_event("region_leave", unit.id, "", UINT32_MAX, region_id);
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

    // Clean up dead triggers — same soft-delete pattern. Triggers
    // are marked dead by DestroyTrigger (which can be called from
    // inside a trigger action, including the trigger's own action);
    // the actual erase happens here so the action's std::function
    // isn't destroyed while it's still on the call stack.
    std::vector<u32> dead_trigs;
    for (auto& [id, trig] : m_triggers) {
        if (!trig.alive) dead_trigs.push_back(id);
    }
    for (u32 id : dead_trigs) {
        m_triggers.erase(id);
    }

    // Fog-aware effect dispatch — per-tick visibility delta for every
    // active effect. The `delivered` set is the players currently
    // rendering it. Vision gain → deliver Create. Vision loss →
    // deliver Destroy. Effects live until the author calls
    // DestroyEffect (or the convenience PlayEffect / PlayEffectOnUnit
    // wrappers do a Create + immediate Destroy).
    //
    // In-flight particles already emitted on the client continue to
    // fade naturally after a Destroy — the particle system owns
    // them. Re-entering vision while the effect is still alive
    // re-fires the burst on the client, which is the right thing for
    // permanent emitters (portals) and harmless for one-shots.
    if (m_sim && (m_effect_deliver_fn || m_effect_destroy_fn)) {
        const auto& world = m_sim->world();
        for (auto& e : m_active_effects) {
            // For unit-attached effects, track the unit's current
            // position so late-delivered players spawn the effect at
            // the unit's now-position.
            if (e.entity_id != UINT32_MAX) {
                if (auto* t = world.transforms.get(e.entity_id)) e.position = t->position;
            }
            for (u32 p = 0; p < m_player_count; ++p) {
                const bool visible = effect_visible_to(e, p);
                const bool was = e.delivered.contains(p);
                if (visible && !was) {
                    if (m_effect_deliver_fn) {
                        m_effect_deliver_fn(p, e.server_id, e.name, e.entity_id,
                                            e.position, e.attach_point);
                    }
                    e.delivered.insert(p);
                } else if (!visible && was) {
                    if (m_effect_destroy_fn) m_effect_destroy_fn(p, e.server_id);
                    e.delivered.erase(p);
                }
            }
        }
    }
}

bool ScriptEngine::effect_visible_to(const ActiveEffect& e, u32 player_id) const {
    if (!m_sim) return true;
    const auto& world  = m_sim->world();
    const auto& vision = m_sim->vision();
    if (e.entity_id != UINT32_MAX) {
        return vision.is_unit_visible_to(world, *m_sim, e.entity_id,
                                          simulation::Player{player_id});
    }
    const auto* terrain = m_sim->terrain();
    if (!terrain || !vision.enabled()) return true;
    auto tile = terrain->world_to_tile(e.position.x, e.position.y);
    u32 tx = static_cast<u32>(tile.x);
    u32 ty = static_cast<u32>(tile.y);
    if (tx >= vision.tiles_x() || ty >= vision.tiles_y()) return false;
    return vision.is_visible(simulation::Player{player_id}, tx, ty);
}

// ── Event firing ──────────────────────────────────────────────────────────

void ScriptEngine::fire_node_event(std::string_view event_name, u32 player_id,
                                    std::string_view node_id) {
    set_context_node_id(std::string(node_id));
    set_context_player(player_id);
    fire_event(event_name, UINT32_MAX, "", player_id, UINT32_MAX, node_id);
    m_ctx_node_id.clear();
}

void ScriptEngine::fire_event(std::string_view event_name, u32 unit_id,
                               std::string_view /*ability_id*/, u32 player_id,
                               u32 region_id, std::string_view node_id) {
    m_ctx_event = std::string(event_name);

    auto matches = [&](const Trigger& trig) -> bool {
        for (auto& eb : trig.events) {
            if (eb.event_name != event_name) continue;
            if (eb.unit_id   != UINT32_MAX && eb.unit_id   != unit_id)   continue;
            if (eb.player_id != UINT32_MAX && eb.player_id != player_id) continue;
            if (eb.region_id != UINT32_MAX && eb.region_id != region_id) continue;
            if (!eb.node_id.empty() && eb.node_id != node_id)            continue;
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

    // Transform a unit into a different type in place. Same handle, same
    // position, same owner. Health / mana carry by percentage. Type-listed
    // abilities flip available true/false so cooldowns ride through morph
    // and back; Lua-added abilities, item-granted passives, and applied
    // buffs are untouched. Returns true on success, false if the type is
    // unknown or the handle is stale.
    lua["MorphUnit"] = [&](simulation::Unit unit, const std::string& new_type_id) -> bool {
        return simulation::morph_unit(sim.world(), unit, new_type_id);
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

    // Script-driven animation. SetUnitAnimation replaces whatever the
    // unit was playing with `clip` (so back-to-back calls overwrite
    // rather than queue). QueueUnitAnimation appends to the queue,
    // creating one if none exists. ResetUnitAnimation drops the queue
    // and lets the engine's per-frame state derivation take back over.
    //
    // Death always wins — when the unit's hp hits 0 the renderer
    // drops the queue and plays the death clip, regardless of what
    // the map scripted. See AnimQueue (components.h) for the contract.
    lua["SetUnitAnimation"] = [&](simulation::Unit u, const std::string& clip,
                                   sol::optional<bool> looping) {
        auto& w = sim.world();
        if (!w.validate(u)) return;
        auto* q = w.anim_queues.get(u.id);
        if (q) {
            q->clips.clear();
            q->clips.push_back(clip);
            q->looping = looping.value_or(false);
        } else {
            simulation::AnimQueue aq;
            aq.clips.push_back(clip);
            aq.looping = looping.value_or(false);
            w.anim_queues.add(u.id, std::move(aq));
        }
    };

    lua["QueueUnitAnimation"] = [&](simulation::Unit u, const std::string& clip) {
        auto& w = sim.world();
        if (!w.validate(u)) return;
        auto* q = w.anim_queues.get(u.id);
        if (q) {
            q->clips.push_back(clip);
        } else {
            simulation::AnimQueue aq;
            aq.clips.push_back(clip);
            w.anim_queues.add(u.id, std::move(aq));
        }
    };

    lua["ResetUnitAnimation"] = [&](simulation::Unit u) {
        sim.world().anim_queues.remove(u.id);
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
        if (!ab) return;
        // Writes to base; the next recalc re-sums passive ability
        // modifiers on top so a +3 armor aura still adds 3 after the
        // script writes a new base.
        ab->base[name] = value;
        simulation::recalculate_modifiers(sim.world(), u.id);
        if (m_unit_update_fn) {
            f32 effective = ab->numeric.count(name) ? ab->numeric.at(name) : value;
            auto pkt = network::build_update_attr(u.id, name, effective);
            m_unit_update_fn(u.id, pkt);
        }
    };

    lua["GetUnitStringAttribute"] = [&](simulation::Unit u, const std::string& name) -> std::string {
        auto* ab = sim.world().attribute_blocks.get(u.id);
        if (!ab) return "";
        auto it = ab->string_attrs.find(name);
        return it != ab->string_attrs.end() ? it->second : "";
    };

    // Status flags. The flag arg is a lowercase string ("stunned",
    // "silenced", "muted", "disarmed", "rooted", "invulnerable",
    // "magic_immune", "untargetable", "paused"). Unknown strings are
    // logged and ignored. The bitmask lives on a per-unit StatusFlags
    // component; sim systems gate per-tick processing on these.
    auto parse_status_flag = [](std::string_view s) -> u32 {
        if (s == "stunned")      return simulation::status::Stunned;
        if (s == "silenced")     return simulation::status::Silenced;
        if (s == "muted")        return simulation::status::Muted;
        if (s == "disarmed")     return simulation::status::Disarmed;
        if (s == "rooted")       return simulation::status::Rooted;
        if (s == "invulnerable") return simulation::status::Invulnerable;
        if (s == "magic_immune") return simulation::status::MagicImmune;
        if (s == "untargetable") return simulation::status::Untargetable;
        if (s == "unattackable") return simulation::status::Unattackable;
        if (s == "paused")       return simulation::status::Paused;
        if (s == "invisible")    return simulation::status::Invisible;
        return 0;
    };

    lua["SetUnitStatus"] = [&, parse_status_flag](simulation::Unit unit,
                                                   const std::string& flag,
                                                   bool on) {
        u32 bit = parse_status_flag(flag);
        if (bit == 0) {
            log::warn(TAG, "SetUnitStatus: unknown flag '{}'", flag);
            return;
        }
        simulation::set_unit_status(sim.world(), unit, bit, on);
        if (m_unit_update_fn) {
            auto pkt = network::build_update_status(unit.id, bit, on);
            m_unit_update_fn(unit.id, pkt);
        }
    };

    lua["GetUnitStatus"] = [&, parse_status_flag](simulation::Unit unit,
                                                   const std::string& flag) -> bool {
        u32 bit = parse_status_flag(flag);
        if (bit == 0) {
            log::warn(TAG, "GetUnitStatus: unknown flag '{}'", flag);
            return false;
        }
        return simulation::unit_has_status(sim.world(), unit, bit);
    };

    lua["ClearAllUnitStatus"] = [&](simulation::Unit unit) {
        simulation::clear_all_unit_status(sim.world(), unit);
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

    // Network sync for these is plumbed at the engine level via
    // World::on_ability_added / on_ability_removed — fires from every
    // path (Lua, engine aura broadcast, natural duration expiry) so
    // clients stay in lockstep regardless of which path drove the change.
    lua["AddAbility"] = [&](simulation::Unit unit, const std::string& ability_id, sol::optional<u32> level) -> bool {
        return simulation::add_ability(sim.world(), sim.abilities(), unit, ability_id, level.value_or(1));
    };

    lua["RemoveAbility"] = [&](simulation::Unit unit, const std::string& ability_id) -> bool {
        return simulation::remove_ability(sim.world(), unit, ability_id);
    };

    lua["ApplyPassiveAbility"] = [&](simulation::Unit target, const std::string& ability_id, simulation::Unit source, f32 duration) -> bool {
        return simulation::apply_passive_ability(sim.world(), sim.abilities(), target, ability_id, source, duration);
    };

    lua["HasAbility"] = [&](simulation::Unit u, const std::string& ability_id) -> bool {
        return simulation::has_ability(sim.world(), u, ability_id);
    };

    // Mutate a modifier value on every active instance of an ability and
    // re-run recalculate_modifiers so the effective attribute changes
    // immediately. Used by Lua-driven tweens (Wind Walk's fade-in, slow
    // ramp-down, etc.) — the ability owns the modifier state, Lua just
    // drives the curve. Returns true if any instance was touched.
    lua["SetAbilityModifier"] = [&](simulation::Unit u, const std::string& ability_id,
                                     const std::string& key, f32 value) -> bool {
        auto& w = sim.world();
        if (!w.validate(u)) return false;
        auto* aset = w.ability_sets.get(u.id);
        if (!aset) return false;
        bool changed = false;
        for (auto& a : aset->abilities) {
            if (a.ability_id == ability_id) {
                a.active_modifiers[key] = value;
                changed = true;
            }
        }
        if (changed) {
            simulation::recalculate_modifiers(w, u.id);
            if (m_unit_update_fn) {
                auto pkt = network::build_update_ability_modifier(u.id, ability_id, key, value);
                m_unit_update_fn(u.id, pkt);
            }
        }
        return changed;
    };

    lua["GetAbilityLevel"] = [&](simulation::Unit u, const std::string& ability_id) -> u32 {
        return simulation::get_ability_level(sim.world(), u, ability_id);
    };

    lua["GetAbilityStackCount"] = [&](simulation::Unit u, const std::string& ability_id) -> u32 {
        return simulation::get_ability_stack_count(sim.world(), u, ability_id);
    };

    // Read / write the cooldown remaining on a unit's ability (seconds).
    // Used by morph helpers to preserve cooldowns across the
    // RemoveAbility / MorphUnit / AddAbility round-trip — read before
    // removing, write back after re-adding. Returns 0 / no-op when the
    // unit doesn't have the ability.
    lua["GetAbilityCooldown"] = [&](simulation::Unit u, const std::string& ability_id) -> f32 {
        if (!sim.world().validate(u)) return 0.0f;
        auto* aset = sim.world().ability_sets.get(u.id);
        if (!aset) return 0.0f;
        for (const auto& a : aset->abilities) {
            if (a.ability_id == ability_id) return a.cooldown_remaining;
        }
        return 0.0f;
    };

    lua["SetAbilityCooldown"] = [&](simulation::Unit u, const std::string& ability_id, f32 secs) {
        if (!sim.world().validate(u)) return;
        auto* aset = sim.world().ability_sets.get(u.id);
        if (!aset) return;
        for (auto& a : aset->abilities) {
            if (a.ability_id == ability_id) {
                a.cooldown_remaining = std::max(0.0f, secs);
                return;
            }
        }
    };

    // List the ids in a unit type's authored `abilities` array. Used
    // by morph helpers to drive Remove/Add loops without the map
    // having to duplicate the type's ability list in Lua. Returns an
    // empty table if the type id is unknown.
    lua["GetUnitTypeAbilities"] = [&](const std::string& type_id) -> sol::table {
        sol::table out = lua.create_table();
        const auto* def = sim.types().get_unit_type(type_id);
        if (!def) return out;
        u32 idx = 1;
        for (const auto& aid : def->abilities) {
            out[idx++] = aid;
        }
        return out;
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
    lua["RandomFloat"] = [](f32 min, f32 max) -> f32 { return min + static_cast<f32>(std::rand()) / static_cast<f32>(RAND_MAX) * (max - min); };

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

    // DefineEffect (runtime effect def from Lua) intentionally
    // omitted. All effect defs live in `types/effects.json` — loaded
    // by both host and client at session start — so the registries
    // stay symmetric and there's no peer-join replay edge case.

    // Effect lifecycle. Every effect is created via Create* and lives
    // until the author calls DestroyEffect — WC3 convention. Burst /
    // one-shot effects use the `DestroyEffect(CreateEffect(...))`
    // idiom; DestroyEffect does a late-delivery pass to every player
    // who can see the effect's position right now BEFORE tearing
    // down, so the burst still fires correctly. PlayEffect /
    // PlayEffectOnUnit are kept as convenience wrappers around that
    // pattern so map code stays terse.
    lua["CreateEffect"] = [this](const std::string& name, f32 x, f32 y, f32 z) -> u32 {
        ActiveEffect e;
        e.server_id = m_next_effect_id++;
        e.name      = name;
        e.position  = {x, y, z};
        e.entity_id = UINT32_MAX;
        m_active_effects.push_back(std::move(e));
        return m_active_effects.back().server_id;
    };

    lua["CreateEffectOnUnit"] = [this, &sim](const std::string& name, simulation::Unit unit,
                                              sol::optional<std::string> attach_point) -> u32 {
        auto* t = sim.world().transforms.get(unit.id);
        if (!t) return 0;
        ActiveEffect e;
        e.server_id    = m_next_effect_id++;
        e.name         = name;
        e.position     = t->position;
        e.entity_id    = unit.id;
        e.attach_point = attach_point ? *attach_point : std::string{};
        m_active_effects.push_back(std::move(e));
        return m_active_effects.back().server_id;
    };

    auto destroy_effect_impl = [this](u32 id) {
        for (auto it = m_active_effects.begin(); it != m_active_effects.end(); ++it) {
            if (it->server_id != id) continue;
            // Late delivery: any player who can see the spot RIGHT
            // NOW (e.g. a `DestroyEffect(CreateEffect(...))` burst
            // called in the same tick the per-tick dispatcher would
            // have run) gets a Create then immediately a Destroy.
            // Particles emitted on Create fade naturally; this is the
            // WC3 idiom and what makes one-shot bursts work.
            if (m_effect_deliver_fn) {
                for (u32 p = 0; p < m_player_count; ++p) {
                    if (it->delivered.contains(p)) continue;
                    if (effect_visible_to(*it, p)) {
                        m_effect_deliver_fn(p, it->server_id, it->name,
                                            it->entity_id, it->position, it->attach_point);
                        it->delivered.insert(p);
                    }
                }
            }
            if (m_effect_destroy_fn) {
                for (u32 p : it->delivered) m_effect_destroy_fn(p, id);
            }
            m_active_effects.erase(it);
            return;
        }
    };
    lua["DestroyEffect"] = destroy_effect_impl;

    // Convenience: one-shot semantics via Create + immediate Destroy.
    // Functionally identical to `DestroyEffect(CreateEffect(...))`.
    lua["PlayEffect"] = [this, destroy_effect_impl](const std::string& name,
                                                     f32 x, f32 y, f32 z) {
        ActiveEffect e;
        e.server_id = m_next_effect_id++;
        e.name      = name;
        e.position  = {x, y, z};
        e.entity_id = UINT32_MAX;
        u32 id = e.server_id;
        m_active_effects.push_back(std::move(e));
        destroy_effect_impl(id);
    };
    lua["PlayEffectOnUnit"] = [this, &sim, destroy_effect_impl](const std::string& name,
                                                                 simulation::Unit unit,
                                                                 sol::optional<std::string> attach_point) {
        auto* t = sim.world().transforms.get(unit.id);
        if (!t) return;
        ActiveEffect e;
        e.server_id    = m_next_effect_id++;
        e.name         = name;
        e.position     = t->position;
        e.entity_id    = unit.id;
        e.attach_point = attach_point ? *attach_point : std::string{};
        u32 id = e.server_id;
        m_active_effects.push_back(std::move(e));
        destroy_effect_impl(id);
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

    // ── Camera API ────────────────────────────────────────────────────────
    // Per-player. Scripts run on the host only, so each command takes
    // a Player handle (from GetPlayer(N)) so the host knows whose
    // screen to drive. Targeting "all players" means looping in Lua.
    // Offline collapses to "apply locally" inside the registered fn.
    //
    // Position arguments below (SetCameraPosition / PanCamera) are
    // GROUND TARGET XY — the point on the world floor the camera
    // should look at, matching WC3's SetCameraPosition / PanCameraTo
    // semantics. Eye position is derived from current pitch / yaw /
    // height so scripts don't redo the math when zoom changes.
    //
    // SetCameraLockUnit is a hard lock — while active, the targeted
    // player's WASD / drag / scroll inputs do nothing until the
    // script unlocks (`SetCameraLockUnit(player, nil)`).

    lua["SetCameraPosition"] = [this](simulation::Player p, f32 x, f32 y) {
        if (m_camera_set_position_fn) m_camera_set_position_fn(p.id, x, y);
    };
    lua["PanCamera"] = [this](simulation::Player p, f32 x, f32 y, f32 duration) {
        if (m_camera_pan_fn) m_camera_pan_fn(p.id, x, y, duration);
    };
    lua["SetCameraZoom"] = [this](simulation::Player p, f32 z) {
        if (m_camera_zoom_fn) m_camera_zoom_fn(p.id, z);
    };
    lua["CameraShake"] = [this](simulation::Player p, f32 intensity, f32 duration) {
        if (m_camera_shake_fn) m_camera_shake_fn(p.id, intensity, duration);
    };
    // unit is optional — pass nil to release the lock.
    lua["SetCameraLockUnit"] = [this](simulation::Player p, sol::optional<simulation::Unit> u) {
        if (!m_camera_lock_unit_fn) return;
        // Default-constructed Unit has id = UINT32_MAX, which the
        // controller treats as "unlock".
        simulation::Unit unit = u.value_or(simulation::Unit{});
        m_camera_lock_unit_fn(p.id, unit);
    };

    // ── Vision API ────────────────────────────────────────────────────────
    // Tile-level fog of war queries. Per-unit invisibility is a separate
    // concern handled by UNIT_STATUS_INVISIBLE and the SetUnitStatus API.

    lua["FogEnable"] = [&sim](bool on) {
        sim.vision().set_enabled(on);
    };

    lua["IsFogEnabled"] = [&sim]() -> bool {
        return sim.vision().enabled();
    };

    auto point_state = [&sim](simulation::Player p, f32 x, f32 y) -> simulation::Visibility {
        auto& vision = sim.vision();
        if (!vision.enabled()) return simulation::Visibility::Visible;
        auto& terrain = *sim.terrain();
        auto tile = terrain.world_to_tile(x, y);
        return vision.get(p, static_cast<u32>(tile.x), static_cast<u32>(tile.y));
    };

    lua["IsPointVisible"] = [point_state](simulation::Player p, f32 x, f32 y) -> bool {
        return point_state(p, x, y) == simulation::Visibility::Visible;
    };

    lua["IsPointFogged"] = [point_state](simulation::Player p, f32 x, f32 y) -> bool {
        return point_state(p, x, y) == simulation::Visibility::Explored;
    };

    lua["IsPointMasked"] = [point_state](simulation::Player p, f32 x, f32 y) -> bool {
        return point_state(p, x, y) == simulation::Visibility::Unexplored;
    };

    lua["IsPointExplored"] = [point_state](simulation::Player p, f32 x, f32 y) -> bool {
        return point_state(p, x, y) != simulation::Visibility::Unexplored;
    };

    auto parse_fog_state = [](std::string_view s) -> simulation::Visibility {
        if (s == "fogged" || s == "explored") return simulation::Visibility::Explored;
        if (s == "masked" || s == "unexplored") return simulation::Visibility::Unexplored;
        return simulation::Visibility::Visible;
    };

    lua["CreateFogModifierRect"] = [&sim, parse_fog_state](
            simulation::Player p, const std::string& state,
            f32 x0, f32 y0, f32 x1, f32 y1) -> u32 {
        return sim.vision().create_fog_modifier_rect(p, parse_fog_state(state), x0, y0, x1, y1);
    };

    lua["CreateFogModifierRadius"] = [&sim, parse_fog_state](
            simulation::Player p, const std::string& state,
            f32 cx, f32 cy, f32 radius) -> u32 {
        return sim.vision().create_fog_modifier_radius(p, parse_fog_state(state), cx, cy, radius);
    };

    lua["DestroyFogModifier"] = [&sim](u32 id) {
        sim.vision().destroy_fog_modifier(id);
    };

    lua["SetFogModifierActive"] = [&sim](u32 id, bool active) {
        sim.vision().set_fog_modifier_active(id, active);
    };

    lua["IsUnitVisibleToPlayer"] = [&sim](simulation::Unit u, simulation::Player p) -> bool {
        if (!sim.world().validate(u)) return false;
        return sim.vision().is_unit_visible_to(sim.world(), sim, u.id, p);
    };

    lua["IsUnitDetected"] = [&sim](simulation::Unit u, simulation::Player p) -> bool {
        if (!sim.world().validate(u)) return false;
        const auto* tv = sim.world().true_sight_vis.get(u.id);
        return tv && (tv->revealed_to_mask & (1u << p.id));
    };

    lua["UnitReveal"] = [&sim](simulation::Unit u, simulation::Player p, bool on) {
        auto& w = sim.world();
        if (!w.validate(u) || !p.is_valid()) return;
        u32 bit = 1u << p.id;
        auto* fv = w.forced_vis.get(u.id);
        if (on) {
            if (fv) fv->revealed_to_mask |= bit;
            else    w.forced_vis.add(u.id, simulation::ForcedVisibility{bit});
        } else if (fv) {
            fv->revealed_to_mask &= ~bit;
            if (fv->revealed_to_mask == 0) w.forced_vis.remove(u.id);
        }
    };

    lua["UnitShareVision"] = [&sim](simulation::Unit u, simulation::Player p, bool on) {
        auto& w = sim.world();
        if (!w.validate(u) || !p.is_valid()) return;
        auto* s = w.sights.get(u.id);
        if (!s) return;
        auto it = std::find(s->share_to_players.begin(), s->share_to_players.end(), p.id);
        if (on) {
            if (it == s->share_to_players.end()) s->share_to_players.push_back(p.id);
        } else if (it != s->share_to_players.end()) {
            s->share_to_players.erase(it);
        }
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

    // ── Item API ─────────────────────────────────────────────────────────
    // Items as a collection of abilities — engine stores type + two free
    // integer fields (`charges`, `level`) but doesn't interpret them.
    // Map Lua drives consumption / level-up / merge / drop-on-death.

    auto item_or_nil = [&](simulation::Item it) -> sol::object {
        return it.is_valid() ? sol::make_object(*m_lua, it)
                              : sol::make_object(*m_lua, sol::nil);
    };

    lua["CreateItem"] = [&, item_or_nil](const std::string& type_id, f32 x, f32 y) -> sol::object {
        auto item = simulation::create_item(sim.world(), type_id, x, y);
        if (item.is_valid()) {
            if (auto* t = sim.world().transforms.get(item.id)) {
                t->position.z      = ::uldum::map::sample_height(terrain_ref, x, y);
                t->prev_position.z = t->position.z;
            }
        }
        return item_or_nil(item);
    };
    lua["RemoveItem"] = [&](simulation::Item item) {
        // If carried, revoke abilities + clear the carrier's slot
        // before destroying the entity.
        if (auto* car = sim.world().carriables.get(item.id);
            car && car->carried_by.is_valid()) {
            auto* inv = sim.world().inventories.get(car->carried_by.id);
            if (inv) {
                for (auto& s : inv->slots) {
                    if (s.id == item.id && s.generation == item.generation) {
                        s = simulation::Item{};
                        break;
                    }
                }
            }
            // Revoke abilities granted by this item.
            if (sim.world().types) {
                if (auto* info = sim.world().item_infos.get(item.id)) {
                    if (auto* def = sim.world().types->get_item_type(info->type_id)) {
                        for (const auto& aid : def->abilities) {
                            simulation::remove_ability(sim.world(), car->carried_by, aid);
                        }
                    }
                }
            }
        }
        simulation::destroy(sim.world(), item);
    };

    lua["GiveItem"] = [&](simulation::Unit unit, simulation::Item item) -> i32 {
        return simulation::give_item_to_unit(sim.world(), unit, item);
    };
    lua["UnitDropItemFromSlot"] = [&](simulation::Unit unit, i32 slot, sol::optional<f32> x, sol::optional<f32> y) {
        glm::vec3 pos{0, 0, 0};
        if (auto* tf = sim.world().transforms.get(unit.id)) pos = tf->position;
        if (x) pos.x = *x;
        if (y) pos.y = *y;
        // pull existing item slot
        auto* inv = sim.world().inventories.get(unit.id);
        if (!inv || slot < 0 || slot >= static_cast<i32>(inv->slots.size())) return false;
        auto item = inv->slots[slot];
        if (!item.is_valid()) return false;
        return simulation::drop_item_from_unit(sim.world(), unit, slot, pos);
    };
    lua["UnitGetItemFromSlot"] = [&, item_or_nil](simulation::Unit unit, i32 slot) -> sol::object {
        auto* inv = sim.world().inventories.get(unit.id);
        if (!inv || slot < 0 || slot >= static_cast<i32>(inv->slots.size())) {
            return sol::make_object(*m_lua, sol::nil);
        }
        return item_or_nil(inv->slots[slot]);
    };
    lua["UnitItemCount"] = [&](simulation::Unit unit) -> i32 {
        auto* inv = sim.world().inventories.get(unit.id);
        if (!inv) return 0;
        i32 n = 0;
        for (auto& it : inv->slots) if (it.is_valid()) ++n;
        return n;
    };
    lua["UnitInventorySize"] = [&](simulation::Unit unit) -> i32 {
        auto* inv = sim.world().inventories.get(unit.id);
        return inv ? static_cast<i32>(inv->slots.size()) : 0;
    };
    lua["UnitHasItemOfType"] = [&](simulation::Unit unit, const std::string& type_id) -> bool {
        auto* inv = sim.world().inventories.get(unit.id);
        if (!inv) return false;
        for (auto& it : inv->slots) {
            if (!it.is_valid()) continue;
            auto* info = sim.world().item_infos.get(it.id);
            if (info && info->type_id == type_id) return true;
        }
        return false;
    };

    lua["GetItemTypeId"]  = [&](simulation::Item item) -> std::string {
        auto* info = sim.world().item_infos.get(item.id);
        return info ? info->type_id : std::string{};
    };
    lua["GetItemCharges"] = [&](simulation::Item item) -> i32 { return simulation::get_charges(sim.world(), item); };
    lua["SetItemCharges"] = [&](simulation::Item item, i32 n) { simulation::set_charges(sim.world(), item, n); };
    lua["GetItemLevel"]   = [&](simulation::Item item) -> i32 { return simulation::get_level(sim.world(), item); };
    lua["SetItemLevel"]   = [&](simulation::Item item, i32 n) { simulation::set_level(sim.world(), item, n); };
    lua["GetItemOwner"]   = [&, unit_or_nil](simulation::Item item) -> sol::object {
        return unit_or_nil(simulation::get_item_owner(sim.world(), item));
    };
    lua["GetItemPosition"] = [&](simulation::Item item) -> std::tuple<f32, f32, f32> {
        auto* tf = sim.world().transforms.get(item.id);
        if (!tf) return {0.0f, 0.0f, 0.0f};
        return {tf->position.x, tf->position.y, tf->position.z};
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
            // Soft-delete owned timers — same reason as DestroyTimer:
            // a trigger action may DestroyTrigger(self), erasing here
            // would free std::functions still on the call stack.
            // update()'s cleanup pass does the real erase next tick.
            for (u32 timer_id : it->second.owned_timers) {
                auto t_it = m_timers.find(timer_id);
                if (t_it != m_timers.end()) t_it->second.alive = false;
            }
            it->second.alive = false;
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
    // GetTriggerItem() — item the current event is about (or nil).
    // Set during item events (picked_up, dropped) and during ability
    // casts that originated from an item slot. Authors hook this from
    // an EVENT_ABILITY_CAST_FINISHED trigger to drive consumption etc.
    lua["GetTriggerItem"] = [&, item_or_nil = [&](simulation::Item it) -> sol::object {
        return it.is_valid() ? sol::make_object(*m_lua, it) : sol::make_object(*m_lua, sol::nil);
    }]() -> sol::object {
        return item_or_nil(m_ctx_item);
    };
    lua["GetTriggerAbilityId"] = [&]() -> std::string {
        if (m_ctx_ability.empty()) {
            log::warn(TAG, "GetTriggerAbilityId() called but no ability in context (event: '{}')", m_ctx_event);
        }
        return m_ctx_ability;
    };

    // Order-event context — populated by the on_order callback.
    // GetTriggerOrderType returns one of: "move", "attack_move",
    // "attack", "stop", "hold", "patrol", "cast", "train", "research",
    // "build", "pickup", "drop", "swap_inventory_slot",
    // "move_direction". For "cast" orders, GetTriggerAbilityId returns
    // the ability id. GetOrderTargetUnit / X / Y are zero / nil when
    // the order didn't carry that field (e.g. Stop has neither).
    lua["GetTriggerOrderType"] = [&]() -> std::string { return m_ctx_order_type; };
    lua["GetOrderTargetUnit"] = [&, unit_or_nil]() -> sol::object {
        return unit_or_nil(make_unit(sim.world(), m_ctx_order_target_unit));
    };
    lua["GetOrderTargetX"] = [&]() -> f32 { return m_ctx_order_target_x; };
    lua["GetOrderTargetY"] = [&]() -> f32 { return m_ctx_order_target_y; };
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
    lua["GetSpellTargetX"] = [&, warn_wrong_event]() -> f32 {
        warn_wrong_event("GetSpellTargetX", {"global_ability_effect", "unit_ability_effect"});
        return m_ctx_spell_target_x;
    };
    lua["GetSpellTargetY"] = [&, warn_wrong_event]() -> f32 {
        warn_wrong_event("GetSpellTargetY", {"global_ability_effect", "unit_ability_effect"});
        return m_ctx_spell_target_y;
    };
    lua["GetSpellTargetPoint"] = [&, warn_wrong_event]() -> std::tuple<f32, f32> {
        warn_wrong_event("GetSpellTargetPoint", {"global_ability_effect", "unit_ability_effect"});
        return {m_ctx_spell_target_x, m_ctx_spell_target_y};
    };

    // ── Regions ───────────────────────────────────────────────────────
    // Authored from Lua via CreateRegion + AddRegionRect/Circle. The
    // sim's `system_regions` walks every authored region per tick and
    // fires "region_enter" / "region_leave" through this script
    // engine's callback hooks (wired in init()). Triggers that want
    // to listen use TriggerRegisterEnterRegion(trig, region) which
    // attaches a region-id filter to the trigger's binding.

    lua["CreateRegion"] = [&]() -> sol::table {
        u32 id = ++sim.world().next_region_id;
        sim.world().regions[id] = simulation::World::Region{id};

        sol::table r = lua.create_table();
        r["_id"]  = id;
        r["data"] = lua.create_table();
        return r;
    };

    // Look up an editor-authored region by its string id (set in
    // objects.json). Returns nil if no region with that id exists.
    // The returned handle is identical in shape to CreateRegion()'s
    // return value, so AddRegionRect / TriggerRegisterEnterRegion /
    // IsUnitInRegion all accept it.
    lua["GetRegion"] = [&](sol::object id_obj) -> sol::object {
        if (!id_obj.is<std::string>()) return sol::lua_nil;
        const std::string id_str = id_obj.as<std::string>();
        if (id_str.empty()) return sol::lua_nil;
        for (const auto& [rid, reg] : sim.world().regions) {
            if (!reg.alive) continue;
            if (reg.id_str == id_str) {
                sol::table r = lua.create_table();
                r["_id"]  = rid;
                r["data"] = lua.create_table();
                return r;
            }
        }
        return sol::lua_nil;
    };

    // Union AABB across the region's rects and circles. Callers can
    // derive center / size / membership tests from this without the
    // engine baking those into the API. Returns four zeros for an
    // empty / invalid region rather than nil so the multi-return
    // destructure on the Lua side stays uniform.
    lua["GetRegionBounds"] = [&](sol::table region) -> std::tuple<f32, f32, f32, f32> {
        if (!region.valid()) return {0.0f, 0.0f, 0.0f, 0.0f};
        u32 id = region["_id"].get_or<u32>(0);
        auto it = sim.world().regions.find(id);
        if (it == sim.world().regions.end()) return {0.0f, 0.0f, 0.0f, 0.0f};
        const auto& reg = it->second;
        if (reg.rects.empty() && reg.circles.empty()) return {0.0f, 0.0f, 0.0f, 0.0f};
        constexpr f32 INF = std::numeric_limits<f32>::infinity();
        f32 x0 =  INF, y0 =  INF, x1 = -INF, y1 = -INF;
        for (const auto& rc : reg.rects) {
            x0 = std::min(x0, rc.x0); y0 = std::min(y0, rc.y0);
            x1 = std::max(x1, rc.x1); y1 = std::max(y1, rc.y1);
        }
        for (const auto& c : reg.circles) {
            x0 = std::min(x0, c.cx - c.r); y0 = std::min(y0, c.cy - c.r);
            x1 = std::max(x1, c.cx + c.r); y1 = std::max(y1, c.cy + c.r);
        }
        return {x0, y0, x1, y1};
    };

    lua["AddRegionRect"] = [&](sol::table region, f32 x0, f32 y0, f32 x1, f32 y1) {
        if (!region.valid()) return;
        u32 id = region["_id"].get_or<u32>(0);
        auto it = sim.world().regions.find(id);
        if (it == sim.world().regions.end()) return;
        // Author may pass corners in any order — normalise so the
        // contains-check below stays simple.
        if (x1 < x0) std::swap(x0, x1);
        if (y1 < y0) std::swap(y0, y1);
        it->second.rects.push_back({x0, y0, x1, y1});
    };

    lua["AddRegionCircle"] = [&](sol::table region, f32 cx, f32 cy, f32 r) {
        if (!region.valid()) return;
        u32 id = region["_id"].get_or<u32>(0);
        auto it = sim.world().regions.find(id);
        if (it == sim.world().regions.end()) return;
        if (r < 0.0f) r = 0.0f;
        it->second.circles.push_back({cx, cy, r});
    };

    lua["RemoveRegion"] = [&](sol::table region) {
        if (!region.valid()) return;
        u32 id = region["_id"].get_or<u32>(0);
        // Soft-delete: keep around for one more tick so any
        // in-flight enter/leave action that's mid-iteration doesn't
        // dereference into a freed slot. system_regions skips
        // alive=false; we erase next session reset.
        auto it = sim.world().regions.find(id);
        if (it != sim.world().regions.end()) it->second.alive = false;
    };

    lua["IsUnitInRegion"] = [&](simulation::Unit unit, sol::table region) -> bool {
        if (!region.valid()) return false;
        u32 id = region["_id"].get_or<u32>(0);
        auto it = sim.world().regions.find(id);
        if (it == sim.world().regions.end()) return false;
        return it->second.contained.count(unit.id) > 0;
    };

    lua["GetUnitsInRegion"] = [&](sol::table region) -> sol::table {
        sol::table out = lua.create_table();
        if (!region.valid()) return out;
        u32 id = region["_id"].get_or<u32>(0);
        auto it = sim.world().regions.find(id);
        if (it == sim.world().regions.end()) return out;
        u32 idx = 1;
        for (u32 uid : it->second.contained) {
            const auto* hi = sim.world().handle_infos.get(uid);
            simulation::Unit u; u.id = uid;
            if (hi) u.generation = hi->generation;
            out[idx++] = u;
        }
        return out;
    };

    lua["TriggerRegisterEnterRegion"] = [&](sol::table t, sol::table region) {
        u32 trig_id   = t["_id"].get_or<u32>(0);
        u32 region_id = region.valid() ? region["_id"].get_or<u32>(0) : 0;
        auto trig_it = m_triggers.find(trig_id);
        if (trig_it == m_triggers.end()) return;
        Trigger::EventBinding eb;
        eb.event_name = "region_enter";
        eb.region_id  = region_id;
        trig_it->second.events.push_back(std::move(eb));
    };

    lua["TriggerRegisterLeaveRegion"] = [&](sol::table t, sol::table region) {
        u32 trig_id   = t["_id"].get_or<u32>(0);
        u32 region_id = region.valid() ? region["_id"].get_or<u32>(0) : 0;
        auto trig_it = m_triggers.find(trig_id);
        if (trig_it == m_triggers.end()) return;
        Trigger::EventBinding eb;
        eb.event_name = "region_leave";
        eb.region_id  = region_id;
        trig_it->second.events.push_back(std::move(eb));
    };

    // Read the region id for the currently firing region_enter /
    // region_leave action. UINT32_MAX outside that context.
    lua["GetTriggerRegion"] = [&]() -> u32 { return m_ctx_region_id; };

    // ── Node events ─────────────────────────────────────────────────
    // TriggerRegisterNodeEvent(trig, node, event_name) — bind the
    // trigger to a HUD-node event filtered by node id. The id is
    // accepted as a string (returned by GetNode/CreateNode) or as a
    // table with a "_id" field for forward-compat with handle-style
    // wrappers.
    lua["TriggerRegisterNodeEvent"] = [&](sol::table t, sol::object node_obj,
                                          sol::object event_obj) {
        if (!event_obj.is<std::string>() || event_obj.as<std::string>().empty()) {
            log::warn(TAG, "TriggerRegisterNodeEvent: event name is nil or empty");
            return;
        }
        std::string node_id;
        if (node_obj.is<std::string>()) {
            node_id = node_obj.as<std::string>();
        } else if (node_obj.is<sol::table>()) {
            sol::table nt = node_obj.as<sol::table>();
            sol::object idv = nt["_id"];
            if (idv.is<std::string>()) node_id = idv.as<std::string>();
        }
        if (node_id.empty()) {
            log::warn(TAG, "TriggerRegisterNodeEvent: node id is nil or empty");
            return;
        }
        u32 trig_id = t["_id"].get<u32>();
        auto trig_it = m_triggers.find(trig_id);
        if (trig_it == m_triggers.end()) return;
        Trigger::EventBinding eb;
        eb.event_name = event_obj.as<std::string>();
        eb.node_id    = std::move(node_id);
        trig_it->second.events.push_back(std::move(eb));
    };

    // ── Scene switching ─────────────────────────────────────────────
    // LoadScene("scene_01") — request a swap to the named scene of
    // the currently loaded map. Defers actual work to the App
    // (registered via set_scene_switch_fn) so it can run between
    // ticks rather than during trigger iteration.
    lua["LoadScene"] = [this](std::string_view scene_name) {
        if (m_scene_switch_fn) m_scene_switch_fn(scene_name);
    };

    // ── Game pause / single-player query ────────────────────────────
    // PauseGame()/UnpauseGame() flip a script-owned flag the App reads
    // each frame to gate sim ticks. Independent of the network's
    // reconnect-pause; safe to use during dialogs, cutscenes, etc.
    lua["PauseGame"]   = [this]() { m_paused = true;  };
    lua["UnpauseGame"] = [this]() { m_paused = false; };
    lua["IsGamePaused"] = [this]() -> bool { return m_paused; };

    // IsSinglePlayer() — true when launched offline (no network
    // session). Lets map authors gate features that don't make sense
    // in MP (e.g., script-driven pause, save-to-file from gameplay).
    lua["IsSinglePlayer"] = [this]() -> bool { return m_singleplayer; };

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
        // Soft-delete only — the cleanup pass at the bottom of
        // update() does the real erase. A Lua callback is allowed
        // to DestroyTimer(self), and erasing here would free the
        // very std::function that's mid-execution; sol2 then walks
        // freed memory on the way out of the Lua call and crashes
        // inside lua_gettop.
        auto it = m_timers.find(id);
        if (it != m_timers.end()) it->second.alive = false;
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

    // ShowNode / HideNode — short-hand that mirrors ui.md's preferred
    // call form for one-shot toggles (popups, dialogs, alerts). Both
    // route through SetNodeVisible.
    lua["ShowNode"] = [this](const std::string& id) { if (m_hud) m_hud->set_node_visible(id, true);  };
    lua["HideNode"] = [this](const std::string& id) { if (m_hud) m_hud->set_node_visible(id, false); };

    // ── Composite visibility ─────────────────────────────────────────
    // No content API beyond visibility for these — the engine drives
    // their per-frame state internally.
    lua["MinimapSetVisible"]  = [this](bool v) { if (m_hud) m_hud->minimap_set_visible(v);  };
    lua["JoystickSetVisible"] = [this](bool v) { if (m_hud) m_hud->joystick_set_visible(v); };

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
                if (def->form == simulation::AbilityForm::PassiveModifier ||
                    def->form == simulation::AbilityForm::PassiveFlag ||
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
