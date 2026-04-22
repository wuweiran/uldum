#pragma once

#include "app/app.h"
#include "core/types.h"
#include "network/lobby.h"
#include "network/network.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Vulkan + platform forward-decls
typedef struct VkCommandBuffer_T* VkCommandBuffer;
namespace uldum::rhi      { class VulkanRhi; }
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
        ClaimSlot, ReleaseSlot,
        StartGame, LeaveLobby,
        EndSession, Quit,
    };
    struct Action {
        ActionType  type = ActionType::None;
        std::string map_path;
        std::string connect_address;
        u16         port = 7777;
        // Lobby-edit payload (used by Claim/Release).
        u32         slot = 0;
    };

    bool init(rhi::VulkanRhi& rhi, platform::Platform& platform);
    void shutdown();

    // Per-frame. `update()` runs the ImGui logic (must happen outside the
    // render pass); `render(cmd)` submits the draw data inside the render
    // pass on the given command buffer.
    //
    // DevConsole reads the lobby / pause view / mode / etc. from the
    // NetworkManager directly; App just passes it through.
    void update(f32 dt, AppState state, network::NetworkManager& net);
    void render(VkCommandBuffer cmd);

    // Consume the most-recent action request. Returns {None} when there is
    // nothing pending.
    Action poll_action();

private:
    void rescan_map_list();
    void draw_menu_screen();
    void draw_lobby_screen(network::NetworkManager& net);
    void draw_loading_screen(const network::LobbyState& lobby);
    void draw_session_overlay(network::NetworkManager& net);
    void draw_pause_overlay(network::NetworkManager& net);
    void draw_disconnected_overlay();

    rhi::VulkanRhi* m_rhi = nullptr;
    void*           m_imgui_pool = nullptr;  // VkDescriptorPool, opaque here
    bool            m_initialized = false;

    // Cached list of discoverable maps, populated at init and on demand.
    std::vector<std::string> m_map_paths;   // e.g. "maps/test_map.uldmap"
    i32                      m_map_selected = 0;

    // Multiplayer input fields.
    std::string m_connect_address = "127.0.0.1";
    i32         m_port = 7777;

    // Pending action (drained by poll_action).
    Action m_pending;

    AppState m_state = AppState::Menu;
};

} // namespace uldum
