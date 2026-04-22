#pragma once

#include "platform/platform.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "render/renderer.h"
#include "audio/audio.h"
#include "network/game_server.h"
#include "network/network.h"
#include "input/command_system.h"
#include "input/selection.h"
#include "input/picking.h"
#include "input/input_preset.h"
#include "input/input_bindings.h"
#include "map/map.h"

#include <memory>
#include <string>

#ifdef ULDUM_SHELL_UI
// Full include (not just forward-decl): game_main.cpp instantiates App with
// an implicit destructor, which needs the complete ui::Shell type for
// std::unique_ptr's deleter. shell.h is cheap — it forward-declares RmlUi
// types, so no RmlUi headers leak into app.h.
#include "ui/shell.h"
#endif

namespace uldum {

enum class AppState {
    Menu,       // waiting for session to start (dev: auto-starts)
    Loading,    // loading map, creating game session
    Playing,    // simulation running, rendering gameplay
    Results,    // game ended, showing stats
};

struct LaunchArgs {
    network::Mode net_mode = network::Mode::Offline;
    std::string connect_address;
    std::string map_path = "maps/test_map.uldmap";
    u16 port = 7777;
};

class App {
public:
    App() = default;
    ~App() = default;

    // One-time init of persistent engine subsystems (platform, RHI, renderer, audio, asset).
    bool init(const LaunchArgs& args = {});

    // Main loop — drives AppState transitions.
    void run();

    // One-time shutdown of persistent engine subsystems.
    void shutdown();

private:
    // ── Session lifecycle ────────────────────────────────────────────────
    // Per-game: load map, create GameServer + Network, wire callbacks.
    bool start_session();
    // Tear down per-session state. Safe to call start_session() again after.
    void end_session();

    // The World to render — server's world in offline/host, client world in client mode.
    simulation::World& active_world();

    LaunchArgs m_args;
    AppState   m_state = AppState::Menu;

    // ── Persistent (survive across sessions) ────────────────────────────
    std::unique_ptr<platform::Platform> m_platform;
    rhi::VulkanRhi           m_rhi;
    asset::AssetManager      m_asset;
    render::Renderer         m_renderer;
    audio::AudioEngine       m_audio;

#ifdef ULDUM_SHELL_UI
    // Game-build only. RmlUi-backed Shell UI (menus, game room, settings,
    // results). Created after RHI is up; torn down in shutdown.
    std::unique_ptr<ui::Shell> m_shell;
#endif

    // ── Per-session (created in start_session, destroyed in end_session) ─
    network::GameServer      m_server;
    network::NetworkManager  m_network;
    input::CommandSystem     m_commands;
    input::SelectionState    m_selection;
    input::Picker            m_picker;
    input::InputBindings     m_bindings;
    std::unique_ptr<input::InputPreset> m_input_preset;
    map::MapManager          m_map;
    bool                     m_session_active = false;
};

} // namespace uldum
