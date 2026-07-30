#include "network/game_server.h"
#include "network/network.h"
#include "network/protocol.h"
#include "simulation/vision.h"
#include "asset/asset.h"
#include "map/map.h"
#include "core/log.h"

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
    // Alliances from manifest. Array index is the player id — there's no
    // separate slot number field.
    {
        auto& manifest = map.manifest();
        const u32 n = static_cast<u32>(manifest.players.size());
        m_simulation.init_alliances(n);
        for (u32 a = 0; a < n; ++a) {
            for (u32 b = 0; b < n; ++b) {
                if (a == b) continue;
                const auto& pa = manifest.players[a];
                const auto& pb = manifest.players[b];
                if (pa.team != pb.team) continue;
                for (auto& team : manifest.teams) {
                    if (team.id == pa.team && team.allied) {
                        m_simulation.set_alliance(
                            simulation::Player{a},
                            simulation::Player{b},
                            true);
                        if (team.shared_vision) {
                            m_simulation.set_shared_vision(
                                simulation::Player{a},
                                simulation::Player{b},
                                true);
                        }
                    }
                }
            }
        }
        log::info(TAG, "Alliances initialized — {} players", manifest.players.size());
    }

    // Terrain
    if (map.terrain().is_valid()) {
        m_simulation.set_terrain(&map.terrain());
    }

    // Fog of war
    {
        auto& manifest = map.manifest();
        auto& terrain = map.terrain();
        simulation::FogMode fog_mode = simulation::FogMode::None;
        if (manifest.fog_of_war == "explored") fog_mode = simulation::FogMode::Explored;
        else if (manifest.fog_of_war == "unexplored") fog_mode = simulation::FogMode::Unexplored;

        m_simulation.vision().init(
            terrain.tiles_x, terrain.tiles_y, terrain.tile_size,
            static_cast<u32>(manifest.players.size()), fog_mode, &terrain);

        if (fog_mode != simulation::FogMode::None) {
            log::info(TAG, "Fog of war enabled (mode: {})", manifest.fog_of_war);
        }
    }

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

u32 GameServer::switch_scene(map::MapManager& map, asset::AssetManager& assets,
                             std::string_view scene_name, PreMainHook pre_main) {
    log::info(TAG, "Scene switch → '{}'", scene_name);

    // Wipe the old scene's entities + swap terrain data (allocator resets to 0).
    m_simulation.world().clear_entities();
    if (!map.switch_scene_terrain_only(scene_name, assets)) {
        log::error(TAG, "switch_scene: terrain swap failed for '{}'", scene_name);
        return UINT32_MAX;
    }
    if (map.terrain().is_valid()) {
        m_simulation.set_terrain(&map.terrain());
    }
    // Instantiate the new scene's placements — must exist before the spawn
    // burst the caller fires afterward.
    simulation::apply_scene_data(m_simulation, map.mutable_scene());
    u32 new_boundary = m_simulation.world().entities.next_id();

    // Full Lua VM reset — per the design contract, Lua state does not survive a
    // scene swap; maps carry data across scenes via SaveData / LoadData.
    m_script.shutdown();
    if (!m_script.init(m_simulation, map, m_audio)) {
        log::error(TAG, "switch_scene: ScriptEngine re-init failed for '{}'", scene_name);
        return UINT32_MAX;
    }
    if (!run_scene_scripts(map, scene_name, pre_main)) {
        return UINT32_MAX;
    }
    return new_boundary;
}

void GameServer::begin_scene_switch(NetworkManager& net,
                                    std::string_view scene_name,
                                    const TeardownHook& local_teardown) {
    // Tell clients first (reliable-ordered ENet guarantees they process this
    // before any later S_SPAWN / S_HUD_* delta), then tear down the caller's own
    // scene state, then mark self loaded so the barrier closes once peers ack.
    std::string scene(scene_name);
    net.host_broadcast_scene_switch(scene);
    if (local_teardown) local_teardown(scene);
    m_pending_finalize_scene = scene;
    net.mark_self_loaded();
}

bool GameServer::try_finish_scene_switch(NetworkManager& net, map::MapManager& map,
                                         asset::AssetManager& assets,
                                         const PreMainHook& pre_main) {
    if (m_pending_finalize_scene.empty() || !net.is_scene_switching() ||
        !net.all_peers_loaded()) {
        return false;
    }
    std::string scene = std::move(m_pending_finalize_scene);
    m_pending_finalize_scene.clear();

    u32 boundary = switch_scene(map, assets, scene, pre_main);
    if (boundary != UINT32_MAX) {
        net.set_placement_count(boundary);
    }
    net.host_finish_scene_switch();
    log::info(TAG, "Scene switch '{}' complete — sim resuming", scene);
    return true;
}

