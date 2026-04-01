#include "app/engine.h"
#include "core/log.h"

#include <algorithm>
#include <chrono>

namespace uldum {

static constexpr const char* TAG = "Engine";
static constexpr float TICK_RATE = 16.0f;  // ticks per second
static constexpr float TICK_DT  = 1.0f / TICK_RATE;

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

    // Scripting
    if (!m_script.init()) {
        log::error(TAG, "ScriptEngine init failed");
        return false;
    }

    // Simulation
    if (!m_simulation.init(m_asset)) {
        log::error(TAG, "Simulation init failed");
        return false;
    }

    // Test units created below, after map terrain is available

    // Network
    if (!m_network.init(m_simulation)) {
        log::error(TAG, "NetworkManager init failed");
        return false;
    }

    // Map
    if (!m_map.init()) {
        log::error(TAG, "MapManager init failed");
        return false;
    }

    // Feed terrain data to renderer
    if (m_map.terrain().is_valid()) {
        m_renderer.set_terrain(m_map.terrain());
    }

    // Create test units on the terrain surface
    {
        using namespace simulation;
        auto& world = m_simulation.world();
        auto& td = m_map.terrain();
        Player p1{0};

        // Helper: sample terrain height at world (x, y)
        auto sample_height = [&](f32 x, f32 y) -> f32 {
            if (!td.is_valid()) return 0.0f;
            u32 ix = static_cast<u32>(x / td.tile_size);
            u32 iy = static_cast<u32>(y / td.tile_size);
            ix = std::min(ix, td.tiles_x);
            iy = std::min(iy, td.tiles_y);
            return td.height_at(ix, iy);
        };

        // Place units close together so they overlap from the camera angle (tests depth)
        f32 x1 = 44.0f, y1 = 52.0f;
        f32 x2 = 46.0f, y2 = 54.0f;
        f32 x3 = 45.0f, y3 = 50.0f;

        auto footman  = create_unit(world, "footman",  p1, x1, y1);
        auto paladin  = create_unit(world, "paladin",  p1, x2, y2);
        auto barracks = create_unit(world, "barracks", p1, x3, y3);

        // Place on terrain surface
        if (footman.is_valid())  set_position(world, footman,  x1, y1);
        if (paladin.is_valid())  set_position(world, paladin,  x2, y2);
        if (barracks.is_valid()) set_position(world, barracks, x3, y3);

        // Set Z to terrain height
        if (footman.is_valid()) {
            auto* t = world.transforms.get(footman.id);
            if (t) t->position.z = sample_height(x1, y1);
        }
        if (paladin.is_valid()) {
            auto* t = world.transforms.get(paladin.id);
            if (t) t->position.z = sample_height(x2, y2);
        }
        if (barracks.is_valid()) {
            auto* t = world.transforms.get(barracks.id);
            if (t) t->position.z = sample_height(x3, y3);
        }

        if (footman.is_valid() && paladin.is_valid() && barracks.is_valid()) {
            log::info(TAG, "Test units created — footman(hp={}) paladin(hp={}, hero={}, lvl={}) barracks(hp={}, building={})",
                      get_health(world, footman),
                      get_health(world, paladin), is_hero(world, paladin), hero_get_level(world, paladin),
                      get_health(world, barracks), is_building(world, barracks));
        } else {
            log::error(TAG, "Test unit creation FAILED");
        }
    }

    // Editor
    if (!m_editor.init()) {
        log::error(TAG, "Editor init failed");
        return false;
    }

    log::info(TAG, "=== All modules initialized ===");
    return true;
}

void Engine::run() {
    log::info(TAG, "Entering main loop (tick rate: {} Hz, dt: {:.4f}s)", TICK_RATE, TICK_DT);

    auto previous_time = std::chrono::high_resolution_clock::now();
    float accumulator = 0.0f;
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

        // Fixed timestep simulation
        accumulator += frame_dt;
        while (accumulator >= TICK_DT) {
            m_simulation.tick(TICK_DT);
            m_script.update(TICK_DT);
            accumulator -= TICK_DT;
        }

        // Update camera
        m_renderer.update_camera(m_platform->input(), frame_dt);

        // Audio
        m_audio.update();

        // Editor
        m_editor.update();

        // Render
        VkCommandBuffer cmd = m_rhi.begin_frame();
        if (cmd) {
            m_renderer.draw_shadows(cmd, m_simulation.world());
            m_rhi.begin_rendering();
            m_renderer.draw(cmd, m_rhi.extent(), m_simulation.world());
            m_editor.render();
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
    m_editor.shutdown();
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
