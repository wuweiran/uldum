#pragma once

#include "platform/platform.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "render/renderer.h"
#include "render/selection_circles.h"
#include "audio/audio.h"
#include "core/settings.h"
#include "network/game_server.h"
#include "network/network.h"
#include "network/lobby.h"
#include "input/command_system.h"
#include "input/selection.h"
#include "input/picking.h"
#include "input/input_preset.h"
#include "input/input_bindings.h"
#include "map/map.h"
#include "hud/hud.h"
#include "hud/world.h"

#include <memory>
#include <string>

// Forward-declare UI types that App owns via unique_ptr. App's destructor
// is defined out-of-line in app.cpp, so the header doesn't need complete
// types — this lets dev_console.h / ui/shell.h include app.h in turn
// without a cycle.
namespace uldum::shell { class Shell; }
namespace uldum { class DevConsole; }

namespace uldum {

enum class AppState {
    Menu,       // main menu / boot screen
    Lobby,      // manifest loaded, slot assignment in progress
    Loading,    // map content loading + waiting for all peers
    Playing,    // simulation running, rendering gameplay
    Results,    // game ended, showing stats
};

struct LaunchArgs {
    network::Mode net_mode = network::Mode::Offline;
    std::string connect_address;
    std::string map_path = "maps/test_map.uldmap";
    u16 port = 7777;
    // Which slot the local user occupies. Set by the lobby at Start time;
    // drives renderer / selection / fog viewer and GameServer slot claim.
    u32 local_slot = 0;
    // True when CLI flags (`--map`) asked to skip the menu and start a
    // session directly. Menu → Lobby → Loading → Playing runs without UI.
    bool auto_start = false;
};

class App {
public:
    App();
    ~App();  // out-of-line (app.cpp) so header only needs forward-decls
             // of DevConsole / Shell — see top of file.

    // One-time init of persistent engine subsystems (platform, RHI, renderer, audio, asset).
    bool init(const LaunchArgs& args = {});

    // Main loop — drives AppState transitions.
    void run();

    // One-time shutdown of persistent engine subsystems.
    void shutdown();

private:
    // ── Session lifecycle ────────────────────────────────────────────────
    // Enter Lobby: loads map (manifest, types, terrain, preplaced), brings
    // up the network (listener/connector for Host/Client, offline stub for
    // Offline), and seeds m_network.lobby_state() so the dev UI can show
    // slot assignments. Simulation is initialized but not yet "active"
    // (no tick, no input wiring, no S_START).
    bool enter_lobby();
    // Exit Lobby back to Menu without ever starting a session. Symmetric
    // to enter_lobby — tears down the map-side state only.
    void leave_lobby();
    // Start the game from the lobby. Preconditions: enter_lobby() succeeded
    // and m_network.lobby_state() is populated (all slots non-Open,
    // m_args.local_slot set).
    bool start_session();
    // Tear down per-session state. Safe to call start_session() again after.
    void end_session();

    // The World to render — server's world in offline/host, client world in client mode.
    simulation::World& active_world();

    // Poll safe-area insets from the platform and re-push to the HUD
    // only when they changed. Called per frame because Android's
    // GameActivity_getWindowInsets returns zeros until after the first
    // layout pass — insets "arrive" asynchronously, sometime after
    // onCreate. Push-on-change keeps the hot path free of pointless
    // HUD re-resolves on steady-state frames.
    void refresh_safe_insets();

    LaunchArgs m_args;
    AppState   m_state = AppState::Menu;

    // Set by Shell UI click handlers (e.g. Quit button) to break out of
    // the main loop. Platform's poll_events() returns false on OS-initiated
    // quit (window close); this covers in-app quit requests.
    bool m_wants_quit = false;

    // Most recent end-of-session elapsed time (seconds) pulled out of the
    // Lua stats JSON. Shown on the Results screen. Stays at 0 until the
    // first EndGame call.
    f32 m_last_elapsed_seconds = 0.0f;

    // Last safe-area insets we pushed to the HUD. Compared on each
    // refresh so the HUD doesn't re-resolve composites when nothing
    // changed.
    platform::Platform::SafeInsets m_last_pushed_insets{};

    // ── Persistent (survive across sessions) ────────────────────────────
    std::unique_ptr<platform::Platform> m_platform;
    rhi::VulkanRhi           m_rhi;
    asset::AssetManager      m_asset;
    render::Renderer         m_renderer;
    render::SelectionCircles m_selection_circles;
    audio::AudioEngine       m_audio;
    settings::Store          m_settings;
    hud::Hud                 m_hud;
    hud::WorldContext        m_hud_world_ctx;

#ifdef ULDUM_SHELL_UI
    // Game-build only. RmlUi-backed Shell UI (menus, game room, settings,
    // results). Created after RHI is up; torn down in shutdown.
    std::unique_ptr<shell::Shell> m_shell;
#endif

#ifdef ULDUM_DEV_UI
    // uldum_dev only. ImGui-based dev console — map picker, session controls.
    std::unique_ptr<DevConsole> m_dev_console;
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

    // Whether enter_lobby() has prepared a session. The LobbyState itself
    // lives on NetworkManager (authoritative for Host; mirrored for Client;
    // host-like local-only for Offline) so that wire traffic and local edits
    // both mutate the same struct.
    bool                     m_lobby_active = false;
};

} // namespace uldum
