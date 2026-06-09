#pragma once

#include "platform/platform.h"
#include "rhi/rhi.h"
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

// Forward-declare UI types that Engine owns via unique_ptr. Engine's destructor
// is defined out-of-line in engine.cpp, so the header doesn't need complete
// types — this lets dev_console.h / ui/shell.h include app.h in turn
// without a cycle.
namespace uldum::shell { class Shell; }
namespace uldum { class App; }

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

class Engine {
public:
    Engine();
    ~Engine();  // out-of-line (engine.cpp) so header only needs forward-decls
                // of App / Shell — see top of file.

    // One-time init of persistent engine subsystems (platform, RHI, renderer, audio, asset).
    bool init(const LaunchArgs& args = {});

    // Main loop — drives AppState transitions.
    void run();

    // One-time shutdown of persistent engine subsystems.
    void shutdown();

    // ── Surface used by App implementations ──────────────────────────
    // Subsystem accessors. The App constructs / interacts with these
    // directly. Returned references are valid for the engine's lifetime
    // (they live as Engine members).
    rhi::Rhi&                  rhi()      { return m_rhi; }
    platform::Platform&        platform() { return *m_platform; }
    settings::Store&           settings() { return m_settings; }
    network::NetworkManager&   network()  { return m_network; }
#ifdef ULDUM_SHELL_UI
    // Shell UI is only present in game-flavor builds. The App is
    // responsible for loading RML documents and binding their click
    // handlers; the engine no longer hosts screen-specific logic.
    shell::Shell&              shell()    { return *m_shell; }
    // Most recent end-of-session elapsed time (seconds). Stays at 0
    // until the first session ends. Used by the App to populate the
    // Results screen.
    f32                        last_session_elapsed_seconds() const { return m_last_elapsed_seconds; }
#endif

    // Launch args — apps mutate fields like map_path / net_mode before
    // calling enter_lobby() to configure the next session.
    LaunchArgs&                launch_args()       { return m_args; }
    const LaunchArgs&          launch_args() const { return m_args; }

    // App-state read / write. Apps drive transitions that the engine
    // can't infer (Menu ↔ Lobby) by calling set_state directly; the
    // engine auto-drives the rest (Loading on enter_lobby, Playing
    // when the simulation comes up, Results on session end). set_state
    // fires App::on_state_changed on actual transitions (no-op if the
    // new state equals the current one) — defined out-of-line because
    // it needs App to be a complete type.
    AppState                   state() const { return m_state; }
    void                       set_state(AppState s);

    // Session verbs. enter_lobby + leave_lobby are App-driven (user
    // picks a map, then backs out). end_session is also user-driven
    // (the user quits a running game).
    bool enter_lobby();
    void leave_lobby();
    void end_session();
    bool is_session_active() const { return m_session_active; }

    // Quit request. The next main-loop iteration drains to shutdown.
    void request_quit() { m_wants_quit = true; }

private:
    // ── Session lifecycle ────────────────────────────────────────────────
    // Start the game from the lobby. Preconditions: enter_lobby() succeeded
    // and m_network.lobby_state() is populated (all slots non-Open,
    // m_args.local_slot set). Engine-internal — promoted from the App's
    // request via the AppState transition Lobby → Loading.
    bool start_session();

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
    // Most recent end-of-session elapsed time (seconds) pulled out of
    // the Lua stats JSON. Stays at 0 until the first EndGame call. The
    // App reads it via last_session_elapsed_seconds() (declared up in
    // the public surface) to populate the Results screen.
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

    // The App implementation for this binary. Constructed in Engine::init
    // as `make_unique<ULDUM_APP_CLASS>()` — the class is picked at compile
    // time via the ULDUM_APP_CLASS macro that CMake sets per target. Always
    // non-null after init, so engine-side dispatch through it needs no guards.
    std::unique_ptr<App> m_app;

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
