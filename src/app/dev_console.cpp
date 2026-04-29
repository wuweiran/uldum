#include "app/dev_console.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "platform/platform.h"
#include "asset/upk.h"
#include "core/log.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
// Win32 path: ImGui's Win32 backend handles WM_* → ImGui IO (mouse,
// keyboard, char input, focus, cursor shape). We keep using it on
// desktop because it's the proven path.
#include <imgui_impl_win32.h>
#include <Windows.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

#include <algorithm>

namespace uldum {

static constexpr const char* TAG = "DevUI";

bool DevConsole::init(rhi::VulkanRhi& rhi, platform::Platform& platform) {
    m_rhi      = &rhi;
    m_platform = &platform;

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
    ImGui::StyleColorsDark();

    // Per-platform input plumbing + DPI scaling.
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(platform.native_window_handle());
    float dpi_scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    ImGui::GetStyle().ScaleAllSizes(dpi_scale);
    io.FontGlobalScale = dpi_scale;

    ImGui_ImplWin32_Init(hwnd);

    // Forward Win32 messages to ImGui so it receives keyboard / mouse /
    // char input, focus changes, etc.
    platform.set_message_hook([](void* h, u32 msg, uintptr_t wparam, intptr_t lparam) -> bool {
        LRESULT r = ImGui_ImplWin32_WndProcHandler(
            static_cast<HWND>(h), msg,
            static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
        return r != 0;
    });
#else
    // Android / non-Win32: feed ImGui IO manually from the platform's
    // InputState each frame (see feed_input_manual). Touch-as-mouse is
    // already mirrored by the platform layer's first-finger rule, so
    // ImGui's mouse-only widget code works out of the box.
    float ui_scale = platform.ui_scale();
    if (ui_scale > 0.0f) {
        // Android pixel densities are much higher than desktop's 1×;
        // scale ImGui sizes + font so widgets are readable on phones.
        ImGui::GetStyle().ScaleAllSizes(ui_scale);
        io.FontGlobalScale = ui_scale;
    }
#endif

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

    rescan_map_list();

    m_initialized = true;
    log::info(TAG, "Dev console initialized");
    return true;
}

void DevConsole::shutdown() {
    if (!m_initialized) return;
    vkDeviceWaitIdle(m_rhi->device());
    ImGui_ImplVulkan_Shutdown();
#ifdef _WIN32
    ImGui_ImplWin32_Shutdown();
#endif
    ImGui::DestroyContext();
    if (m_imgui_pool) {
        vkDestroyDescriptorPool(m_rhi->device(),
                                static_cast<VkDescriptorPool>(m_imgui_pool), nullptr);
        m_imgui_pool = nullptr;
    }
    m_initialized = false;
}

#ifndef _WIN32
// Manual per-frame ImGui IO feed for platforms without a native ImGui
// backend (Android). Mirrors the touch-first input from
// `Platform::input()` into ImGui's queued event API. Buttons + lists
// work; full keyboard / IME for text fields is out of scope for v1
// (deferred with the rest of mobile IME).
static void feed_imgui_input_manual(const platform::Platform& platform, f32 dt) {
    ImGuiIO& io = ImGui::GetIO();
    const auto& in = platform.input();

    io.DeltaTime    = dt > 0.0f ? dt : (1.0f / 60.0f);
    io.DisplaySize  = ImVec2(static_cast<float>(platform.width()),
                              static_cast<float>(platform.height()));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

    io.AddMousePosEvent(in.mouse_x, in.mouse_y);
    io.AddMouseButtonEvent(0, in.mouse_left);
    io.AddMouseButtonEvent(1, in.mouse_right);
    io.AddMouseButtonEvent(2, in.mouse_middle);
    if (in.scroll_delta != 0.0f) io.AddMouseWheelEvent(0.0f, in.scroll_delta);

    io.AddKeyEvent(ImGuiMod_Shift, in.key_shift);
    io.AddKeyEvent(ImGuiMod_Ctrl,  in.key_ctrl);
    io.AddKeyEvent(ImGuiMod_Alt,   in.key_alt);
    io.AddKeyEvent(ImGuiKey_Escape, in.key_escape);
}
#endif

// Peek at a .uldmap package's manifest.json without mounting it. Cheap:
// opens the .upk header + one entry. Returns false on any read failure
// — the caller falls back to a path-only entry.
static bool read_map_info(std::string_view pkg_path, DevConsole::MapInfo& out) {
    asset::UPKReader r;
    if (!r.open(pkg_path)) return false;
    auto bytes = r.read("manifest.json");
    if (bytes.empty()) return false;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(bytes.begin(), bytes.end());
    } catch (...) {
        return false;
    }

    out.name              = j.value("name",              std::string{});
    out.author            = j.value("author",            std::string{});
    out.description       = j.value("description",       std::string{});
    out.game_mode         = j.value("game_mode",         std::string{});
    out.suggested_players = j.value("suggested_players", std::string{});
    out.version           = j.value("version",           std::string{});
    out.fog_of_war        = j.value("fog_of_war",        std::string{});
    if (j.contains("players") && j["players"].is_array())
        out.player_count = static_cast<u32>(j["players"].size());
    if (j.contains("teams") && j["teams"].is_array())
        out.team_count   = static_cast<u32>(j["teams"].size());
    return true;
}

// Inset-adjusted viewport rect. Subtracts the platform safe-area insets
// (status bar, gesture nav, notch) from the ImGui main viewport so dev
// panels don't land under system chrome on Android. Desktop returns
// the unmodified viewport since insets are zero there.
static void get_safe_viewport(const platform::Platform* platform,
                              ImVec2& out_pos, ImVec2& out_size) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    out_pos  = vp->Pos;
    out_size = vp->Size;
    if (!platform) return;
    auto ins = platform->safe_insets();
    out_pos.x  += ins.left;
    out_pos.y  += ins.top;
    out_size.x -= (ins.left + ins.right);
    out_size.y -= (ins.top  + ins.bottom);
    if (out_size.x < 0) out_size.x = 0;
    if (out_size.y < 0) out_size.y = 0;
}

