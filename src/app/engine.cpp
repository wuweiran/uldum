#include "app/engine.h"
#include "core/log.h"

#include <chrono>

namespace uldum {

static constexpr const char* TAG = "Engine";
static constexpr float TICK_RATE = 32.0f;  // real-time ticks per second (always constant)
static constexpr float TICK_DT  = 1.0f / TICK_RATE;  // real-time interval between ticks

bool Engine::init() {
    log::info(TAG, "=== Initializing Uldum Engine ===");

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
    platform_config.width  = 1280;
    platform_config.height = 720;

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

    // Verify asset pipeline with test loads
    {
        auto tex = m_asset.load_texture("textures/test_2x2.png");
        if (!tex.is_valid()) log::error(TAG, "Test texture load FAILED");

        auto model = m_asset.load_model("models/test_triangle.gltf");
        if (!model.is_valid()) log::error(TAG, "Test model load FAILED");

        auto cfg = m_asset.load_config("config/engine.json");
        if (cfg.is_valid()) {
            auto* doc = m_asset.get(cfg);
            if (doc && doc->data.contains("engine")) {
                auto& eng = doc->data["engine"];
                log::info(TAG, "Config loaded — engine: {} v{}",
                          eng.value("name", "?"), eng.value("version", "?"));
            }
        } else {
            log::error(TAG, "Test config load FAILED");
        }

        log::info(TAG, "Asset pipeline verified — {} textures, {} models, {} configs loaded",
                  m_asset.texture_count(), m_asset.model_count(), m_asset.config_count());
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

    // Simulation
    if (!m_simulation.init(m_asset)) {
        log::error(TAG, "Simulation init failed");
        return false;
    }

    // Network
    if (!m_network.init(m_simulation)) {
        log::error(TAG, "NetworkManager init failed");
        return false;
    }

    // Map — loads types, terrain, and preplaced objects
    if (!m_map.init()) {
        log::error(TAG, "MapManager init failed");
        return false;
    }

    if (!m_map.load_map("maps/test_map.uldmap", m_asset, m_simulation)) {
        log::error(TAG, "Failed to load test map");
        return false;
    }

    // Initialize alliances from manifest teams
    {
        auto& manifest = m_map.manifest();
        m_simulation.init_alliances(static_cast<u32>(manifest.players.size()));
        // Players on the same team with allied=true are allies
        for (auto& pa : manifest.players) {
            for (auto& pb : manifest.players) {
                if (pa.slot == pb.slot) continue;
                if (pa.team == pb.team) {
                    // Find the team def to check if it's allied
                    for (auto& team : manifest.teams) {
                        if (team.id == pa.team && team.allied) {
                            m_simulation.set_alliance(
                                simulation::Player{pa.slot},
                                simulation::Player{pb.slot},
                                true);
                        }
                    }
                }
            }
        }
        log::info(TAG, "Alliances initialized — {} players", manifest.players.size());
    }

    // Feed terrain data to renderer and simulation
    if (m_map.terrain().is_valid()) {
        m_renderer.set_terrain(m_map.terrain());
        m_simulation.set_terrain(&m_map.terrain());
        m_renderer.set_pathfinder(&m_simulation.pathfinder());
    }

    // Scripting — init after map so we can load map scripts
    if (!m_script.init(m_simulation, m_map, &m_renderer.effect_registry(), &m_renderer.effect_manager())) {
        log::error(TAG, "ScriptEngine init failed");
        return false;
    }

    // Initialize spatial grid before scripts run (so GetUnitsInRange works in main())
    m_simulation.spatial_grid().update(m_simulation.world());

    // Load and run map scripts
    {
        std::string main_script = m_map.map_root() + "/shared/scripts/main.lua";
        m_script.load_script(main_script);
        m_script.call_function("main");
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

    while (m_platform->poll_events()) {
        // Input
        if (m_platform->input().key_escape) {
            break;
        }

        // Handle resize
        if (m_platform->was_resized()) {
            m_rhi.handle_resize(m_platform->width(), m_platform->height());
            f32 aspect = static_cast<f32>(m_platform->width()) / static_cast<f32>(m_platform->height());
            m_renderer.handle_resize(aspect);
        }

        // Delta time
        auto current_time = std::chrono::high_resolution_clock::now();
        float frame_dt = std::chrono::duration<float>(current_time - previous_time).count();
        previous_time = current_time;

        // Cap delta to avoid spiral of death
        if (frame_dt > 0.25f) frame_dt = 0.25f;

        // Network: receive state / commands
        m_network.update();

        // Fixed timestep simulation (32 real-time ticks/sec, game speed scales game_dt)
        float game_dt = TICK_DT * game_speed;  // how much game time passes per tick
        accumulator += frame_dt;
        while (accumulator >= TICK_DT) {
            m_simulation.tick(game_dt);
            m_script.update(game_dt);
            game_time += game_dt;
            accumulator -= TICK_DT;
        }

        // Update camera
        m_renderer.update_camera(m_platform->input(), frame_dt);

        // Audio
        m_audio.update();

        // Render (skip if window is minimized — extent would be zero)
        VkCommandBuffer cmd = m_rhi.begin_frame();
        if (cmd && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
            m_renderer.draw_shadows(cmd, m_simulation.world());
            m_rhi.begin_rendering();
            m_renderer.draw(cmd, m_rhi.extent(), m_simulation.world());
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
    m_simulation.shutdown();
    m_script.shutdown();
    m_audio.shutdown();
    m_renderer.shutdown();
    m_asset.shutdown();
    m_rhi.shutdown();
    m_platform->shutdown();

    log::info(TAG, "=== All modules shut down ===");
}

} // namespace uldum
