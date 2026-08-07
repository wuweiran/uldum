#include "network/game_server.h"
#include "network/network.h"
#include "network/protocol.h"
#include "simulation/vision.h"
#include "simulation/command_system.h"
#include "asset/asset.h"
#include "map/map.h"
#include "hud/hud.h"
#include "core/log.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <utility>

namespace uldum::network {

static constexpr const char* TAG = "GameServer";

bool GameServer::init_simulation(asset::AssetManager& assets) {
    if (!m_simulation.init(assets)) {
        log::error(TAG, "Simulation init failed");
        return false;
    }
    log::info(TAG, "Simulation initialized");
    return true;
}

bool GameServer::init_game(map::MapManager& map,
                            audio::AudioEngine* audio,
                            PreMainHook pre_main_hook) {
    // Scripting
    if (!m_script.init(m_simulation, map, audio)) {
        log::error(TAG, "ScriptEngine init failed");
        return false;
    }
    m_audio = audio;  // retained for switch_scene's VM re-init

    if (!run_scene_scripts(map, map.manifest().start_scene, pre_main_hook)) {
        return false;
    }

    log::info(TAG, "GameServer initialized");
    return true;
}

// Shared script-load tail for init_game + switch_scene. Assumes m_script was
// just init()'d and the scene's world entities exist. Runs main() for scene.
bool GameServer::run_scene_scripts(map::MapManager& map, std::string_view scene_name,
                                   const PreMainHook& pre_main) {
    // Player count for the per-tick effect-visibility scan. Re-set here because a
    // scene switch's script.shutdown()/init() clears it.
    m_script.set_player_count(static_cast<u32>(map.manifest().players.size()));

    // Apply building pathing blocks and spatial grid update
    m_simulation.sync_pathing_blockers();
    m_simulation.spatial_grid().update(m_simulation.world());

    // Configure Lua require() search paths: scene → shared → engine
    {
        std::string scene_scripts = map.map_root() + "/scenes/" + std::string(scene_name) + "/scripts";
        std::string shared_scripts = map.map_root() + "/scripts";
        std::string engine_scripts = "engine/scripts";
        m_script.set_script_paths(scene_scripts, shared_scripts, engine_scripts);
    }

    // Configure save data directory (%APPDATA%/saves/<map_uuid>/)
    {
        std::string map_id = map.manifest().id;
        if (map_id.empty()) map_id = map.manifest().name;  // fallback if no id

        std::string save_dir;
#ifdef _WIN32
        char* appdata = nullptr;
        size_t appdata_len = 0;
        if (_dupenv_s(&appdata, &appdata_len, "APPDATA") == 0 && appdata) {
            save_dir = std::string(appdata) + "/saves/" + map_id;
            free(appdata);
        } else {
            save_dir = "saves/" + map_id;
        }
#else
        // Android / POSIX — fall back to a relative path under CWD. Real
        // mobile save-dir handling (app-private storage via GameActivity)
        // comes later.
        save_dir = "saves/" + map_id;
#endif
        m_script.set_save_path(save_dir);
    }

    // Load engine constants (available to all scripts via require("constants"))
    m_script.load_script("engine/scripts/constants.lua");

    // Pre-main hook: the caller re-installs callbacks the VM (re)init cleared
    // (set_hud / set_script / wire_to_network / render hooks) and/or injects
    // globals (e.g. GAME_SESSION), all BEFORE any map script runs.
    if (pre_main) pre_main(m_script);

    // Load and run per-scene main script. Every map MUST define `main()` in its
    // scene's scripts/main.lua (or fall back to <map>/scripts/main.lua). Missing
    // main() is a hard error — "a map without gameplay logic" isn't supported.
    {
        std::string scene_dir = map.map_root() + "/scenes/" + std::string(scene_name) + "/scripts";
        std::string main_script = scene_dir + "/main.lua";
        bool loaded = m_script.load_script(main_script);
        if (!loaded) {
            std::string fallback = map.map_root() + "/scripts/main.lua";
            loaded = m_script.load_script(fallback);
        }
        if (!loaded) {
            log::error(TAG,
                "Map '{}' has no scripts/main.lua at '{}' (or {}/scripts/main.lua). "
                "Every map must define a main() entry point.",
                map.manifest().name, main_script, map.map_root());
            return false;
        }
        if (!m_script.call_function("main")) {
            log::error(TAG,
                "Map '{}' scripts loaded but main() is not defined or errored out.",
                map.manifest().name);
            return false;
        }
    }
    return true;
}

bool GameServer::switch_scene(map::MapManager& map,
                              std::string_view scene_name,
                              PreMainHook pre_main) {
    log::info(TAG, "Scene switch → '{}'", scene_name);

    m_script.shutdown();
    if (!m_script.init(m_simulation, map, m_audio)) {
        log::error(TAG, "switch_scene: ScriptEngine re-init failed for '{}'", scene_name);
        return false;
    }
    return run_scene_scripts(map, scene_name, pre_main);
}

void GameServer::begin_scene_switch(NetworkManager& net,
                                    std::string_view scene_name,
                                    const TeardownHook& local_teardown) {
    // Tell clients first, then load the caller's local scene data while clients
    // do the same. Reliable ordering keeps later spawn/HUD packets behind this.
    std::string scene(scene_name);
    net.host_broadcast_scene_switch(scene);
    if (local_teardown) local_teardown(scene);
    clear_replication();
    m_controlled_unit_by_player.clear();
    m_pending_finalize_scene = scene;
    net.mark_self_loaded();
}

bool GameServer::try_finish_scene_switch(NetworkManager& net, map::MapManager& map,
                                         const PreMainHook& pre_main) {
    if (m_pending_finalize_scene.empty() || !net.is_scene_switching() ||
        !net.all_peers_loaded()) {
        return false;
    }
    std::string scene = std::move(m_pending_finalize_scene);
    m_pending_finalize_scene.clear();

    if (!switch_scene(map, scene, pre_main)) return false;
    net.host_finish_scene_switch();
    log::info(TAG, "Scene switch '{}' complete — sim resuming", scene);
    return true;
}

void GameServer::shutdown() {
    m_pending_finalize_scene.clear();
    clear_replication();
    m_controlled_unit_by_player.clear();
    m_hud_replay = nullptr;
    m_placement_count = 0;
    m_commands = nullptr;
    m_script.shutdown();
    m_simulation.shutdown();
    log::info(TAG, "GameServer shut down");
}

void GameServer::tick(f32 dt) {
    m_simulation.tick(dt);
    m_script.update(dt);
}

bool GameServer::receive_order(simulation::Player player,
                               std::span<const u8> packet) {
    auto command = parse_order(packet, player);
    if (!command) return false;
    if (m_commands) m_commands->submit(*command);
    return true;
}

void GameServer::receive_node_event(simulation::Player player,
                                    std::span<const u8> packet) {
    ByteReader reader(packet);
    reader.read_u8();
    std::string node_id = reader.read_string();
    auto kind = static_cast<NodeEventKind>(reader.read_u8());
    if (kind == NodeEventKind::ButtonPressed) {
        m_script.fire_node_event("button_pressed", player.id, node_id);
    }
}

void GameServer::peer_disconnected(u32 player_id) {
    m_script.fire_player_event("global_disconnect", player_id);
    m_script.fire_player_event("player_disconnect", player_id);
}

void GameServer::player_dropped(u32 player_id) {
    m_script.fire_player_event("global_leave", player_id);
    m_script.fire_player_event("player_leave", player_id);
    m_known_by_player.erase(player_id);
    m_controlled_unit_by_player.erase(player_id);
}

static UnitState build_unit_state(const simulation::World& world, u32 id) {
    UnitState state{};
    state.entity_id = id;
    const auto* transform = world.transforms.get(id);
    state.x = transform->position.x;
    state.y = transform->position.y;
    state.z = transform->position.z;
    state.facing = transform->facing;

    if (const auto* movement = world.movements.get(id);
        movement && movement->moving) {
        state.flags |= 0x01;
    }
    if (const auto* abilities = world.ability_sets.get(id);
        abilities &&
        (abilities->cast_state == simulation::CastState::Foreswing ||
         abilities->cast_state == simulation::CastState::Channeling ||
         abilities->cast_state == simulation::CastState::Backswing)) {
        state.flags |= 0x04;
    }
    if (const auto* health = world.healths.get(id)) {
        state.flags |= 0x20;
        state.health_current = health->current;
    }
    if (const auto* states = world.state_blocks.get(id)) {
        state.state_currents.reserve(states->states.size());
        for (const auto& [name, value] : states->states) {
            state.state_currents.push_back(value.current);
        }
    }
    return state;
}

MaterializeData GameServer::materialize_data(u32 entity_id) const {
    const auto& world = m_simulation.world();
    const auto& info = *world.handle_infos.get(entity_id);
    const auto* transform = world.transforms.get(entity_id);
    MaterializeData data;
    data.entity_id = entity_id;
    data.type_id = info.type_id;
    data.x = transform->position.x;
    data.y = transform->position.y;
    switch (info.category) {
    case simulation::Category::Unit:
        data.category = MaterializeCategory::Unit;
        data.payload = UnitMaterialize{
            static_cast<u8>(world.owners.get(entity_id)->id), transform->facing};
        break;
    case simulation::Category::Destructable:
        data.category = MaterializeCategory::Destructable;
        data.payload = DestructableMaterialize{
            transform->facing, world.destructables.get(entity_id)->variation};
        break;
    case simulation::Category::Item:
        data.category = MaterializeCategory::Item;
        data.payload = ItemMaterialize{};
        break;
    case simulation::Category::Projectile: {
        data.category = MaterializeCategory::Projectile;
        data.type_id = world.renderables.get(entity_id)->model_path;
        const auto* projectile = world.projectiles.get(entity_id);
        data.payload = ProjectileMaterialize{
            transform->facing, projectile->target.id, projectile->is_attack};
        break;
    }
    case simulation::Category::Doodad:
        break;
    }
    return data;
}

std::vector<ColdRecord> GameServer::collect_cold_records(u32 entity_id) const {
    const auto& world = m_simulation.world();
    std::vector<ColdRecord> records;
    const simulation::UnitTypeDef* unit_def = nullptr;
    if (world.types) {
        if (const auto* info = world.handle_infos.get(entity_id)) {
            unit_def = world.types->get_unit_type(info->type_id);
        }
    }

    if (const auto* health = world.healths.get(entity_id);
        health && health->current < health->max) {
        records.push_back(cold_health(health->current, health->max));
    }
    if (const auto* states = world.state_blocks.get(entity_id)) {
        for (const auto& [key, value] : states->states) {
            if (value.current != value.max) {
                records.push_back(cold_state(key, value.current, value.max));
            }
        }
    }
    if (const auto* abilities = world.ability_sets.get(entity_id)) {
        for (const auto& ability : abilities->abilities) {
            bool is_default = unit_def &&
                std::find(unit_def->abilities.begin(), unit_def->abilities.end(),
                          ability.ability_id) != unit_def->abilities.end();
            if (!is_default) {
                for (const auto& source : ability.sources) {
                    records.push_back(cold_ability_add_rec(
                        ability.ability_id, ability.level,
                        static_cast<u8>(simulation::ability_source_kind(source))));
                }
                for (const auto& [key, value] : ability.active_modifiers) {
                    records.push_back(cold_ability_modifier_rec(
                        ability.ability_id, key, value));
                }
            }
            if (ability.cooldown_remaining > 0.0f) {
                records.push_back(cold_cooldown_rec(
                    ability.ability_id, ability.cooldown_remaining));
            }
        }
    }
    if (const auto* item = world.item_infos.get(entity_id)) {
        const simulation::ItemTypeDef* def = world.types
            ? world.types->get_item_type(item->type_id)
            : nullptr;
        if (!def || item->charges != def->initial_charges) {
            records.push_back(cold_item_charges_rec(item->charges));
        }
        if (!def || item->level != def->initial_level) {
            records.push_back(cold_item_level_rec(item->level));
        }
    }
    if (const auto* queue = world.anim_queues.get(entity_id);
        queue && queue->looping && !queue->clips.empty()) {
        records.push_back(cold_anim_rec(queue->clips.back()));
    }
    if (const auto* construction = world.constructions.get(entity_id);
        construction && construction->under_construction) {
        records.push_back(cold_construction_rec(
            true, construction->build_time_total, construction->build_progress));
    }
    return records;
}

void GameServer::send_cold_batch(NetworkManager& net, u32 peer_id,
                                 u32 entity_id) {
    auto records = collect_cold_records(entity_id);
    if (!records.empty()) {
        net.send_to_peer(peer_id, build_cold_batch(entity_id, records));
    }
}

void GameServer::send_inventory_state(NetworkManager& net, u32 peer_id,
                                      u32 carrier_id) {
    const auto& world = m_simulation.world();
    const auto* inventory = world.inventories.get(carrier_id);
    if (!inventory) return;
    for (u32 slot = 0; slot < inventory->slots.size(); ++slot) {
        simulation::Item item = inventory->slots[slot];
        if (world.contains(item)) {
            net.send_to_peer(
                peer_id, build_cold_inventory(carrier_id, slot, item.id));
        }
    }
}

void GameServer::send_spawn(NetworkManager& net, u32 peer_id,
                            simulation::Player player, u32 entity_id,
                            bool born) {
    auto packet = born ? build_spawn(materialize_data(entity_id))
                       : build_show(materialize_data(entity_id));
    net.send_to_peer(peer_id, packet);
    m_known_by_player[player.id].insert(entity_id);
    send_cold_batch(net, peer_id, entity_id);
    send_inventory_state(net, peer_id, entity_id);
}

bool GameServer::is_visible_to(u32 entity_id,
                               simulation::Player player) const {
    return m_simulation.vision().is_unit_visible_to(
        m_simulation.world(), m_simulation, entity_id, player);
}

void GameServer::send_spawn_burst(NetworkManager& net, u32 peer_id,
                                  simulation::Player player) {
    auto& world = m_simulation.world();
    auto& known = m_known_by_player[player.id];
    for (u32 i = 0; i < world.handle_infos.count(); ++i) {
        u32 id = world.handle_infos.ids()[i];
        const auto& info = world.handle_infos.data()[i];
        if (info.category == simulation::Category::Doodad) continue;
        if (id < m_placement_count) {
            if (info.hidden) {
                net.send_to_peer(peer_id, build_destroy(id));
                continue;
            }
            known.insert(id);
            if (is_visible_to(id, player)) {
                send_cold_batch(net, peer_id, id);
                send_inventory_state(net, peer_id, id);
            }
            continue;
        }
        if (is_visible_to(id, player)) {
            send_spawn(net, peer_id, player, id, false);
        }
    }
}

void GameServer::broadcast_tick(NetworkManager& net, u32 tick) {
    auto& world = m_simulation.world();
    auto& infos = world.handle_infos;
    auto previous_entities = std::move(m_prev_tick_entities);
    m_prev_tick_entities.clear();
    m_prev_tick_entities.reserve(infos.count());
    for (u32 id : infos.ids()) m_prev_tick_entities.insert(id);

    for (u32 peer_index = 0; peer_index < net.connected_peer_count(); ++peer_index) {
        u32 peer_id = net.connected_peer_id(peer_index);
        simulation::Player player = net.connected_peer_player(peer_index);
        if (!player.is_valid()) continue;
        auto& known = m_known_by_player[player.id];
            std::unordered_set<u32> visible;
        visible.reserve(infos.count());
        std::vector<UnitState> units;
        std::vector<ProjectileState> projectiles;
        units.reserve(infos.count());

        for (u32 i = 0; i < infos.count(); ++i) {
            u32 id = infos.ids()[i];
            const auto& info = infos.data()[i];
            if (info.category == simulation::Category::Doodad) continue;
            if (const auto* carried = world.carriables.get(id);
                carried && simulation::is_non_null_handle(carried->carried_by)) {
                continue;
            }
            if (!is_visible_to(id, player)) continue;
            if (const auto* projectile = world.projectiles.get(id);
                projectile && projectile->dying) {
                continue;
            }
            visible.insert(id);
            if (!known.contains(id)) {
                send_spawn(
                    net, peer_id, player, id,
                    !previous_entities.contains(id));
            }
            const auto* transform = world.transforms.get(id);
            if (!transform) continue;
            if (info.category == simulation::Category::Unit) {
                units.push_back(build_unit_state(world, id));
            } else if (info.category == simulation::Category::Projectile) {
                projectiles.push_back(ProjectileState{
                    id, transform->position.x, transform->position.y,
                    transform->position.z, transform->facing});
            }
        }

        std::vector<u32> forgotten;
        for (u32 id : known) {
            if (visible.contains(id)) continue;
            const auto* carried = world.carriables.get(id);
            const auto* projectile = world.projectiles.get(id);
            bool silent =
                (carried && simulation::is_non_null_handle(carried->carried_by)) ||
                (projectile && projectile->dying);
            if (!silent) {
                const auto* info = infos.get(id);
                net.send_to_peer(
                    peer_id,
                    !info || info->hidden ? build_destroy(id) : build_hide(id));
            }
            forgotten.push_back(id);
        }
        for (u32 id : forgotten) known.erase(id);

        if (!units.empty()) {
            net.send_to_peer(peer_id, build_unit_state(tick, units), false);
        }
        if (!projectiles.empty()) {
            net.send_to_peer(
                peer_id, build_projectile_state(tick, projectiles), false);
        }
    }
}

void GameServer::broadcast_update(NetworkManager& net, u32 entity_id,
                                  std::span<const u8> packet) {
    const auto& world = m_simulation.world();
    if (!world.handle_infos.has(entity_id)) return;
    u32 visibility_id = entity_id;
    if (const auto* carried = world.carriables.get(entity_id);
        carried && simulation::is_non_null_handle(carried->carried_by)) {
        visibility_id = carried->carried_by.id;
    }
    for (const auto& [player_id, known] : m_known_by_player) {
        if (known.contains(visibility_id)) {
            net.host_send_to_player(player_id, packet);
        }
    }
}

void GameServer::broadcast_entity_event(NetworkManager& net, u32 entity_id,
                                        std::span<const u8> packet) {
    for (const auto& [player_id, known] : m_known_by_player) {
        if (known.contains(entity_id)) {
            net.host_send_to_player(player_id, packet);
        }
    }
}

void GameServer::replay_persistent_state(NetworkManager& net, u32 peer_id,
                                         simulation::Player player) {
    if (m_hud_replay) {
        u32 player_bit = 1u << player.id;
        m_hud_replay->emit_state_to(
            [&](const std::vector<u8>& packet, u32 mask) {
                if (mask & player_bit) net.send_to_peer(peer_id, packet);
            });
    }
    if (auto controlled = m_controlled_unit_by_player.find(player.id);
        controlled != m_controlled_unit_by_player.end()) {
        net.send_to_peer(peer_id, build_set_controlled_unit(controlled->second));
    }
}

void GameServer::clear_replication() {
    m_known_by_player.clear();
    m_prev_tick_entities.clear();
}

void GameServer::clear_replication(simulation::Player player) {
    m_known_by_player.erase(player.id);
}

void GameServer::wire_to_network(NetworkManager& net) {
    auto& world = m_simulation.world();

    // ── Script → client sends ────────────────────────────────────────────
    m_script.set_unit_update_fn([this, &net](u32 entity_id, const std::vector<u8>& pkt) {
        broadcast_update(net,entity_id, pkt);
    });
    m_script.set_anim_event_fn([this, &net](u32 entity_id, const std::vector<u8>& pkt) {
        broadcast_entity_event(net,entity_id, pkt);
    });
    m_script.set_broadcast_fn([this, &net](const std::vector<u8>& pkt) {
        net.host_broadcast(pkt);
    });

    // Fog-aware effect deliver / destroy — SEND HALF ONLY. The scan in
    // ScriptEngine::update fires this per (player, effect) once that player can
    // see it. Here we send to whichever player it's for; host_send_to_player is
    // a no-op for the host's own slot (it has no peer), so the host chains its
    // renderer apply for the local slot ON TOP of this without double-sending.
    m_script.set_effect_deliver_fn(
        [this, &net](u32 player_id, u32 server_id, std::string_view name,
               simulation::Unit entity, glm::vec3 pos, std::string_view attach_point) {
            auto pkt = build_effect_create(server_id, name, entity, pos, attach_point);
            net.host_send_to_player(player_id, pkt);
        });
    m_script.set_effect_destroy_fn([this, &net](u32 player_id, u32 server_id) {
        auto pkt = build_effect_destroy(server_id);
        net.host_send_to_player(player_id, pkt);
    });

    // Scripted-camera commands are sent per player in the Lua call's players_mask.
    // The host chains its own local apply on top (see Engine::wire_host_broadcasts).
    if (net.mode() == Mode::Host) {
        m_script.set_camera_apply_setup_fn(
            [this, &net](u32 mask, f32 tx, f32 ty, f32 tz, f32 dist,
                   f32 pitch_rad, f32 yaw_rad, f32 dur) {
                auto packet = build_camera_apply_setup(
                    tx, ty, tz, dist, pitch_rad, yaw_rad, dur);
                for (u32 p = 0; p < 32; ++p) {
                    if (mask & (1u << p)) net.host_send_to_player(p, packet);
                }
            });
        m_script.set_camera_set_target_position_fn(
            [this, &net](u32 mask, f32 x, f32 y, f32 z, f32 dur) {
                auto packet = build_camera_set_target_position(x, y, z, dur);
                for (u32 p = 0; p < 32; ++p) {
                    if (mask & (1u << p)) net.host_send_to_player(p, packet);
                }
            });
        m_script.set_camera_set_source_distance_fn(
            [this, &net](u32 mask, f32 dist, f32 dur) {
                auto packet = build_camera_set_source_distance(dist, dur);
                for (u32 p = 0; p < 32; ++p) {
                    if (mask & (1u << p)) net.host_send_to_player(p, packet);
                }
            });
        m_script.set_camera_shake_fn(
            [this, &net](u32 mask, f32 intensity, f32 dur) {
                auto packet = build_camera_shake(intensity, dur);
                for (u32 p = 0; p < 32; ++p) {
                    if (mask & (1u << p)) net.host_send_to_player(p, packet);
                }
            });
        m_script.set_camera_set_target_controller_fn(
            [this, &net](u32 mask, simulation::Unit unit) {
                auto packet = build_camera_set_target_controller(unit.id);
                for (u32 p = 0; p < 32; ++p) {
                    if (mask & (1u << p)) net.host_send_to_player(p, packet);
                }
            });

        m_script.set_set_controlled_unit_fn(
            [this, &net](u32 mask, simulation::Unit unit) {
                auto packet = build_set_controlled_unit(unit.id);
                for (u32 p = 0; p < 32; ++p) {
                    if (!(mask & (1u << p))) continue;
                    if (unit.id == UINT32_MAX) m_controlled_unit_by_player.erase(p);
                    else m_controlled_unit_by_player[p] = unit.id;
                    net.host_send_to_player(p, packet);
                }
            });
    }

    // EndGame(winning_team, stats) records the authoritative result and sends
    // S_END to clients in Host mode.
    m_script.set_end_game_fn([this, &net](u32 winning_team, std::string_view stats) {
        net.host_end_game(winning_team, stats);
    });

    // ── World → client sends ─────────────────────────────────────────────
    world.on_attack_start =
        [this, &net](simulation::Unit attacker, simulation::Widget target,
               f32 windup, f32 backswing, f32 damage_point) {
            auto pkt = build_anim_attack_start(
                attacker.id, target.id, windup, backswing, damage_point);
            broadcast_entity_event(net,attacker.id, pkt);
        };
    world.on_ability_added =
        [this, &net](simulation::Unit unit, std::string_view ability_id, u32 level,
               const simulation::AbilitySource& source) {
            auto pkt = build_cold_ability_add(
                unit.id, ability_id, level,
                static_cast<u8>(simulation::ability_source_kind(source)));
            broadcast_update(net,unit.id, pkt);
        };
    world.on_ability_removed =
        [this, &net](simulation::Unit unit, std::string_view ability_id,
               const simulation::AbilitySource& source, bool all_instances) {
            auto pkt = build_cold_ability_remove(
                unit.id, ability_id,
                static_cast<u8>(simulation::ability_source_kind(source)),
                all_instances);
            broadcast_update(net,unit.id, pkt);
        };
    world.on_ability_cooldown_started =
        [this, &net](simulation::Unit unit, std::string_view ability_id, f32 seconds) {
            auto pkt = build_cold_cooldown(unit.id, ability_id, seconds);
            broadcast_update(net,unit.id, pkt);
        };
    world.on_item_charges_changed =
        [this, &net](simulation::Item item, i32 charges) {
            auto pkt = build_cold_item_charges(item.id, charges);
            broadcast_update(net,item.id, pkt);
        };
    // Death signal for entities off the per-tick path (destructables): they don't
    // ride S_UNIT_STATE, so a dying crate's health→0 needs its own on-change
    // S_COLD{Health}. The client derives death from health (health_is_dead), so
    // this is what makes it render a corpse. Units carry health in S_UNIT_STATE and
    // don't need this. host_broadcast_update routes only to peers that already know
    // the entity (same visibility gate as other on-change updates).
    world.on_entity_died =
        [this, &net, &world](u32 entity_id) {
            const auto* h = world.healths.get(entity_id);
            f32 cur = h ? h->current : 0.0f;
            f32 mx  = h ? h->max     : 1.0f;
            auto pkt = build_cold_health(entity_id, cur, mx);
            broadcast_update(net,entity_id, pkt);
        };
    {
        auto script_revived = std::move(world.on_unit_revived);
        world.on_unit_revived =
            [this, &net, &world, script_revived = std::move(script_revived)](
                simulation::Unit unit) {
                if (script_revived) script_revived(unit);
                if (!world.contains(unit)) return;
                const auto* health = world.healths.get(unit.id);
                const auto* transform = world.transforms.get(unit.id);
                if (!health || !transform) return;
                broadcast_update(net,
                    unit.id, build_cold_health(unit.id, health->current, health->max));
                broadcast_update(net,
                    unit.id, build_cold_transform(
                        unit.id, transform->position.x, transform->position.y,
                        transform->position.z, transform->facing));
            };
    }

    // Inventory pickup / drop → S_COLD. CHAIN onto the script's trigger
    // dispatch (init_game installed on_item_picked_up/_dropped to fire the
    // EVENT_*_ITEM_PICKED_UP/_DROPPED triggers) — capture + call it first, then
    // add the network sync. Overwriting would silently kill the map's item
    // triggers (e.g. rune-pickup VFX).
    {
        auto script_picked_up = std::move(world.on_item_picked_up);
        world.on_item_picked_up =
            [this, &net, script_picked_up = std::move(script_picked_up)](
                simulation::Unit unit, simulation::Item item, i32 slot) {
                if (script_picked_up) script_picked_up(unit, item, slot);
                if (slot < 0) return;  // powerup: no slot to sync
                auto pkt = build_cold_inventory(
                    unit.id, static_cast<u32>(slot), item.id);
                broadcast_update(net,unit.id, pkt);
            };
    }
    {
        auto script_dropped = std::move(world.on_item_dropped);
        world.on_item_dropped =
            [this, &net, script_dropped = std::move(script_dropped)](
                simulation::Unit unit, simulation::Item item) {
                if (script_dropped) script_dropped(unit, item);
                // Carry the item's ground position — drop_item_from_unit already
                // set transform.position to the drop point before this fires.
                // Items left the per-tick snapshot, so this event is their only
                // position sync; without it the client re-shows the item at its
                // stale spawn transform.
                glm::vec3 pos{0};
                if (const auto* t = m_simulation.world().transforms.get(item.id)) {
                    pos = t->position;
                }
                auto pkt = build_cold_inventory(unit.id, UINT32_MAX, item.id,
                                                  pos.x, pos.y, pos.z);
                broadcast_update(net,unit.id, pkt);
            };
    }

    // Construction start / finish → S_COLD, chained onto the script callbacks
    // (init_game installed on_construction_* to fire the map's triggers). Start
    // syncs the under-construction state + progress so the client plays the
    // time-scaled birth clip; finish clears it so the client transitions to Idle.
    {
        auto script_start = std::move(world.on_construction_start);
        world.on_construction_start =
            [this, &net, script_start = std::move(script_start)](
                simulation::Unit structure, simulation::Unit builder) {
                if (script_start) script_start(structure, builder);
                const auto* c = m_simulation.world().constructions.get(structure.id);
                if (!c) return;
                auto pkt = build_cold_construction(structure.id, c->under_construction,
                                                   c->build_time_total, c->build_progress);
                broadcast_update(net,structure.id, pkt);
            };
    }
    {
        auto script_finish = std::move(world.on_construction_finish);
        world.on_construction_finish =
            [this, &net, script_finish = std::move(script_finish)](
                simulation::Unit structure, simulation::Unit builder) {
                if (script_finish) script_finish(structure, builder);
                // under_construction=false → client stops the stretched birth clip.
                auto pkt = build_cold_construction(structure.id, false, 0.0f, 1.0f);
                broadcast_update(net,structure.id, pkt);
            };
    }

}

} // namespace uldum::network