void DevConsole::rescan_map_list() {
    m_maps.clear();
    if (!m_platform) return;

    // Cross-platform map enumeration — Platform::list_files dispatches
    // to filesystem on desktop and AAssetManager_openDir on Android,
    // both yielding the basenames in `maps/`. Filter to `.uldmap`
    // archives because those are what AssetManager actually mounts.
    auto files = m_platform->list_files("maps");
    for (auto& name : files) {
        if (name.size() < 7 || name.compare(name.size() - 7, 7, ".uldmap") != 0)
            continue;
        MapInfo info;
        info.path = "maps/" + name;
        if (!read_map_info(info.path, info)) {
            log::warn(TAG, "Could not read manifest.json from '{}'", info.path);
            // Fallback to filename-derived display name so the row still
            // renders even if the manifest peek failed.
            info.name = name.substr(0, name.size() - 7);  // strip ".uldmap"
        }
        m_maps.push_back(std::move(info));
    }
    std::sort(m_maps.begin(), m_maps.end(),
              [](const MapInfo& a, const MapInfo& b) { return a.path < b.path; });
    m_map_selected = std::min(m_map_selected, static_cast<i32>(m_maps.size()) - 1);
    if (m_map_selected < 0 && !m_maps.empty()) m_map_selected = 0;
}

