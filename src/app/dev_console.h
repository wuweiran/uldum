#pragma once

#include "app/engine.h"
#include "core/settings.h"
#include "core/types.h"
#include "network/lobby.h"
#include "network/network.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rhi/command_list.h"

namespace uldum::rhi      { class Rhi; }
namespace uldum::platform { class Platform; }

namespace uldum {

// Dev-time ImGui console for `uldum_dev`. Handles:
//   - enumerating bundled `maps/*.uldmap/` archives
//   - starting an offline session on a selected map
//   - hosting a session + accepting clients
//   - connecting to a host by IP/port
//   - ending the current session to return to the picker
//
// The console owns ImGui (context + Win32 backend + Vulkan backend) for the
// `uldum_dev` process. It is NOT used by `uldum_editor` (which has its own
// ImGui init) or `uldum_game` (which uses the Shell UI / RmlUi).
class DevConsole {
public:
    // Action the user requested via the console. App polls `poll_action()`
    // each frame; a non-None action signals the app to act, after which the
    // action is consumed and the next poll returns None again.
    //
    //   EnterLobbyOffline / EnterLobbyHost / EnterLobbyClient — from Menu,
    //     dev asked to prepare a lobby. App loads the map and enters Lobby.
    //   StartGame   — from Lobby, dev clicked Start. Host commits slot
    //     assignments, begins simulation.
    //   LeaveLobby  — from Lobby, dev clicked Back. App tears down map
    //     preview and returns to Menu.
    //   EndSession  — from Playing, dev wants to return to Menu.
    //   Quit        — from anywhere, shut down the process.
    enum class ActionType {
        None,
        EnterLobbyOffline, EnterLobbyHost, EnterLobbyClient,
        HostViaServer,
        ClaimSlot, ReleaseSlot,
        StartGame, LeaveLobby,
        EndSession, Quit,
    };
    struct Action {
        ActionType  type = ActionType::None;
        std::string map_path;
        std::string connect_address;
        u16         port = 7777;
        // Direct-connect bearer token (Connect box) — presented in C_JOIN so an
        // orchestrator-spawned worker's auth check passes. Empty on the LAN path.
        std::string token;
        // Orchestrator base URL for HostViaServer (e.g. "http://127.0.0.1:8080").
        std::string server_url;
        // Lobby-edit payload (used by Claim/Release).
        u32         slot = 0;
    };

    // Seed the locale input with the current value so the field
    // reflects what's active (initial CLI / settings value).
    void set_active_locale(std::string code);

    // The settings panel reads/writes this store live; `save` persists it
    // to disk (the Engine owns the actual path). Both injected at init.
    bool init(rhi::Rhi& rhi, platform::Platform& platform,
              settings::Store& settings, std::function<void()> save);
    void shutdown();

    // Per-frame. `update()` runs the ImGui logic (must happen outside the
    // render pass); `render(cmd)` submits the draw data inside the render
    // pass on the given command buffer.
    //
    // DevConsole reads the lobby / pause view / mode / etc. from the
    // NetworkManager directly; App just passes it through.
    void update(f32 dt, AppState state, network::NetworkManager& net);
    void render(rhi::CommandList& cmd);

    // Consume the most-recent action request. Returns {None} when there is
    // nothing pending.
    Action poll_action();

    // Queue a modal error dialog (e.g. "host failed: port in use"). Shown
    // on the next frame over whatever screen is active; dismissed with OK.
    void show_error(std::string message);

#ifdef ULDUM_ORCHESTRATOR_CLIENT
    // Show the "share with other players" blob after a successful Host via
    // Server: the worker addr:port + the spare slot tokens. Rendered in the
    // menu with a Copy button until the next create. Desktop-only.
    void show_session_info(std::string share_text);
#endif

    // Cached display info for every discoverable map. Populated by
    // `rescan_map_list()` peeking at each .uldmap's manifest.json
    // (cheap — header + one file extract per package). Public because
    // the rescan helper that fills it is a free function in the .cpp.
    struct MapInfo {
        std::string path;               // "maps/test_map.uldmap"
        std::string name;
        std::string author;
        std::string description;
        std::string game_mode;
        std::string version;
        std::string fog_of_war;
        u32         player_count = 0;
        u32         team_count   = 0;
    };

private:
    void rescan_map_list();
    void draw_menu_screen();
    void draw_lobby_screen(network::NetworkManager& net);
    void draw_loading_screen(const network::LobbyState& lobby);
    void draw_session_overlay(network::NetworkManager& net);
    void draw_pause_overlay(network::NetworkManager& net);
    void draw_disconnected_overlay();
    void draw_settings_panel();

    rhi::Rhi*     m_rhi = nullptr;
    platform::Platform* m_platform = nullptr;  // for input feed + map enumeration
    settings::Store*    m_settings = nullptr;  // injected at init; panel reads/writes
    std::function<void()> m_save_settings;     // persists m_settings to disk
    bool                m_show_settings = false;
    void*               m_imgui_pool = nullptr;  // VkDescriptorPool, opaque here
    bool                m_initialized = false;

    std::vector<MapInfo> m_maps;
    i32                  m_map_selected = 0;

    // Multiplayer input fields.
    std::string m_connect_address = "127.0.0.1";
    i32         m_host_port = 7777;  // listen port for the Host button
    i32         m_port = 7777;       // target port for the Connect button
    std::string m_token_input;  // Connect box bearer token (orchestrator workers)

#ifdef ULDUM_ORCHESTRATOR_CLIENT
    // Orchestrator ("Host via Server") input + result display. Desktop-only:
    // the Android dev build shares this TU but is offline, so the whole flow
    // compiles out. m_session_share is the "give this to other players" blob
    // (addr:port + spare tokens) shown after a successful create.
    std::string m_server_url = "http://127.0.0.1:8080";
    std::string m_session_share;      // non-empty → info panel visible
#endif

    // Locale text-input buffer. Seeded by set_active_locale(); the user
    // types any BCP 47 code (`en`, `zh-CN`, `ja`, ...) and presses Enter
    // to apply. Plain text input avoids the need for an engine-shipped
    // locale registry.
    std::string m_locale_input;

    // Pending action (drained by poll_action).
    Action m_pending;

    // Error dialog: non-empty message = a modal is pending/open. Set via
    // show_error(); a one-shot flag opens the ImGui popup once.
    std::string m_error_message;
    bool        m_error_open = false;

    AppState m_state = AppState::Menu;
};

} // namespace uldum
