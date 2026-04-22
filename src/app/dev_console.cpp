#include "app/dev_console.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "platform/platform.h"
#include "core/log.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>

#include <Windows.h>
#include <algorithm>
#include <filesystem>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace uldum {

static constexpr const char* TAG = "DevUI";

bool DevConsole::init(rhi::VulkanRhi& rhi, platform::Platform& platform) {
    m_rhi = &rhi;

    // Descriptor pool — ImGui needs one slot per font texture and per custom
    // image; 100 is far more than we need for dev widgets.
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
    };
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_ci.maxSets       = 100;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes    = pool_sizes;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(rhi.device(), &pool_ci, nullptr, &pool) != VK_SUCCESS) {
        log::error(TAG, "Failed to create ImGui descriptor pool");
        return false;
    }
    m_imgui_pool = pool;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Don't consume input when the dev console isn't actively being used —
    // gameplay keys (WASD camera, selection clicks) need to pass through.
    // We'll adjust this per-widget via ImGui::Begin flags instead of a
    // global input capture.

    HWND hwnd = static_cast<HWND>(platform.native_window_handle());
    float dpi_scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    ImGui::GetStyle().ScaleAllSizes(dpi_scale);
    io.FontGlobalScale = dpi_scale;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance       = rhi.instance();
    init_info.PhysicalDevice = rhi.physical_device();
    init_info.Device         = rhi.device();
    init_info.QueueFamily    = rhi.graphics_family();
    init_info.Queue          = rhi.graphics_queue();
    init_info.DescriptorPool = pool;
    init_info.MinImageCount  = 2;
    init_info.ImageCount     = 2;
    init_info.PipelineInfoMain.MSAASamples = rhi.msaa_samples();
    init_info.UseDynamicRendering = true;

    VkFormat color_format = rhi.swapchain_format();
    VkFormat depth_format = rhi.depth_format();
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat  = depth_format;

    ImGui_ImplVulkan_Init(&init_info);

    // Forward Win32 messages to ImGui so it receives keyboard/mouse input.
    platform.set_message_hook([](void* h, u32 msg, uintptr_t wparam, intptr_t lparam) -> bool {
        LRESULT r = ImGui_ImplWin32_WndProcHandler(
            static_cast<HWND>(h), msg,
            static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
        return r != 0;
    });

    rescan_map_list();

    m_initialized = true;
    log::info(TAG, "Dev console initialized");
    return true;
}

void DevConsole::shutdown() {
    if (!m_initialized) return;
    vkDeviceWaitIdle(m_rhi->device());
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (m_imgui_pool) {
        vkDestroyDescriptorPool(m_rhi->device(),
                                static_cast<VkDescriptorPool>(m_imgui_pool), nullptr);
        m_imgui_pool = nullptr;
    }
    m_initialized = false;
}

void DevConsole::rescan_map_list() {
    m_map_paths.clear();
    // Scan the runtime map directory — `maps/*.uldmap` packed archives,
    // produced by the desktop build into build/bin/maps/. These are what
    // AssetManager actually resolves at runtime, so offering them is honest.
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator("maps", ec)) {
        if (!entry.is_regular_file()) continue;
        auto p = entry.path();
        if (p.extension() != ".uldmap") continue;
        // Store with forward slashes to match engine path convention.
        std::string s = "maps/" + p.filename().string();
        m_map_paths.push_back(std::move(s));
    }
    std::sort(m_map_paths.begin(), m_map_paths.end());
    m_map_selected = std::min(m_map_selected, static_cast<i32>(m_map_paths.size()) - 1);
    if (m_map_selected < 0 && !m_map_paths.empty()) m_map_selected = 0;
}