void DevConsole::update([[maybe_unused]] f32 dt, AppState state, network::NetworkManager& net) {
    if (!m_initialized) return;
    m_state = state;

    ImGui_ImplVulkan_NewFrame();
#ifdef _WIN32
    ImGui_ImplWin32_NewFrame();
#else
    if (m_platform) feed_imgui_input_manual(*m_platform, dt);
#endif
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
    // Fullscreen dev menu. All hard-coded sizes go through `s` so 100%
    // and 300% DPI both lay out cleanly — rows fit the (scaled) text and
    // buttons don't clip their labels. ScaleAllSizes() scales padding /
    // spacing for us; we scale only the local hard-coded numbers.
    ImVec2 vp_pos, vp_size;
    get_safe_viewport(m_platform, vp_pos, vp_size);
    ImGui::SetNextWindowPos (vp_pos,  ImGuiCond_Always);
    ImGui::SetNextWindowSize(vp_size, ImGuiCond_Always);

    const float s = ImGui::GetIO().FontGlobalScale > 0.0f
                  ? ImGui::GetIO().FontGlobalScale : 1.0f;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.10f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * s, 16 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8  * s,  8 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

    ImGui::Begin("##uldum_dev_menu", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    const MapInfo* selected = (m_map_selected >= 0 && m_map_selected < static_cast<i32>(m_maps.size()))
        ? &m_maps[m_map_selected] : nullptr;
    const char* selected_path = selected ? selected->path.c_str() : nullptr;

    // Layout: list on the left, two stacked panes on the right (info on
    // top, session on bottom), Quit at the bottom of the screen.
    const float row_h        = ImGui::GetFrameHeightWithSpacing();
    const float footer_h     = row_h + 8.0f * s;
    const float right_pane_w = 320.0f * s;
    const float gap          = 12.0f * s;
    const ImVec2 btn         = ImVec2(120.0f * s, 0);   // shared across every action button

    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= footer_h;
    float left_pane_w = avail.x - right_pane_w - gap;
    if (left_pane_w < 200.0f * s) left_pane_w = 200.0f * s;

    // Info pane gets ~55% of the right column's height; session takes
    // the rest. Floor it so neither collapses on tiny windows.
    float info_h    = avail.y * 0.55f;
    float session_h = avail.y - info_h - gap;
    if (info_h    < 160.0f * s) info_h    = 160.0f * s;
    if (session_h < 160.0f * s) session_h = 160.0f * s;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.12f, 0.15f, 1.0f));

    // ── Maps pane (left, full height) ─────────────────────────────────
    ImGui::BeginChild("##maps", ImVec2(left_pane_w, avail.y), ImGuiChildFlags_Borders);
    {
        ImGui::Text("Maps (%d)", static_cast<int>(m_maps.size()));
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btn.x);
        if (ImGui::Button("Rescan", btn)) rescan_map_list();
        ImGui::Separator();

        if (m_maps.empty()) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "No maps in maps/");
        } else {
            for (i32 i = 0; i < static_cast<i32>(m_maps.size()); ++i) {
                bool sel = (i == m_map_selected);
                ImGui::PushID(i);
                const char* label = m_maps[i].name.empty()
                                  ? m_maps[i].path.c_str()
                                  : m_maps[i].name.c_str();
                if (ImGui::Selectable(label, sel,
                        ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_map_selected = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        m_pending.type     = ActionType::EnterLobbyOffline;
                        m_pending.map_path = m_maps[i].path;
                    }
                }
                if (sel) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::Dummy(ImVec2(gap - ImGui::GetStyle().ItemSpacing.x, 0));
    ImGui::SameLine();

    // Right column — vertical split (info on top, session on bottom).
    // Wrap in a child so the two stacked panes share the same column
    // without ImGui flowing the second one to a new row.
    ImGui::BeginGroup();

    // ── Info pane (top-right) ─────────────────────────────────────────
    ImGui::BeginChild("##info", ImVec2(right_pane_w, info_h), ImGuiChildFlags_Borders);
    {
        ImGui::TextUnformatted("Map info");
        ImGui::Separator();

        if (!selected) {
            ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1), "No map selected");
        } else {
            auto kv = [](const char* key, const std::string& val) {
                if (val.empty()) return;
                ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1), "%s", key);
                ImGui::SameLine();
                ImGui::TextWrapped("%s", val.c_str());
            };
            ImGui::TextWrapped("%s", selected->name.empty()
                ? selected->path.c_str() : selected->name.c_str());
            ImGui::TextColored(ImVec4(0.45f, 0.50f, 0.60f, 1), "%s", selected->path.c_str());
            ImGui::Dummy(ImVec2(0, 4 * s));
            kv("Author:",  selected->author);
            kv("Mode:",    selected->game_mode);
            kv("Players:", selected->suggested_players);
            kv("Slots:",   std::to_string(selected->player_count));
            kv("Teams:",   std::to_string(selected->team_count));
            kv("Fog:",     selected->fog_of_war);
            kv("Version:", selected->version);
            if (!selected->description.empty()) {
                ImGui::Dummy(ImVec2(0, 4 * s));
                ImGui::Separator();
                ImGui::TextWrapped("%s", selected->description.c_str());
            }
        }
    }
    ImGui::EndChild();

    // ── Session pane (bottom-right) ───────────────────────────────────
    ImGui::BeginChild("##session", ImVec2(right_pane_w, session_h), ImGuiChildFlags_Borders);
    {
        ImGui::TextUnformatted("Session");
        ImGui::Separator();

        ImGui::BeginDisabled(selected_path == nullptr);
        if (ImGui::Button("Offline", btn)) {
            m_pending.type     = ActionType::EnterLobbyOffline;
            m_pending.map_path = selected_path ? selected_path : "";
        }
        if (ImGui::Button("Host", btn)) {
            m_pending.type     = ActionType::EnterLobbyHost;
            m_pending.map_path = selected_path ? selected_path : "";
            m_pending.port     = static_cast<u16>(m_port);
        }
        ImGui::EndDisabled();

        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::Separator();

        char addr_buf[64];
        std::snprintf(addr_buf, sizeof(addr_buf), "%s", m_connect_address.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##addr", addr_buf, sizeof(addr_buf))) {
            m_connect_address = addr_buf;
        }
        ImGui::SetNextItemWidth(120 * s);
        ImGui::InputInt("Port", &m_port, 0, 0);

        ImGui::BeginDisabled(selected_path == nullptr);
        if (ImGui::Button("Connect", btn)) {
            m_pending.type            = ActionType::EnterLobbyClient;
            m_pending.map_path        = selected_path ? selected_path : "";
            m_pending.connect_address = m_connect_address;
            m_pending.port            = static_cast<u16>(m_port);
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    ImGui::EndGroup();
    ImGui::PopStyleColor();

    // ── Footer ─────────────────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::Button("Quit", btn)) {
        m_pending.type = ActionType::Quit;
    }

    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void DevConsole::draw_lobby_screen(network::NetworkManager& net) {
    network::LobbyState& lobby = net.lobby_state();
    const bool is_host_authority = (net.mode() != network::Mode::Client);
    const u32  seatless_peer_count = net.seatless_peer_count();
    const u32  my_peer_id = (net.mode() == network::Mode::Client)
        ? net.client_peer_id() : network::LOCAL_PEER;

    ImVec2 vp_pos, vp_size;
    get_safe_viewport(m_platform, vp_pos, vp_size);
    ImGui::SetNextWindowPos (vp_pos,  ImGuiCond_Always);
    ImGui::SetNextWindowSize(vp_size, ImGuiCond_Always);

    const float s = ImGui::GetIO().FontGlobalScale > 0.0f
                  ? ImGui::GetIO().FontGlobalScale : 1.0f;
    const ImVec2 btn = ImVec2(120.0f * s, 0);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.10f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * s, 16 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8  * s,  8 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

    ImGui::Begin("##uldum_dev_lobby", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::Text("Map: %s", lobby.map_name.c_str());
    ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1), "%s", lobby.map_path.c_str());
    ImGui::Separator();

    // Client-pre-sync: no slots yet, just status + Back.
    if (lobby.slots.empty()) {
        ImGui::TextColored(ImVec4(1, 0.9f, 0.4f, 1),
            "Connecting to host - waiting for lobby state...");
        ImGui::Dummy(ImVec2(0, 8 * s));
        if (ImGui::Button("Back", btn)) {
            m_pending.type = ActionType::LeaveLobby;
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        return;
    }

    // Reserve footer row (single line: status + Start/Back) so the
    // slot-table child fills the rest of the viewport.
    const float footer_h = ImGui::GetFrameHeightWithSpacing() + 8.0f * s;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= footer_h;
    if (avail.y < 200.0f * s) avail.y = 200.0f * s;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.12f, 0.15f, 1.0f));
    ImGui::BeginChild("##slots", ImVec2(-FLT_MIN, avail.y), ImGuiChildFlags_Borders);
    {
        std::vector<u32> teams;
        for (const auto& a : lobby.slots) {
            if (std::find(teams.begin(), teams.end(), a.team) == teams.end())
                teams.push_back(a.team);
        }
        std::sort(teams.begin(), teams.end());

        const char* occ_labels[] = { "Open", "Computer", "Human" };

        for (u32 team : teams) {
            ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.4f, 1), "Team %u", team);
            if (ImGui::BeginTable((std::string("team_") + std::to_string(team)).c_str(), 4,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Color",    ImGuiTableColumnFlags_WidthFixed, 90 * s);
                ImGui::TableSetupColumn("Occupant", ImGuiTableColumnFlags_WidthFixed, 110 * s);
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed, btn.x);
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
                        if (ImGui::Button("Release", btn)) {
                            m_pending.type = ActionType::ReleaseSlot;
                            m_pending.slot = i;
                        }
                    } else {
                        ImGui::BeginDisabled(!can_claim);
                        if (ImGui::Button("Claim", btn)) {
                            m_pending.type = ActionType::ClaimSlot;
                            m_pending.slot = i;
                        }
                        ImGui::EndDisabled();
                    }

                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::Dummy(ImVec2(0, 4 * s));
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Separator();

    // Footer — single row: status text on the left, action buttons
    // right-aligned. host gets Start + Back; client gets Back only.
    bool can_start = is_host_authority && (seatless_peer_count == 0);
    char status[128] = "";
    if (is_host_authority) {
        if (!can_start) {
            std::snprintf(status, sizeof(status),
                "Waiting for %u peer(s) to claim a slot...", seatless_peer_count);
        }
    } else {
        u32 my_slot = network::lobby_slot_for_peer(lobby, net.client_peer_id());
        if (my_slot == UINT32_MAX) std::snprintf(status, sizeof(status), "Claim a slot to join.");
        else                       std::snprintf(status, sizeof(status), "Waiting for host to start...");
    }
    if (status[0]) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s", status);
    else           ImGui::Dummy(ImVec2(0, 0));

    // Right-align Start (host only) + Back on the same row as status.
    float trailing_w = btn.x;
    if (is_host_authority) trailing_w += btn.x + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - trailing_w);
    if (is_host_authority) {
        ImGui::BeginDisabled(!can_start);
        if (ImGui::Button("Start", btn)) m_pending.type = ActionType::StartGame;
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::Button("Back", btn)) m_pending.type = ActionType::LeaveLobby;

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void DevConsole::draw_loading_screen(const network::LobbyState& lobby) {
    const float s = ImGui::GetIO().FontGlobalScale > 0.0f
                  ? ImGui::GetIO().FontGlobalScale : 1.0f;
    ImVec2 vp_pos, vp_size;
    get_safe_viewport(m_platform, vp_pos, vp_size);
    ImVec2 panel_size{ 420.0f * s, 140.0f * s };
    ImVec2 panel_pos{ vp_pos.x + (vp_size.x - panel_size.x) * 0.5f,
                      vp_pos.y + (vp_size.y - panel_size.y) * 0.5f };
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
    // Tiny HUD-style panel in the top-right — FPS + an End button so the
    // dev can return to the picker without quitting the exe. Auto-sizes
    // to its content so the panel stays as small as the widgets allow.
    const float s = ImGui::GetIO().FontGlobalScale > 0.0f
                  ? ImGui::GetIO().FontGlobalScale : 1.0f;
    ImVec2 vp_pos, vp_size;
    get_safe_viewport(m_platform, vp_pos, vp_size);

    // Right-anchor by guessing a tight content width — auto-resize will
    // shrink the actual window. 6 dp padding from the safe-area edge.
    ImGui::SetNextWindowPos(ImVec2(vp_pos.x + vp_size.x - 6.0f * s,
                                   vp_pos.y + 6.0f * s),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6 * s, 4 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4 * s, 4 * s));

    ImGui::Begin("##session_overlay", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoTitleBar);

    ImGui::Text("FPS %.0f", static_cast<double>(ImGui::GetIO().Framerate));
    ImGui::SameLine();
    if (ImGui::Button("End")) {
        m_pending.type = ActionType::EndSession;
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void DevConsole::draw_pause_overlay(network::NetworkManager& net) {
    // Centered modal-style dialog on top of the frozen scene. Lists every
    // disconnected player with a live countdown. End Session returns to
    // Menu if the user doesn't want to wait.
    const float s = ImGui::GetIO().FontGlobalScale > 0.0f
                  ? ImGui::GetIO().FontGlobalScale : 1.0f;
    ImVec2 vp_pos, vp_size;
    get_safe_viewport(m_platform, vp_pos, vp_size);
    ImVec2 panel_size{ 460.0f * s, 220.0f * s };
    ImVec2 panel_pos{ vp_pos.x + (vp_size.x - panel_size.x) * 0.5f,
                      vp_pos.y + (vp_size.y - panel_size.y) * 0.5f };
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
    if (ImGui::Button("End Session", ImVec2(180.0f * s, 0))) {
        m_pending.type = ActionType::EndSession;
    }

    ImGui::End();
}

void DevConsole::draw_disconnected_overlay() {
    const float s = ImGui::GetIO().FontGlobalScale > 0.0f
                  ? ImGui::GetIO().FontGlobalScale : 1.0f;
    ImVec2 vp_pos, vp_size;
    get_safe_viewport(m_platform, vp_pos, vp_size);
    ImVec2 panel_size{ 420.0f * s, 160.0f * s };
    ImVec2 panel_pos{ vp_pos.x + (vp_size.x - panel_size.x) * 0.5f,
                      vp_pos.y + (vp_size.y - panel_size.y) * 0.5f };
    ImGui::SetNextWindowPos (panel_pos,  ImGuiCond_Always);
    ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);

    ImGui::Begin("Disconnected", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Lost connection to host.");
    ImGui::TextUnformatted("The game is no longer in sync.");
    ImGui::Separator();
    if (ImGui::Button("Return to Menu", ImVec2(180.0f * s, 0))) {
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
