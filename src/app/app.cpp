#include "app/app.h"
#include "core/log.h"

#include <chrono>
#include <functional>

namespace uldum {

static constexpr const char* TAG = "App";
static constexpr float TICK_RATE = 32.0f;  // real-time ticks per second (always constant)
static constexpr float TICK_DT  = 1.0f / TICK_RATE;  // real-time interval between ticks

// Simple FNV-1a hash for map identity
static u32 hash_string(std::string_view s) {
    u32 h = 2166136261u;
    for (char c : s) { h ^= static_cast<u8>(c); h *= 16777619u; }
    return h;
}

simulation::World& App::active_world() {
    if (m_args.net_mode == network::Mode::Client)
        return m_network.client_world();
    return m_server.simulation().world();
}

bool App::init(const LaunchArgs& args) {
    m_args = args;
    log::info(TAG, "=== Initializing Uldum Engine ===");

    const char* mode_str = "Offline";
    if (args.net_mode == network::Mode::Host) mode_str = "Host";
    else if (args.net_mode == network::Mode::Client) mode_str = "Client";
    log::info(TAG, "Network mode: {}", mode_str);

#ifdef ULDUM_DEBUG
    log::set_level(log::Level::Trace);
    log::info(TAG, "Debug mode — verbose logging enabled");
#else
    log::set_level(log::Level::Info);
#endif

    // Platform
    m_platform = platform::Platform::create();
    platform::Config platform_config{};
    platform_config.title  = "Uldum Engine";
    platform_config.width  = 960;
    platform_config.height = 540;

    if (!m_platform->init(platform_config)) {
        log::error(TAG, "Platform init failed");
        return false;
    }

    // RHI
    rhi::Config rhi_config{};
#ifdef ULDUM_DEBUG
    rhi_config.enable_validation = true;
#else
    rhi_config.enable_validation = false;
#endif

    if (!m_rhi.init(rhi_config, *m_platform)) {
        log::error(TAG, "RHI init failed");
        return false;
    }

    // Asset manager
    if (!m_asset.init("engine")) {
        log::error(TAG, "AssetManager init failed");
        return false;
    }

    // Verify asset pipeline with config load
    {
        auto cfg = m_asset.load_config("config/engine.json");
        if (cfg.is_valid()) {
            auto* doc = m_asset.get(cfg);
            if (doc && doc->data.contains("engine")) {
                auto& eng = doc->data["engine"];
                log::info(TAG, "Config loaded — engine: {} v{}",
                          eng.value("name", "?"), eng.value("version", "?"));
            }
        } else {
            log::error(TAG, "Engine config load FAILED");
        }
    }

    // Renderer
    if (!m_renderer.init(m_rhi)) {
        log::error(TAG, "Renderer init failed");
        return false;
    }

    // Audio
    if (!m_audio.init()) {
        log::error(TAG, "AudioEngine init failed");
        return false;
    }

    log::info(TAG, "=== Engine subsystems initialized ===");
    return true;
}

// ── Per-session lifecycle ─────────────────────────────────────────────────

bool App::start_session() {
    log::info(TAG, "=== Starting session: {} ===", m_args.map_path);

    // Simulation init (all modes — client needs TypeRegistry for model lookups)
    if (!m_server.init_simulation(m_asset)) {
        log::error(TAG, "GameServer simulation init failed");
        return false;
    }

    // Map — loads types, terrain, and preplaced objects into the simulation
    if (!m_map.init()) {
        log::error(TAG, "MapManager init failed");
        return false;
    }

    if (!m_map.load_map(m_args.map_path, m_asset, m_server.simulation())) {
        log::error(TAG, "Failed to load map '{}'", m_args.map_path);
        return false;
    }

    u32 map_hash = hash_string(m_map.manifest().name + m_map.manifest().version);

    // Set map root for asset path resolution
    m_renderer.set_map_root(m_map.map_root());
    m_audio.set_map_root(m_map.map_root());

    // Load map-defined effects
    {
        std::string effects_path = m_map.map_root() + "/types/effects.json";
        m_renderer.effect_registry().load_from_json(effects_path);
    }

    // Set water layer IDs on terrain from tileset
    {
        std::vector<u8> shallow, deep;
        for (auto& layer : m_map.tileset().layers) {
            if (layer.type == map::LayerType::WaterShallow)
                shallow.push_back(static_cast<u8>(layer.id));
            else if (layer.type == map::LayerType::WaterDeep)
                deep.push_back(static_cast<u8>(layer.id));
        }
        m_map.terrain().set_water_layers(shallow, deep);
    }

    // Load tileset textures and feed terrain data to renderer
    m_renderer.load_tileset_textures(m_map.tileset());
    if (m_map.terrain().is_valid()) {
        m_renderer.set_terrain(m_map.terrain());
        m_renderer.set_terrain_data(&m_map.terrain());
    }

    bool is_client = (m_args.net_mode == network::Mode::Client);

    // Game server phase 2 (offline/host only — client doesn't run the simulation)
    if (!is_client) {
        if (!m_server.init_game(m_map,
                                &m_renderer.effect_registry(), &m_renderer.effect_manager(), &m_audio)) {
            log::error(TAG, "GameServer game init failed");
            return false;
        }
    }

    // Network
    m_network.set_map_hash(map_hash);
    switch (m_args.net_mode) {
    case network::Mode::Offline:
        m_network.init_offline();
        break;
    case network::Mode::Host: {
        if (!m_network.init_host(m_args.port, 8, m_server.simulation(), m_commands)) {
            log::error(TAG, "Failed to start host on port {}", m_args.port);
            return false;
        }
        u32 total_players = static_cast<u32>(m_map.manifest().players.size());
        m_network.set_expected_players(total_players > 1 ? total_players - 1 : 0);
        m_network.set_disconnect_timeout(m_map.manifest().disconnect_timeout);
        m_network.set_pause_on_disconnect(m_map.manifest().pause_on_disconnect);
        break;
    }
    case network::Mode::Client:
        if (!m_network.init_client(m_args.connect_address, m_args.port)) {
            log::error(TAG, "Failed to connect to {}:{}", m_args.connect_address, m_args.port);
            return false;
        }
        m_network.set_type_registry(&m_server.simulation().types());
        m_network.init_client_fog(m_map.terrain(), m_map, m_server.simulation());
        break;
    }

    // Wire callbacks
    if (!is_client) {
        m_server.simulation().world().on_sound = [this](std::string_view path, glm::vec3 pos) {
            m_audio.play_sfx(path, pos);
        };
        m_server.script().set_attach_point_fn([this](u32 entity_id, std::string_view bone) {
            return m_renderer.get_attachment_point(entity_id, bone);
        });
        m_network.on_player_disconnected = [this](u32 player_id) {
            m_server.script().fire_event("global_disconnect", UINT32_MAX, "", player_id);
            m_server.script().fire_event("player_disconnect", UINT32_MAX, "", player_id);
        };
        m_network.on_player_dropped = [this](u32 player_id) {
            m_server.script().fire_event("global_leave", UINT32_MAX, "", player_id);
            m_server.script().fire_event("player_leave", UINT32_MAX, "", player_id);
        };
        m_server.script().set_unit_update_fn([this](u32 entity_id, const std::vector<u8>& pkt) {
            m_network.host_broadcast_update(entity_id, pkt);
        });
        m_server.script().set_end_game_fn([this](u32 winner_id, std::string_view stats) {
            if (m_args.net_mode == network::Mode::Host) {
                m_network.host_end_game(winner_id, stats);
            }
            log::info(TAG, "Game ended — winner: player {}", winner_id);
            m_state = AppState::Results;
        });
    } else {
        m_network.on_sound = [this](std::string_view path, glm::vec3 pos) {
            m_audio.play_sfx(path, pos);
        };
    }

    // Input: command system, selection, picking, preset
    if (!is_client) {
        m_commands.init(&m_server.simulation().world());
    } else {
        m_commands.init(nullptr);  // no local world
        m_commands.set_network_send([this](const input::GameCommand& cmd) {
            m_network.send_order(cmd);
        });
    }

    u32 local_player_id = is_client ? UINT32_MAX : 0;  // client gets ID from S_WELCOME later
    m_selection.set_player(simulation::Player{local_player_id});

    m_picker.init(&m_renderer.camera(), &m_map.terrain(),
                  &active_world(),
                  m_platform->width(), m_platform->height());

    // Load input configuration from manifest
    m_input_preset = input::create_preset(m_map.manifest().input_preset);
    m_bindings.load(m_map.manifest().input_bindings_json);
    m_bindings.apply_defaults(input::rts_default_bindings());

    // Wire fog of war to renderer
    if (!is_client) {
        m_renderer.set_simulation(&m_server.simulation());
        m_renderer.set_local_player(0);
    } else {
        // Client: renderer doesn't need simulation ref for fog filtering
        // (server already filters entities by fog)
        m_renderer.set_simulation(nullptr);
    }

    // Wire input to script engine (offline/host only)
    if (!is_client) {
        m_server.script().set_input(&m_selection, &m_commands);
    }

    m_session_active = true;
    log::info(TAG, "=== Session started ===");
    return true;
}

void App::end_session() {
    if (!m_session_active) return;
    log::info(TAG, "=== Ending session ===");

    m_input_preset.reset();
    m_bindings = input::InputBindings{};
    m_commands = input::CommandSystem{};
    m_selection = input::SelectionState{};

    m_network.shutdown();
    m_map.shutdown();
    m_server.shutdown();

    m_renderer.set_simulation(nullptr);
    m_renderer.set_terrain_data(nullptr);
    m_renderer.set_fog_grid(nullptr, 0, 0);

    m_session_active = false;
    log::info(TAG, "=== Session ended ===");
}

// ── Main loop ─────────────────────────────────────────────────────────────

void App::run() {
    log::info(TAG, "Entering main loop (tick rate: {} Hz)", TICK_RATE);

    auto previous_time = std::chrono::high_resolution_clock::now();
    float accumulator = 0.0f;
    float game_speed = 1.0f;
    float game_time = 0.0f;
    u32 tick_counter = 0;

    m_state = AppState::Menu;
    log::info(TAG, "Entered Menu state");
    bool dev_auto_start = true;

    while (m_platform->poll_events()) {
        auto current_time = std::chrono::high_resolution_clock::now();
        float frame_dt = std::chrono::duration<float>(current_time - previous_time).count();
        previous_time = current_time;
        if (frame_dt > 0.25f) frame_dt = 0.25f;

        if (m_platform->was_resized()) {
            m_rhi.handle_resize(m_platform->width(), m_platform->height());
            f32 aspect = static_cast<f32>(m_platform->width()) / static_cast<f32>(m_platform->height());
            m_renderer.handle_resize(aspect);
            if (m_session_active)
                m_picker.set_screen_size(m_platform->width(), m_platform->height());
        }

        switch (m_state) {
        case AppState::Menu:
            if (dev_auto_start) {
                dev_auto_start = false;
                m_state = AppState::Loading;
                log::info(TAG, "Dev auto-start → Loading");
            }
            break;

        case AppState::Loading:
            if (start_session()) {
                m_state = AppState::Playing;
                accumulator = 0; game_time = 0; tick_counter = 0;
                log::info(TAG, "Session loaded → Playing");
            } else {
                log::error(TAG, "Session failed to start → Menu");
                m_state = AppState::Menu;
            }
            break;

        case AppState::Playing: {
            bool is_client = (m_args.net_mode == network::Mode::Client);

            m_network.update(frame_dt);

            // Client: handle server disconnect
            if (is_client && !m_network.is_connected() && m_network.local_player().is_valid()) {
                log::warn(TAG, "Lost connection to server");
                m_state = AppState::Results;
                break;
            }

            if (is_client && m_network.is_connected()) {
                auto lp = m_network.local_player();
                if (lp.is_valid() && lp.id != m_selection.player().id) {
                    m_selection.set_player(lp);
                    log::info(TAG, "Local player set to {}", lp.id);
                }
            }

            if (is_client && m_network.client_game_ended()) {
                m_state = AppState::Results;
                break;
            }

            bool should_tick = !is_client &&
                (m_args.net_mode == network::Mode::Offline || m_network.is_game_started()) &&
                !m_network.is_paused();
            if (should_tick) {
                float game_dt = TICK_DT * game_speed;
                accumulator += frame_dt;
                while (accumulator >= TICK_DT) {
                    m_server.tick(game_dt);
                    game_time += game_dt;
                    tick_counter++;
                    if (m_args.net_mode == network::Mode::Host)
                        m_network.host_broadcast_tick(tick_counter);
                    accumulator -= TICK_DT;
                }
            }

            {
                input::InputContext ictx{
                    m_platform->input(), m_selection, m_commands, m_picker,
                    m_renderer.camera(), m_bindings, m_server.simulation(),
                    m_platform->width(), m_platform->height()
                };
                m_input_preset->update(ictx, frame_dt);
            }

            {
                auto& cam = m_renderer.camera();
                m_audio.set_listener(cam.position(), cam.forward_dir(), glm::vec3{0, 0, 1});
            }
            m_audio.update();

            if (!is_client) {
                auto& fog = m_server.simulation().fog();
                if (fog.enabled()) {
                    const f32* visual = fog.update_visual(simulation::Player{0}, frame_dt);
                    m_renderer.set_fog_grid(visual, fog.tiles_x(), fog.tiles_y());
                }
            } else {
                const f32* visual = m_network.update_client_fog(frame_dt);
                if (visual) {
                    m_renderer.set_fog_grid(visual, m_network.client_fog().tiles_x(),
                                            m_network.client_fog().tiles_y());
                }
            }
            break;
        }

        case AppState::Results:
            log::info(TAG, "Session complete → ending session → Menu");
            end_session();
            m_state = AppState::Menu;
            break;
        }

        if (m_state == AppState::Playing && m_session_active) {
            auto& world = active_world();
            bool is_client = (m_args.net_mode == network::Mode::Client);
            f32 alpha = is_client ? 1.0f : (accumulator / TICK_DT);
            VkCommandBuffer cmd = m_rhi.begin_frame();
            if (cmd && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
                m_renderer.upload_fog(cmd);
                m_renderer.draw_shadows(cmd, world, alpha);
                m_rhi.begin_rendering();
                m_renderer.draw(cmd, m_rhi.extent(), world, alpha);
                m_rhi.end_frame();
            }
        }
        // TODO (Phase 16): Menu, Loading, Results screens render here
    }

    if (m_session_active) end_session();
    log::info(TAG, "Exiting main loop");
}

void App::shutdown() {
    log::info(TAG, "=== Shutting down engine subsystems ===");
    if (m_session_active) end_session();
    m_audio.shutdown();
    m_renderer.shutdown();
    m_asset.shutdown();
    m_rhi.shutdown();
    m_platform->shutdown();
    log::info(TAG, "=== All shut down ===");
}

} // namespace uldum
