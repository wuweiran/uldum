#pragma once

#include "platform/platform.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "render/renderer.h"
#include "render/camera_controller.h"
#include "render/world_overlays.h"
#include "audio/audio.h"
#include "core/settings.h"
#include "i18n/locale.h"
#include "network/game_server.h"
#include "network/network.h"
#include "network/lobby.h"
#include "simulation/command_system.h"
#include "simulation/selection.h"
#include "input/picking.h"
#include "input/input_preset.h"
#include "input/input_bindings.h"
#include "map/map.h"
#include "hud/hud.h"
#include "render/hud/hud_renderer.h"
#include "render/hud/world.h"

#include <memory>
#include <string>
#include <unordered_map>

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
    // BCP 47 locale code (e.g. "en", "zh-CN"). Empty = use default "en".
    // Set via `--locale <code>` CLI arg.
    std::string locale;
    // Bearer token presented to the worker in C_JOIN. Set when the dev
    // CLI got a token from `uldum_server` (via `--server`) or passed one
    // through directly (`--token`). Empty = no token (LAN / dev path).
    std::vector<u8> auth_token;
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

#ifdef ULDUM_SHELL_UI
    // Drive Shell document loading from the AppState machine. Called
    // each frame; loads the matching screen RML when the state
    // transitions, hides on entering Playing. Idempotent — re-loads
    // are gated by `m_shell_state` cache so we don't re-parse RML
    // every frame in steady state.
    void update_shell_for_state();
#endif

    LaunchArgs m_args;
    AppState   m_state = AppState::Menu;
#ifdef ULDUM_SHELL_UI
    // Last AppState the Shell document was synced to. Lets
    // update_shell_for_state load on transitions only.
    AppState   m_shell_state = AppState::Menu;
    bool       m_shell_state_initialized = false;
#endif

    // Set by Shell UI click handlers (e.g. Quit button) to break out of
    // the main loop. Platform's poll_events() returns false on OS-initiated
    // quit (window close); this covers in-app quit requests.
    bool m_wants_quit = false;

    // Client-side ambient-loop handle map. The host assigns the loop
    // an AudioEngine handle when it starts, and that id is what ships
    // on the wire. Each client maps host_id → its own AudioEngine
    // handle so the matching S_AMBIENT_STOP stops the right loop.
    std::unordered_map<u32, u32> m_client_ambient_handles;

    // Lua-driven scene-switch request. Set from `LoadScene(name)` via
    // ScriptEngine's SceneSwitchFn callback; processed once per frame
    // before the tick loop, then cleared. Empty = no request pending.
    std::string m_pending_scene_switch;
    void perform_scene_switch(const std::string& scene_name);

    // Host MP path: scene name being switched to, held while the host
    // waits for every peer to ack C_LOAD_DONE. Empty when no swap is
    // mid-barrier. finalize_scene_switch consumes it.
    std::string m_pending_scene_switch_finalize;
    void scene_switch_local_teardown(const std::string& scene_name);
    void scene_switch_run_main(const std::string& scene_name);
    void finalize_scene_switch();

    // Wire ScriptEngine's per-player camera callbacks to App's
    // CameraController + network. Called by start_session and the
    // scene-switch run_main path (after script.init re-instantiates
    // the VM and clears prior callbacks).
    void register_script_camera_callbacks();

    // Route a camera command to every player in `players_mask`. For
    // each set bit: apply locally if it's the host's own slot, else
    // send the corresponding S_CAMERA_* packet to that peer.
    void route_camera_apply_setup(u32 players_mask,
                                   f32 tx, f32 ty, f32 tz, f32 distance,
                                   f32 pitch_rad, f32 yaw_rad, f32 duration);
    void route_camera_set_target_position(u32 players_mask,
                                           f32 x, f32 y, f32 z, f32 duration);
    void route_camera_set_source_distance(u32 players_mask, f32 distance, f32 duration);
    void route_camera_shake(u32 players_mask, f32 intensity, f32 duration);
    void route_camera_set_target_controller(u32 players_mask, simulation::Unit unit);

#ifdef ULDUM_SHELL_UI
    // Most recent end-of-session elapsed time (seconds) pulled out of the
    // Lua stats JSON. Shown on the Results screen. Stays at 0 until the
    // first EndGame call. Only the Shell UI build reads it.
    f32 m_last_elapsed_seconds = 0.0f;
#endif

    // Last safe-area insets we pushed to the HUD. Compared on each
    // refresh so the HUD doesn't re-resolve composites when nothing
    // changed.
    platform::Platform::SafeInsets m_last_pushed_insets{};

    // ── Persistent (survive across sessions) ────────────────────────────
    std::unique_ptr<platform::Platform> m_platform;
    rhi::Rhi           m_rhi;
    asset::AssetManager      m_asset;
    render::Renderer         m_renderer;
    render::CameraController m_camera_controller;
    render::WorldOverlays    m_world_overlays;
    audio::AudioEngine       m_audio;
    settings::Store          m_settings;
    i18n::LocaleManager      m_i18n;
    hud::Hud                 m_hud;
    hud::HudRenderer         m_hud_renderer;
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
    simulation::CommandSystem m_commands;
    simulation::SelectionState m_selection;
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

    // WC3-style "target acquired" ping. Set when the input preset
    // commits a right-click on a unit / item / destructable. Lives for
    // a fraction of a second, scales/fades, then expires. If `unit` is
    // valid, the ring follows the unit's interpolated position; if not,
    // it stays at `pos`.
    struct TargetPing {
        simulation::Unit unit{};       // invalid → use `pos`
        glm::vec3        pos{0.0f};
        input::InputContext::TargetPingKind kind = input::InputContext::TargetPingKind::Ally;
        f32              age      = 0.0f;
        f32              lifespan = 0.45f;
    };
    TargetPing m_target_ping;

    // Client-side mapping from server-assigned CreateEffect handles to
    // local EffectManager instance ids. Server's id is the canonical
    // wire identifier (handles arrive via S_EFFECT_CREATE and need to
    // resolve back to a local id on S_EFFECT_DESTROY).
    std::unordered_map<u32, u32> m_effect_id_map;
};

} // namespace uldum