void GameServer::shutdown() {
    m_pending_finalize_scene.clear();
    m_script.shutdown();
    m_simulation.shutdown();
    log::info(TAG, "GameServer shut down");
}

void GameServer::tick(f32 dt) {
    m_simulation.tick(dt);
    m_script.update(dt);
}

void GameServer::wire_to_network(NetworkManager& net) {
    auto& world = m_simulation.world();

    // ── Script → client sends ────────────────────────────────────────────
    m_script.set_unit_update_fn([&net](u32 entity_id, const std::vector<u8>& pkt) {
        net.host_broadcast_update(entity_id, pkt);
    });
    m_script.set_broadcast_fn([&net](const std::vector<u8>& pkt) {
        net.host_broadcast(pkt);
    });

    // Fog-aware effect deliver / destroy — SEND HALF ONLY. The scan in
    // ScriptEngine::update fires this per (player, effect) once that player can
    // see it. Here we send to whichever player it's for; host_send_to_player is
    // a no-op for the host's own slot (it has no peer), so the host chains its
    // renderer apply for the local slot ON TOP of this without double-sending.
    m_script.set_effect_deliver_fn(
        [&net](u32 player_id, u32 server_id, std::string_view name,
               simulation::Unit entity, glm::vec3 pos, std::string_view attach_point) {
            auto pkt = build_effect_create(server_id, name, entity, pos, attach_point);
            net.host_send_to_player(player_id, pkt);
        });
    m_script.set_effect_destroy_fn([&net](u32 player_id, u32 server_id) {
        auto pkt = build_effect_destroy(server_id);
        net.host_send_to_player(player_id, pkt);
    });

    // Scripted-camera commands (CameraSetTargetPosition, ApplySetup, source
    // distance, shake, target-controller) — SEND HALF ONLY, per player in the Lua
    // call's players_mask. host_send_camera_* no-ops for the host's own slot and
    // off-Host, so it's safe on host/worker/offline; the host chains its own local
    // apply on top (see Engine::wire_host_broadcasts).
    m_script.set_camera_apply_setup_fn(
        [&net](u32 mask, f32 tx, f32 ty, f32 tz, f32 dist,
               f32 pitch_rad, f32 yaw_rad, f32 dur) {
            for (u32 p = 0; p < 32; ++p) {
                if (mask & (1u << p))
                    net.host_send_camera_apply_setup(p, tx, ty, tz, dist,
                                                     pitch_rad, yaw_rad, dur);
            }
        });
    m_script.set_camera_set_target_position_fn(
        [&net](u32 mask, f32 x, f32 y, f32 z, f32 dur) {
            for (u32 p = 0; p < 32; ++p) {
                if (mask & (1u << p))
                    net.host_send_camera_set_target_position(p, x, y, z, dur);
            }
        });
    m_script.set_camera_set_source_distance_fn(
        [&net](u32 mask, f32 dist, f32 dur) {
            for (u32 p = 0; p < 32; ++p) {
                if (mask & (1u << p))
                    net.host_send_camera_set_source_distance(p, dist, dur);
            }
        });
    m_script.set_camera_shake_fn(
        [&net](u32 mask, f32 intensity, f32 dur) {
            for (u32 p = 0; p < 32; ++p) {
                if (mask & (1u << p))
                    net.host_send_camera_shake(p, intensity, dur);
            }
        });
    m_script.set_camera_set_target_controller_fn(
        [&net](u32 mask, simulation::Unit unit) {
            for (u32 p = 0; p < 32; ++p) {
                if (mask & (1u << p))
                    net.host_send_camera_set_target_controller(p, unit.id);
            }
        });

    // SetControlledUnit(unit) → S_SET_CONTROLLED_UNIT. SEND HALF: route to the
    // owner player only (mask is 1<<owner). host_send_set_controlled_unit stores
    // the id per player for join-replay AND sends to that peer now; no-ops for
    // the host's own slot and when not in Host mode. The host chains its own
    // App-selection apply for the local slot on top (wire_host_broadcasts).
    m_script.set_set_controlled_unit_fn(
        [&net](u32 mask, simulation::Unit unit) {
            for (u32 p = 0; p < 32; ++p) {
                if (mask & (1u << p))
                    net.host_send_set_controlled_unit(p, unit.id);
            }
        });

    // EndGame(winning_team, stats) → S_END. SEND HALF: the host also chains a
    // local Results transition on top.
    m_script.set_end_game_fn([&net](u32 winning_team, std::string_view stats) {
        net.host_end_game(winning_team, stats);
    });

    // ── World → client sends ─────────────────────────────────────────────
    world.on_ability_added =
        [&net](simulation::Unit unit, std::string_view ability_id, u32 level,
               const simulation::AbilitySource& source) {
            auto pkt = build_cold_ability_add(
                unit.id, ability_id, level,
                static_cast<u8>(simulation::ability_source_kind(source)));
            net.host_broadcast_update(unit.id, pkt);
        };
    world.on_ability_removed =
        [&net](simulation::Unit unit, std::string_view ability_id,
               const simulation::AbilitySource& source, bool all_instances) {
            auto pkt = build_cold_ability_remove(
                unit.id, ability_id,
                static_cast<u8>(simulation::ability_source_kind(source)),
                all_instances);
            net.host_broadcast_update(unit.id, pkt);
        };
    world.on_ability_cooldown_started =
        [&net](simulation::Unit unit, std::string_view ability_id, f32 seconds) {
            auto pkt = build_cold_cooldown(unit.id, ability_id, seconds);
            net.host_broadcast_update(unit.id, pkt);
        };
    world.on_item_charges_changed =
        [&net](simulation::Item item, i32 charges) {
            auto pkt = build_cold_item_charges(item.id, charges);
            net.host_broadcast_update(item.id, pkt);
        };
    // Death signal for entities off the per-tick path (destructables): they don't
    // ride S_UNIT_STATE, so a dying crate's health→0 needs its own on-change
    // S_COLD{Health}. The client derives death from health (health_is_dead), so
    // this is what makes it render a corpse. Units carry health in S_UNIT_STATE and
    // don't need this. host_broadcast_update routes only to peers that already know
    // the entity (same visibility gate as other on-change updates).
    world.on_entity_died =
        [&net, &world](u32 entity_id) {
            const auto* h = world.healths.get(entity_id);
            f32 cur = h ? h->current : 0.0f;
            f32 mx  = h ? h->max     : 1.0f;
            auto pkt = build_cold_health(entity_id, cur, mx);
            net.host_broadcast_update(entity_id, pkt);
        };

    // Inventory pickup / drop → S_COLD. CHAIN onto the script's trigger
    // dispatch (init_game installed on_item_picked_up/_dropped to fire the
    // EVENT_*_ITEM_PICKED_UP/_DROPPED triggers) — capture + call it first, then
    // add the network sync. Overwriting would silently kill the map's item
    // triggers (e.g. rune-pickup VFX).
    {
        auto script_picked_up = std::move(world.on_item_picked_up);
        world.on_item_picked_up =
            [&net, script_picked_up = std::move(script_picked_up)](
                simulation::Unit unit, simulation::Item item, i32 slot) {
                if (script_picked_up) script_picked_up(unit, item, slot);
                if (slot < 0) return;  // powerup: no slot to sync
                auto pkt = build_cold_inventory(
                    unit.id, static_cast<u32>(slot), item.id);
                net.host_broadcast_update(unit.id, pkt);
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
                net.host_broadcast_update(unit.id, pkt);
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
                net.host_broadcast_update(structure.id, pkt);
            };
    }
    {
        auto script_finish = std::move(world.on_construction_finish);
        world.on_construction_finish =
            [&net, script_finish = std::move(script_finish)](
                simulation::Unit structure, simulation::Unit builder) {
                if (script_finish) script_finish(structure, builder);
                // under_construction=false → client stops the stretched birth clip.
                auto pkt = build_cold_construction(structure.id, false, 0.0f, 1.0f);
                net.host_broadcast_update(structure.id, pkt);
            };
    }

    net.on_player_disconnected = [this](u32 player_id) {
        m_script.fire_player_event("global_disconnect", player_id);
        m_script.fire_player_event("player_disconnect", player_id);
    };
    net.on_player_dropped = [this](u32 player_id) {
        m_script.fire_player_event("global_leave", player_id);
        m_script.fire_player_event("player_leave", player_id);
    };
}

} // namespace uldum::network