void DevConsole::update(f32 /*dt*/, AppState state, network::NetworkManager& net) {
    if (!m_initialized) return;
    m_state = state;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    switch (state) {
    case AppState::Menu:
        draw_menu_screen();
        break;
    case AppState::Lobby:
        draw_lobby_screen(net);
        break;
    case AppState::Loading:
        draw_loading_screen(net.lobby_state());
        break;
    case AppState::Playing: {
        draw_session_overlay(net);
        // Stack mid-game dialogs on top. Self-disconnect takes precedence
        // over the pause overlay — if the host dropped, there's no one to
        // wait for.
        bool self_disconnected = (net.mode() == network::Mode::Client &&
                                  !net.is_connected() &&
                                  net.local_player().is_valid());
        if (self_disconnected) {
            draw_disconnected_overlay();
        } else if (net.pause_view_active()) {
            draw_pause_overlay(net);
        }
        break;
    }
    case AppState::Results:
        // Dev build: Results falls through to Menu immediately in the App's
        // main loop, so we never stay here long enough to draw anything.
        break;
    }

    ImGui::Render();
}

void DevConsole::draw_menu_screen() {
    // Centered-ish "Uldum Dev" menu — fullscreen-sized so it reads as the
    // primary UI when there's no scene behind it.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 panel_size{ 520.0f, 420.0f };
    ImVec2 panel_pos{ vp->Pos.x + (vp->Size.x - panel_size.x) * 0.5f,
                      vp->Pos.y + (vp->Size.y - panel_size.y) * 0.5f };
    ImGui::SetNextWindowPos (panel_pos,  ImGuiCond_Always);
    ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);

    ImGui::Begin("Uldum Dev", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    ImGui::TextUnformatted("Select map");
    ImGui::Separator();

    if (m_map_paths.empty()) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1),
            "No maps found in maps/. Run scripts\\build.ps1 first.");
    } else {
        // Map list as a selectable list box.
        if (ImGui::BeginListBox("##maps", ImVec2(-FLT_MIN, 160))) {
            for (i32 i = 0; i < static_cast<i32>(m_map_paths.size()); ++i) {
                bool sel = (i == m_map_selected);
                if (ImGui::Selectable(m_map_paths[i].c_str(), sel)) m_map_selected = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndListBox();
        }
        if (ImGui::Button("Rescan", ImVec2(100, 0))) rescan_map_list();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Session");
    const char* selected = (m_map_selected >= 0 && m_map_selected < static_cast<i32>(m_map_paths.size()))
        ? m_map_paths[m_map_selected].c_str() : nullptr;

    ImGui::BeginDisabled(selected == nullptr);
    if (ImGui::Button("Offline", ImVec2(160, 0))) {
        m_pending.type     = ActionType::EnterLobbyOffline;
        m_pending.map_path = selected;
    }
    ImGui::SameLine();
    if (ImGui::Button("Host", ImVec2(80, 0))) {
        m_pending.type     = ActionType::EnterLobbyHost;
        m_pending.map_path = selected;
        m_pending.port     = static_cast<u16>(m_port);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::InputInt("Port", &m_port, 0, 0);
    char addr_buf[64];
    std::snprintf(addr_buf, sizeof(addr_buf), "%s", m_connect_address.c_str());
    if (ImGui::InputText("Host address", addr_buf, sizeof(addr_buf))) {
        m_connect_address = addr_buf;
    }
    ImGui::BeginDisabled(selected == nullptr);
    if (ImGui::Button("Connect", ImVec2(160, 0))) {
        m_pending.type            = ActionType::EnterLobbyClient;
        m_pending.map_path        = selected;   // in 16b-iii the server tells the client; until then dev picks locally
        m_pending.connect_address = m_connect_address;
        m_pending.port            = static_cast<u16>(m_port);
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Quit", ImVec2(160, 0))) {
        m_pending.type = ActionType::Quit;
    }

    ImGui::End();
}

void DevConsole::draw_lobby_screen(network::NetworkManager& net) {
    network::LobbyState& lobby = net.lobby_state();
    const bool is_host_authority = (net.mode() != network::Mode::Client);
    const u32  seatless_peer_count = net.seatless_peer_count();
    // What peer_id means "me" in the lobby snapshot: LOCAL_PEER sentinel for
    // host/offline (that's what claim_first_open_as_me / host-local claims
    // write), or the server-assigned client_peer_id() for Client.
    const u32  my_peer_id = (net.mode() == network::Mode::Client)
        ? net.client_peer_id() : network::LOCAL_PEER;
    // WC3-style "Game Lobby": table of slots with per-row occupant/team/color
    // plus Start / Back controls. The lobby state object is mutated in-place
    // as the dev clicks occupancy dropdowns or claims slots.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 panel_size{ 620.0f, 460.0f };
    ImVec2 panel_pos{ vp->Pos.x + (vp->Size.x - panel_size.x) * 0.5f,
                      vp->Pos.y + (vp->Size.y - panel_size.y) * 0.5f };
    ImGui::SetNextWindowPos (panel_pos,  ImGuiCond_Always);
    ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);

    ImGui::Begin("Game Lobby", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);

    ImGui::Text("Map: %s", lobby.map_name.c_str());
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "%s", lobby.map_path.c_str());
    ImGui::Separator();

    // Client-pre-sync: host hasn't sent S_LOBBY_STATE yet, so we have no
    // slots to render. Show a status line instead of an empty table.
    if (lobby.slots.empty()) {
        ImGui::TextColored(ImVec4(1, 0.9f, 0.4f, 1), "Connecting to host — waiting for lobby state...");
        ImGui::Spacing();
        if (ImGui::Button("Back", ImVec2(120, 0))) {
            m_pending.type = ActionType::LeaveLobby;
        }
        ImGui::End();
        return;
    }

    // Group slots by team. Each row shows color, occupancy (Open / Computer
    // / name of seated player), and a Claim or Release button. Locked slots
    // (reserved for future use, e.g. named NPCs) render with no button.
    std::vector<u32> teams;
    for (const auto& a : lobby.slots) {
        if (std::find(teams.begin(), teams.end(), a.team) == teams.end())
            teams.push_back(a.team);
    }
    std::sort(teams.begin(), teams.end());

    const char* occ_labels[] = { "Open", "Computer", "Human" };

    for (u32 team : teams) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.4f, 1), "Team %u", team);
        if (ImGui::BeginTable((std::string("team_") + std::to_string(team)).c_str(), 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Color",    ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Occupant");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableHeadersRow();

            for (u32 i = 0; i < lobby.slots.size(); ++i) {
                auto& a = lobby.slots[i];
                if (a.team != team) continue;
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a.color.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(occ_labels[static_cast<int>(a.occupant)]);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a.display_name.c_str());

                ImGui::TableNextColumn();
                bool is_mine = (a.occupant == network::SlotOccupant::Human && a.peer_id == my_peer_id);
                bool can_claim = !a.locked && !is_mine;
                bool can_release = is_mine;
                if (can_release) {
                    if (ImGui::Button("Release", ImVec2(-FLT_MIN, 0))) {
                        m_pending.type = ActionType::ReleaseSlot;
                        m_pending.slot = i;
                    }
                } else {
                    ImGui::BeginDisabled(!can_claim);
                    if (ImGui::Button("Claim", ImVec2(-FLT_MIN, 0))) {
                        m_pending.type = ActionType::ClaimSlot;
                        m_pending.slot = i;
                    }
                    ImGui::EndDisabled();
                }

                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Open slots are fine — they mean "no player in that seat" at game time.
    // Connected-but-seatless peers are NOT fine — they'd become zombie
    // clients (no S_WELCOME, empty world). Block Start until they claim.
    if (is_host_authority) {
        bool can_start = (seatless_peer_count == 0);
        if (!can_start) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1),
                "Waiting for %u connected peer(s) to claim a slot...", seatless_peer_count);
        }
        ImGui::BeginDisabled(!can_start);
        if (ImGui::Button("Start Game", ImVec2(160, 0))) {
            m_pending.type = ActionType::StartGame;
        }
        ImGui::EndDisabled();
    } else {
        // Client: prompt to claim if not yet seated, otherwise wait on host.
        u32 my_slot = network::lobby_slot_for_peer(lobby, net.client_peer_id());
        if (my_slot == UINT32_MAX) {
            ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1),
                "Click Claim on a slot to join the game.");
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1),
                "Waiting for host to start the game...");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Back", ImVec2(120, 0))) {
        m_pending.type = ActionType::LeaveLobby;
    }

    ImGui::End();
}

