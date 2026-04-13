#include "app/engine.h"
#include "core/log.h"

#include <chrono>
#include <functional>

namespace uldum {

static constexpr const char* TAG = "Engine";
static constexpr float TICK_RATE = 32.0f;  // real-time ticks per second (always constant)
static constexpr float TICK_DT  = 1.0f / TICK_RATE;  // real-time interval between ticks

// Simple FNV-1a hash for map identity
static u32 hash_string(std::string_view s) {
    u32 h = 2166136261u;
    for (char c : s) { h ^= static_cast<u8>(c); h *= 16777619u; }
    return h;
}

simulation::World& Engine::active_world() {
    if (m_args.net_mode == network::Mode::Client)
        return m_network.client_world();
    return m_server.simulation().world();
}

bool Engine::init(const LaunchArgs& args) {
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

    if (!m_map.load_map("maps/test_map.uldmap", m_asset, m_server.simulation())) {
        log::error(TAG, "Failed to load test map");
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

    // Feed terrain data to renderer
    if (m_map.terrain().is_valid()) {
        m_renderer.set_terrain(m_map.terrain());
        m_renderer.set_terrain_data(&m_map.terrain());
    }

    bool is_client = (args.net_mode == network::Mode::Client);

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
    switch (args.net_mode) {
    case network::Mode::Offline:
        m_network.init_offline();
        break;
    case network::Mode::Host:
        if (!m_network.init_host(args.port, 8, m_server.simulation(), m_commands)) {
            log::error(TAG, "Failed to start host on port {}", args.port);
            return false;
        }
        break;
    case network::Mode::Client:
        if (!m_network.init_client(args.connect_address, args.port)) {
            log::error(TAG, "Failed to connect to {}:{}", args.connect_address, args.port);
            return false;
        }
        m_network.set_type_registry(&m_server.simulation().types());
        m_network.init_client_fog(m_map.terrain(), m_map, m_server.simulation());
        break;
    }

    // Wire client-side callbacks
    if (!is_client) {
        m_server.simulation().world().on_sound = [this](std::string_view path, glm::vec3 pos) {
            m_audio.play_sfx(path, pos);
            // Host: also forward sounds to remote clients
            if (m_args.net_mode == network::Mode::Host) {
                auto msg = network::build_sound(path, pos);
                // broadcast is handled by transport
                // (for now, sounds are sent as S_SOUND to all peers)
            }
        };
        m_server.script().set_attach_point_fn([this](u32 entity_id, std::string_view bone) {
            return m_renderer.get_attachment_point(entity_id, bone);
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

    log::info(TAG, "=== All modules initialized ===");
    return true;
}

void Engine::run() {
    log::info(TAG, "Entering main loop (tick rate: {} Hz, dt: {:.4f}s)", TICK_RATE, TICK_DT);

    auto previous_time = std::chrono::high_resolution_clock::now();
    float accumulator = 0.0f;
    float game_speed = 1.0f;     // multiplier: 0 = paused, 1 = normal, 2 = fast
    float game_time = 0.0f;      // in-game elapsed time (affected by game_speed)
    u32 frame_count = 0;
    u32 tick_counter = 0;
    bool is_client = (m_args.net_mode == network::Mode::Client);

    while (m_platform->poll_events()) {
        // Handle resize
        if (m_platform->was_resized()) {
            m_rhi.handle_resize(m_platform->width(), m_platform->height());
            f32 aspect = static_cast<f32>(m_platform->width()) / static_cast<f32>(m_platform->height());
            m_renderer.handle_resize(aspect);
            m_picker.set_screen_size(m_platform->width(), m_platform->height());
        }

        // Delta time
        auto current_time = std::chrono::high_resolution_clock::now();
        float frame_dt = std::chrono::duration<float>(current_time - previous_time).count();
        previous_time = current_time;

        // Cap delta to avoid spiral of death
        if (frame_dt > 0.25f) frame_dt = 0.25f;

        // Network: receive state / commands
        m_network.update();

        // Client: update local player after welcome
        if (is_client && m_network.is_connected()) {
            auto lp = m_network.local_player();
            if (lp.is_valid() && lp.id != m_selection.player().id) {
                m_selection.set_player(lp);
                log::info(TAG, "Local player set to {}", lp.id);
            }
        }

        // Fixed timestep simulation (offline/host only)
        if (!is_client) {
            float game_dt = TICK_DT * game_speed;
            accumulator += frame_dt;
            while (accumulator >= TICK_DT) {
                m_server.tick(game_dt);
                game_time += game_dt;
                tick_counter++;

                // Host: broadcast state to remote clients
                if (m_args.net_mode == network::Mode::Host) {
                    m_network.host_broadcast_tick(tick_counter);
                }

                accumulator -= TICK_DT;
            }
        }

        // Input preset: selection, orders, camera
        {
            input::InputContext ictx{
                m_platform->input(),
                m_selection,
                m_commands,
                m_picker,
                m_renderer.camera(),
                m_bindings,
                m_server.simulation(),
                m_platform->width(),
                m_platform->height()
            };
            m_input_preset->update(ictx, frame_dt);
        }

        // Audio — update listener from camera position
        {
            auto& cam = m_renderer.camera();
            glm::vec3 forward = cam.forward_dir();
            m_audio.set_listener(cam.position(), forward, glm::vec3{0, 0, 1});
        }
        m_audio.update();

        // Update fog of war visual interpolation and upload to renderer
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

        // Render (skip if window is minimized — extent would be zero)
        auto& world = active_world();
        f32 alpha = is_client ? 1.0f : (accumulator / TICK_DT);
        VkCommandBuffer cmd = m_rhi.begin_frame();
        if (cmd && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
            m_renderer.upload_fog(cmd);
            m_renderer.draw_shadows(cmd, world, alpha);
            m_rhi.begin_rendering();
            m_renderer.draw(cmd, m_rhi.extent(), world, alpha);
            m_rhi.end_frame();
        }

        frame_count++;
        if (frame_count == 1) {
            log::info(TAG, "First frame completed — all systems running");
        }
    }

    log::info(TAG, "Exiting main loop (ran {} frames)", frame_count);
}

void Engine::shutdown() {
    log::info(TAG, "=== Shutting down Uldum Engine ===");

    // Shutdown in reverse init order
    m_map.shutdown();
    m_network.shutdown();
    m_server.shutdown();
    m_audio.shutdown();
    m_renderer.shutdown();
    m_asset.shutdown();
    m_rhi.shutdown();
    m_platform->shutdown();

    log::info(TAG, "=== All modules shut down ===");
}

} // namespace uldum