void DevConsole::draw_loading_screen(const network::LobbyState& lobby) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 panel_size{ 420.0f, 140.0f };
    ImVec2 panel_pos{ vp->Pos.x + (vp->Size.x - panel_size.x) * 0.5f,
                      vp->Pos.y + (vp->Size.y - panel_size.y) * 0.5f };
    ImGui::SetNextWindowPos (panel_pos,  ImGuiCond_Always);
    ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);

    ImGui::Begin("Loading", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);

    ImGui::Text("Loading map: %s", lobby.map_name.c_str());
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1),
        "Loading terrain and waiting for all players...");

    ImGui::End();
}

void DevConsole::draw_session_overlay(network::NetworkManager& /*net*/) {
    // Small HUD-style panel in the top-right — lets the dev end the session
    // without quitting the exe. Deliberately minimal; gameplay input
    // continues to reach the 3D scene.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 panel_pos{ vp->Pos.x + vp->Size.x - 260.0f, vp->Pos.y + 12.0f };
    ImGui::SetNextWindowPos(panel_pos, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.35f);

    ImGui::Begin("Session", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    ImGui::Text("FPS: %.0f", static_cast<double>(ImGui::GetIO().Framerate));
    ImGui::Separator();
    if (ImGui::Button("End Session", ImVec2(220, 0))) {
        m_pending.type = ActionType::EndSession;
    }

    ImGui::End();
}

void DevConsole::draw_pause_overlay(network::NetworkManager& net) {
    // Centered modal-style dialog on top of the frozen scene. Lists every
    // disconnected player with a live countdown. End Session returns to
    // Menu if the user doesn't want to wait.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 panel_size{ 460.0f, 220.0f };
    ImVec2 panel_pos{ vp->Pos.x + (vp->Size.x - panel_size.x) * 0.5f,
                      vp->Pos.y + (vp->Size.y - panel_size.y) * 0.5f };
    ImGui::SetNextWindowPos (panel_pos,  ImGuiCond_Always);
    ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);

    ImGui::Begin("Game Paused", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1),
        "Paused — waiting for disconnected player(s) to reconnect");
    ImGui::Separator();
    for (const auto& d : net.disconnected_view()) {
        ImGui::Text("  %s (player %u) — %.0fs",
            d.display_name.empty() ? "(unnamed)" : d.display_name.c_str(),
            d.player_id, d.seconds_remaining);
    }
    ImGui::Separator();
    if (ImGui::Button("End Session", ImVec2(220, 0))) {
        m_pending.type = ActionType::EndSession;
    }

    ImGui::End();
}

void DevConsole::draw_disconnected_overlay() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 panel_size{ 420.0f, 160.0f };
    ImVec2 panel_pos{ vp->Pos.x + (vp->Size.x - panel_size.x) * 0.5f,
                      vp->Pos.y + (vp->Size.y - panel_size.y) * 0.5f };
    ImGui::SetNextWindowPos (panel_pos,  ImGuiCond_Always);
    ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);

    ImGui::Begin("Disconnected", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Lost connection to host.");
    ImGui::TextUnformatted("The game is no longer in sync.");
    ImGui::Separator();
    if (ImGui::Button("Return to Menu", ImVec2(220, 0))) {
        m_pending.type = ActionType::EndSession;
    }

    ImGui::End();
}

void DevConsole::render(VkCommandBuffer cmd) {
    if (!m_initialized) return;
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

DevConsole::Action DevConsole::poll_action() {
    Action a = m_pending;
    m_pending = Action{};
    return a;
}

} // namespace uldum
