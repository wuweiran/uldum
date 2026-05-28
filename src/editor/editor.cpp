#include "editor/editor.h"
#include "map/map.h"
#include "map/terrain_data.h"
#include "simulation/pathfinding.h"  // PATHING_SUBDIV
#include "core/log.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <chrono>
#include <shobjidl.h>
#include <cmath>

// stb_image is in third_party and already linked via uldum_asset.
// stbi_info() reads only the PNG header — cheap and lets the import
// dialog prefill the resize fields with the source's dimensions.
#include <stb_image.h>

// Forward declare the Win32 ImGui WndProc handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace uldum::editor {

// ── Undo helpers (used by begin_stroke/end_stroke/apply_edit) ────────────

static void copy_region(const map::TerrainData& td, u32 min_vx, u32 min_vy, u32 max_vx, u32 max_vy,
                         std::vector<f32>& heightmap, std::vector<u8>& cliff,
                         std::vector<u8>& tile_layer, std::vector<u8>& pathing) {
    u32 w = max_vx - min_vx + 1;
    u32 count = w * (max_vy - min_vy + 1);
    heightmap.resize(count);
    cliff.resize(count);
    tile_layer.resize(count);
    pathing.resize(count);

    for (u32 vy = min_vy; vy <= max_vy; ++vy) {
        for (u32 vx = min_vx; vx <= max_vx; ++vx) {
            u32 src = vy * td.verts_x() + vx;
            u32 dst = (vy - min_vy) * w + (vx - min_vx);
            heightmap[dst]  = td.heightmap[src];
            cliff[dst]      = td.cliff_level[src];
            tile_layer[dst] = td.tile_layer[src];
            pathing[dst]    = td.pathing[src];
        }
    }
}

static void paste_region(map::TerrainData& td, u32 min_vx, u32 min_vy, u32 max_vx, u32 max_vy,
                          const std::vector<f32>& heightmap, const std::vector<u8>& cliff,
                          const std::vector<u8>& tile_layer, const std::vector<u8>& pathing) {
    u32 w = max_vx - min_vx + 1;
    for (u32 vy = min_vy; vy <= max_vy; ++vy) {
        for (u32 vx = min_vx; vx <= max_vx; ++vx) {
            u32 dst = vy * td.verts_x() + vx;
            u32 src = (vy - min_vy) * w + (vx - min_vx);
            td.heightmap[dst]   = heightmap[src];
            td.cliff_level[dst] = cliff[src];
            td.tile_layer[dst]  = tile_layer[src];
            td.pathing[dst]     = pathing[src];
        }
    }
}

static constexpr const char* TAG = "Editor";

// ── Init / Shutdown ──────────────────────────────────────────────────────

bool Editor::init(const std::string& map_path) {
    if (!map_path.empty()) m_map_path = map_path;
    log::info(TAG, "=== Initializing Uldum Editor ===");

#ifdef ULDUM_DEBUG
    log::set_level(log::Level::Trace);
#else
    log::set_level(log::Level::Info);
#endif

    // Platform
    m_platform = platform::Platform::create();
    platform::Config platform_config{};
    platform_config.title  = "Uldum Editor";
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

    // Renderer
    if (!m_renderer.init(m_rhi)) {
        log::error(TAG, "Renderer init failed");
        return false;
    }

    // Editor-side depth-tested line renderer — used by placement
    // footprints, selection circles, and (eventually) brush + region
    // wireframes. Owns its own pipeline so the editor's overlay
    // language stays line-art independent of the runtime HUD.
    if (!m_overlays.init(m_rhi)) {
        log::error(TAG, "Editor overlays init failed");
        return false;
    }

    // Simulation (for type registry — needed to place objects)
    if (!m_simulation.init(m_asset)) {
        log::error(TAG, "Simulation init failed");
        return false;
    }

    // Map
    if (!m_map.init()) {
        log::error(TAG, "MapManager init failed");
        return false;
    }

    // Resolve map path relative to source directory so saves go to the source tree
#ifdef ULDUM_SOURCE_DIR
    m_map_path = std::string(ULDUM_SOURCE_DIR) + "/" + m_map_path;
#endif

    if (m_map.load_map(m_map_path, m_asset, m_simulation, /*allow_directory=*/true)) {
        m_map_loaded = true;
        m_scenes = m_map.list_scenes();
        m_current_scene = m_map.manifest().start_scene;
        m_renderer.set_map_root(m_map.map_root());
        m_renderer.load_tileset_textures(m_map.tileset());
        m_renderer.set_environment(m_map.manifest().environment);
        if (m_map.terrain().is_valid()) {
            std::vector<u8> shallow, deep;
            m_map.tileset().get_water_layer_ids(shallow, deep);
            m_map.terrain().set_water_layers(shallow, deep);
            m_renderer.set_terrain(&m_map.terrain());
            m_simulation.set_terrain(&m_map.terrain());
        }
        log::info(TAG, "Map loaded: {} ({} scenes)", m_map_path, m_scenes.size());
    } else {
        log::warn(TAG, "No map loaded — starting with empty terrain");
    }

    // ImGui
    if (!init_imgui()) {
        log::error(TAG, "ImGui init failed");
        return false;
    }

    // Hook Win32 messages to ImGui so it receives input
    m_platform->set_message_hook([](void* hwnd, u32 msg, uintptr_t wparam, intptr_t lparam) -> bool {
        LRESULT result = ImGui_ImplWin32_WndProcHandler(
            static_cast<HWND>(hwnd), msg,
            static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
        return result != 0;
    });

    log::info(TAG, "=== Editor initialized ===");
    return true;
}

bool Editor::init_imgui() {
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 },
    };

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_ci.maxSets       = 100;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes    = pool_sizes;

    if (vkCreateDescriptorPool(m_rhi.device(), &pool_ci, nullptr, &m_imgui_pool) != VK_SUCCESS) {
        log::error(TAG, "Failed to create ImGui descriptor pool");
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // DPI scaling
    HWND hwnd = static_cast<HWND>(m_platform->native_window_handle());
    float dpi_scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    ImGui::GetStyle().ScaleAllSizes(dpi_scale);
    io.FontGlobalScale = dpi_scale;

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance       = m_rhi.instance();
    init_info.PhysicalDevice = m_rhi.physical_device();
    init_info.Device         = m_rhi.device();
    init_info.QueueFamily    = m_rhi.graphics_family();
    init_info.Queue          = m_rhi.graphics_queue();
    init_info.DescriptorPool = m_imgui_pool;
    init_info.MinImageCount  = 2;
    init_info.ImageCount     = 2;
    init_info.PipelineInfoMain.MSAASamples = m_rhi.msaa_samples();
    init_info.UseDynamicRendering = true;

    VkFormat color_format = m_rhi.swapchain_format_vk();
    VkFormat depth_format = m_rhi.depth_format_vk();
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    init_info.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = depth_format;

    ImGui_ImplVulkan_Init(&init_info);

    log::info(TAG, "ImGui initialized (Vulkan + Win32, dynamic rendering)");
    return true;
}

// ── Main loop ────────────────────────────────────────────────────────────

void Editor::run() {
    log::info(TAG, "Entering editor loop");

    auto previous_time = std::chrono::high_resolution_clock::now();
    u32 frame_count = 0;

    while (m_platform->poll_events()) {
        if (m_platform->input().key_escape) break;

        // Handle resize
        if (m_platform->was_resized()) {
            m_rhi.handle_resize(m_platform->width(), m_platform->height());
            f32 aspect = static_cast<f32>(m_platform->width()) / static_cast<f32>(m_platform->height());
            m_renderer.handle_resize(aspect);
        }

        // Delta time
        auto current_time = std::chrono::high_resolution_clock::now();
        f32 frame_dt = std::chrono::duration<f32>(current_time - previous_time).count();
        previous_time = current_time;
        if (frame_dt > 0.25f) frame_dt = 0.25f;

        // Terrain cursor — raycast and snap to nearest vertex
        auto& input = m_platform->input();

        // Scroll wheel zoom (when not over UI)
        if (input.scroll_delta != 0.0f && !ImGui::GetIO().WantCaptureMouse) {
            m_renderer.camera().zoom(input.scroll_delta);
        }
        m_cursor_valid = raycast_terrain(input.mouse_x, input.mouse_y, m_cursor_pos);
        if (m_cursor_valid && m_map_loaded) {
            auto& td = m_map.terrain();
            m_cursor_vx = static_cast<i32>(std::round((m_cursor_pos.x - td.origin_x()) / td.tile_size));
            m_cursor_vy = static_cast<i32>(std::round((m_cursor_pos.y - td.origin_y()) / td.tile_size));
            m_cursor_vx = std::clamp(m_cursor_vx, 0, static_cast<i32>(td.tiles_x));
            m_cursor_vy = std::clamp(m_cursor_vy, 0, static_cast<i32>(td.tiles_y));
        }
        // Keep the Object-mode ghost unit in sync with the current
        // cursor + selected type. Creates / destroys / moves the
        // preview entity as needed.
        update_object_preview();

        // Right-click drag: pan camera (ground-pinned at Z=0 plane)
        bool over_ui = ImGui::GetIO().WantCaptureMouse;
        auto unproject_to_ground = [&](f32 sx, f32 sy, glm::vec3& out) -> bool {
            auto& cam = m_renderer.camera();
            f32 w = static_cast<f32>(m_rhi.extent().width);
            f32 h = static_cast<f32>(m_rhi.extent().height);
            if (w <= 0 || h <= 0) return false;
            f32 ndc_x = (2.0f * sx / w) - 1.0f;
            f32 ndc_y = (2.0f * sy / h) - 1.0f;
            glm::mat4 inv_vp = glm::inverse(cam.view_projection());
            glm::vec4 nw = inv_vp * glm::vec4{ndc_x, ndc_y, 0.0f, 1.0f};
            glm::vec4 fw = inv_vp * glm::vec4{ndc_x, ndc_y, 1.0f, 1.0f};
            nw /= nw.w;
            fw /= fw.w;
            glm::vec3 ro = glm::vec3(nw);
            glm::vec3 rd = glm::normalize(glm::vec3(fw) - ro);
            if (std::abs(rd.z) < 1e-6f) return false;
            f32 t = -ro.z / rd.z;  // intersect Z=0 plane
            if (t <= 0.0f) return false;
            out = ro + rd * t;
            return true;
        };
        if (input.mouse_right_pressed && !over_ui) {
            if (unproject_to_ground(input.mouse_x, input.mouse_y, m_drag_anchor))
                m_drag_active = true;
        }
        if (input.mouse_right && m_drag_active) {
            glm::vec3 current;
            if (unproject_to_ground(input.mouse_x, input.mouse_y, current)) {
                glm::vec3 delta = m_drag_anchor - current;
                m_renderer.camera().translate(delta.x, delta.y);
            }
        }
        if (input.mouse_right_released) {
            m_drag_active = false;
        }

        // Mode dispatch for left-click. Terrain mode runs the brush
        // stroke logic below; Place mode handles each rising-edge
        // click separately (no drag-paint) and short-circuits the
        // brush path.
        if (m_mode == EditMode::Object && input.mouse_left_pressed && m_cursor_valid && !over_ui) {
            place_mode_on_left_click(input.mouse_x, input.mouse_y);
        }
        // Continuous-mode brush: while the left button stays held and
        // we're in Place mode, keep attempting to place. Dedup gates
        // prevent spam.
        if (m_mode == EditMode::Object && m_place_continuous &&
            input.mouse_left && !input.mouse_left_pressed && m_cursor_valid && !over_ui) {
            place_mode_continuous_tick(input.mouse_x, input.mouse_y);
        }
        // Mouse released → reset the dedup baseline so the next stroke
        // starts fresh (the next click is always allowed).
        if (input.mouse_left_released) {
            m_has_last_placement = false;
        }
        // Delete key removes the current Place-mode selection. Pulled
        // through ImGui because InputState doesn't carry a Delete bit.
        if (m_mode == EditMode::Object && !over_ui &&
            ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false)) {
            place_mode_on_delete();
        }

        // Region-mode click drag: Add* tools shape new rects/circles
        // in the focused region; Select tool picks a region under the
        // cursor. Drag-active state survives a mouse drag, finalized
        // on release. Delete key removes the focused region.
        if (m_mode == EditMode::Region && m_cursor_valid && !over_ui &&
            input.mouse_left_pressed) {
            region_mode_on_left_press();
        }
        if (m_mode == EditMode::Region && m_cursor_valid &&
            m_region_drag_active) {
            m_region_drag_current = {m_cursor_pos.x, m_cursor_pos.y};
        }
        if (m_mode == EditMode::Region && input.mouse_left_released &&
            m_region_drag_active) {
            region_mode_on_left_release();
        }
        if (m_mode == EditMode::Region && !over_ui &&
            ImGui::IsKeyPressed(ImGuiKey_Delete, /*repeat=*/false)) {
            region_mode_on_delete();
        }

        // Apply brush when left-clicking on terrain (and not over ImGui)
        if (m_mode == EditMode::Terrain && input.mouse_left && m_cursor_valid && !over_ui) {
            if (!m_stroke_active) begin_stroke();

            // If cursor moved outside the snapshot region, re-snapshot with full terrain
            if (m_stroke_active && !m_undo_stack.empty()) {
                auto& edit = m_undo_stack.back();
                i32 r = m_brush_size;
                bool outside = (m_cursor_vx - r < static_cast<i32>(edit.min_vx)) ||
                               (m_cursor_vy - r < static_cast<i32>(edit.min_vy)) ||
                               (m_cursor_vx + r > static_cast<i32>(edit.max_vx)) ||
                               (m_cursor_vy + r > static_cast<i32>(edit.max_vy));
                if (outside) {
                    // Extend to full terrain — old snapshot still valid for its region,
                    // but we need the surrounding area too. Re-snapshot at full size.
                    auto& td = m_map.terrain();
                    // The current terrain already has modifications from this stroke.
                    // We can't get the old state of the new area. Extend old_ by reading
                    // the NEW region and copying old_ into its sub-region.
                    u32 new_min_vx = 0, new_min_vy = 0;
                    u32 new_max_vx = td.tiles_x, new_max_vy = td.tiles_y;
                    u32 new_w = new_max_vx - new_min_vx + 1;
                    u32 new_count = new_w * (new_max_vy - new_min_vy + 1);

                    // Read current terrain as the "old" base (already partially modified)
                    std::vector<f32> h(new_count); std::vector<u8> c(new_count), tl(new_count), p(new_count);
                    copy_region(td, new_min_vx, new_min_vy, new_max_vx, new_max_vy, h, c, tl, p);

                    // Overlay the original old snapshot into the correct sub-region
                    u32 old_w = edit.max_vx - edit.min_vx + 1;
                    for (u32 vy = edit.min_vy; vy <= edit.max_vy; ++vy) {
                        for (u32 vx = edit.min_vx; vx <= edit.max_vx; ++vx) {
                            u32 ni = (vy - new_min_vy) * new_w + (vx - new_min_vx);
                            u32 oi = (vy - edit.min_vy) * old_w + (vx - edit.min_vx);
                            h[ni]  = edit.old_heightmap[oi];
                            c[ni]  = edit.old_cliff[oi];
                            tl[ni] = edit.old_tile_layer[oi];
                            p[ni]  = edit.old_pathing[oi];
                        }
                    }

                    edit.min_vx = new_min_vx; edit.min_vy = new_min_vy;
                    edit.max_vx = new_max_vx; edit.max_vy = new_max_vy;
                    edit.old_heightmap = std::move(h);
                    edit.old_cliff = std::move(c);
                    edit.old_tile_layer = std::move(tl);
                    edit.old_pathing = std::move(p);
                }
            }

            if (m_continuous) {
                apply_brush(frame_dt);
            } else {
                bool new_vertex = (m_cursor_vx != m_last_brush_vx || m_cursor_vy != m_last_brush_vy);
                if (!m_brush_applied || new_vertex) {
                    apply_brush(1.0f);
                    m_brush_applied = true;
                    m_last_brush_vx = m_cursor_vx;
                    m_last_brush_vy = m_cursor_vy;
                }
            }
        }
        if (!input.mouse_left) {
            if (m_stroke_active) end_stroke();
            m_brush_applied = false;
        }

        // Undo/Redo (Ctrl+Z / Ctrl+Y) — rising edge only
        {
            bool z_now = input.key_ctrl && input.key_letter['Z' - 'A'];
            bool y_now = input.key_ctrl && input.key_letter['Y' - 'A'];
            if (z_now && !m_prev_undo_key && !m_stroke_active) undo();
            if (y_now && !m_prev_redo_key && !m_stroke_active) redo();
            m_prev_undo_key = z_now;
            m_prev_redo_key = y_now;
        }

        // Re-upload terrain mesh if modified
        if (m_terrain_dirty) {
            rebuild_terrain_mesh();
            m_terrain_dirty = false;
        }

        // ImGui
        imgui_new_frame();
        draw_ui();
        m_overlays.begin_frame();
        draw_overlays();

        // Render. Minimized window → cmd is null / extent is zero; we still
        // need to balance the ImGui::NewFrame() above with EndFrame() so
        // next loop iteration doesn't assert on a stale frame.
        rhi::CommandList cmd = m_rhi.begin_frame();
        if (cmd.is_valid() && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
            m_renderer.draw_shadows(cmd, m_simulation.world());
            m_rhi.begin_rendering();
            m_renderer.draw(cmd, m_rhi.extent(), m_simulation.world());
            m_overlays.draw(cmd, m_renderer.camera().view_projection());
            imgui_render(cmd);
            m_rhi.end_frame();
        } else {
            ImGui::EndFrame();
        }

        frame_count++;
    }

    m_rhi.wait_idle();
    log::info(TAG, "Exiting editor loop ({} frames)", frame_count);
}

void Editor::imgui_new_frame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Editor::imgui_render(rhi::CommandList& cmd) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                                    static_cast<VkCommandBuffer>(cmd.backend_handle()));
}

// ── Ray-terrain intersection ─────────────────────────────────────────────

bool Editor::raycast_terrain(f32 screen_x, f32 screen_y, glm::vec3& hit) const {
    if (!m_map_loaded || !m_map.terrain().is_valid()) return false;

    auto& cam = m_renderer.camera();
    f32 w = static_cast<f32>(m_rhi.extent().width);
    f32 h = static_cast<f32>(m_rhi.extent().height);
    if (w <= 0 || h <= 0) return false;

    // Unproject NDC → world-space ray, then defer to the shared
    // terrain raycast (DDA + per-tile bilinear) in map::.
    f32 ndc_x = (2.0f * screen_x / w) - 1.0f;
    f32 ndc_y = (2.0f * screen_y / h) - 1.0f;
    glm::mat4 inv_vp = glm::inverse(cam.view_projection());
    glm::vec4 near_clip{ndc_x, ndc_y, 0.0f, 1.0f};
    glm::vec4 far_clip {ndc_x, ndc_y, 1.0f, 1.0f};
    glm::vec4 near_world = inv_vp * near_clip;  near_world /= near_world.w;
    glm::vec4 far_world  = inv_vp * far_clip;   far_world  /= far_world.w;
    glm::vec3 ray_origin = glm::vec3(near_world);
    glm::vec3 ray_dir    = glm::normalize(glm::vec3(far_world) - ray_origin);

    return map::raycast_terrain(m_map.terrain(), ray_origin, ray_dir, hit);
}

// ── Brush helpers ────────────────────────────────────────────────────────

// Compute vertex range affected by brush (clamped to terrain bounds)
struct BrushRange {
    u32 min_ix, max_ix, min_iy, max_iy;
};

// size = WC3 convention (1 = single vertex). Radius = size - 1.
static BrushRange compute_brush_range(const map::TerrainData& td, i32 cx, i32 cy, i32 size) {
    i32 r = size - 1;
    BrushRange br;
    br.min_ix = static_cast<u32>(std::max(0, cx - r));
    br.max_ix = static_cast<u32>(std::min(static_cast<i32>(td.tiles_x), cx + r)) + 1;
    br.min_iy = static_cast<u32>(std::max(0, cy - r));
    br.max_iy = static_cast<u32>(std::min(static_cast<i32>(td.tiles_y), cy + r)) + 1;
    return br;
}

// Falloff based on tile distance from center vertex (0 at edge, 1 at center)
// size = WC3 convention (1 = single vertex). Radius = size - 1.
static f32 tile_falloff(i32 dx, i32 dy, i32 size) {
    i32 r = size - 1;
    if (r <= 0) return 1.0f;  // size 1: full strength at center
    f32 dist = std::sqrt(static_cast<f32>(dx * dx + dy * dy));
    f32 t = dist / static_cast<f32>(r);
    if (t >= 1.0f) return 0.0f;
    return (1.0f - t * t) * (1.0f - t * t);
}

// ── Brush application ────────────────────────────────────────────────────

void Editor::apply_brush(f32 dt) {
    // In single-click mode (dt=1), strength = m_brush_amount
    // In continuous mode (dt=frame_dt), strength = m_brush_speed
    f32 strength = m_continuous ? m_brush_speed : m_brush_amount;

    switch (m_tool) {
    case Tool::Raise:   brush_raise(strength, dt);   break;
    case Tool::Lower:   brush_lower(strength, dt);   break;
    case Tool::Smooth:  brush_smooth(strength, dt);  break;
    case Tool::Flatten: brush_flatten(strength, dt); break;
    case Tool::Paint:   brush_paint(strength, dt);   break;
    case Tool::CliffRaise:
        if (!m_brush_applied) { brush_cliff_raise(); m_brush_applied = true; }
        break;
    case Tool::CliffLower:
        if (!m_brush_applied) { brush_cliff_lower(); m_brush_applied = true; }
        break;
    case Tool::RampSet:
        if (!m_brush_applied) { brush_ramp_set(); m_brush_applied = true; }
        break;
    case Tool::RampClear:
        if (!m_brush_applied) { brush_ramp_clear(); m_brush_applied = true; }
        break;
    default: break;
    }
}

void Editor::brush_raise(f32 strength, f32 dt) {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);

    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            f32 w = tile_falloff(static_cast<i32>(ix) - m_cursor_vx,
                                 static_cast<i32>(iy) - m_cursor_vy, m_brush_size);
            if (w > 0.0f) {
                td.height_at(ix, iy) += strength * w * dt;
                m_terrain_dirty = true;
            }
        }
    }
}

void Editor::brush_lower(f32 strength, f32 dt) {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);

    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            f32 w = tile_falloff(static_cast<i32>(ix) - m_cursor_vx,
                                 static_cast<i32>(iy) - m_cursor_vy, m_brush_size);
            if (w > 0.0f) {
                td.height_at(ix, iy) -= strength * w * dt;
                m_terrain_dirty = true;
            }
        }
    }
}

void Editor::brush_smooth(f32 strength, f32 dt) {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);

    // Compute weighted average height
    f32 total_h = 0.0f, total_w = 0.0f;
    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            f32 w = tile_falloff(static_cast<i32>(ix) - m_cursor_vx,
                                 static_cast<i32>(iy) - m_cursor_vy, m_brush_size);
            if (w > 0.0f) {
                total_h += td.height_at(ix, iy) * w;
                total_w += w;
            }
        }
    }
    if (total_w <= 0.0f) return;
    f32 avg_h = total_h / total_w;

    f32 blend = std::min(1.0f, strength * dt * 0.1f);
    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            f32 w = tile_falloff(static_cast<i32>(ix) - m_cursor_vx,
                                 static_cast<i32>(iy) - m_cursor_vy, m_brush_size);
            if (w > 0.0f) {
                f32& h = td.height_at(ix, iy);
                h += (avg_h - h) * w * blend;
                m_terrain_dirty = true;
            }
        }
    }
}

void Editor::brush_flatten(f32 strength, f32 dt) {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);

    f32 blend = std::min(1.0f, strength * dt * 0.1f);
    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            f32 w = tile_falloff(static_cast<i32>(ix) - m_cursor_vx,
                                 static_cast<i32>(iy) - m_cursor_vy, m_brush_size);
            if (w > 0.0f) {
                f32& h = td.height_at(ix, iy);
                h += (m_flatten_height - h) * w * blend;
                m_terrain_dirty = true;
            }
        }
    }
}

void Editor::brush_paint(f32 /*strength*/, f32 /*dt*/) {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);
    u32 max_layer = m_map.tileset().layers.empty() ? 3 : static_cast<u32>(m_map.tileset().layers.size()) - 1;
    u8 layer = static_cast<u8>(std::clamp(m_paint_layer, 0, static_cast<int>(max_layer)));

    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            f32 w = tile_falloff(static_cast<i32>(ix) - m_cursor_vx,
                                 static_cast<i32>(iy) - m_cursor_vy, m_brush_size);
            if (w <= 0.0f) continue;

            td.tile_layer[iy * td.verts_x() + ix] = layer;
            m_terrain_dirty = true;
        }
    }
}

// ── Cliff level editing ──────────────────────────────────────────────────

// Cliff brush: raise all 4 corners of tiles whose center is within the circular brush.
// Propagate cliff levels to enforce max 1 difference between adjacent vertices.
// direction: +1 = after raise (pull neighbors up), -1 = after lower (pull neighbors down)
static void enforce_cliff_constraint(map::TerrainData& td, i32 direction) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (u32 vy = 0; vy <= td.tiles_y; ++vy) {
            for (u32 vx = 0; vx <= td.tiles_x; ++vx) {
                u8 level = td.cliff_at(vx, vy);
                // Check 8 neighbors (including diagonals — tile corners share a tile)
                for (auto [nx, ny] : {std::pair{vx-1,vy-1}, {vx,vy-1}, {vx+1,vy-1},
                                      std::pair{vx-1,vy},              {vx+1,vy},
                                      std::pair{vx-1,vy+1}, {vx,vy+1}, {vx+1,vy+1}}) {
                    if (nx > td.tiles_x || ny > td.tiles_y) continue;  // unsigned underflow handles < 0
                    u8& neighbor = td.cliff_at(nx, ny);
                    if (direction > 0 && neighbor + 1 < level) {
                        neighbor = level - 1;
                        changed = true;
                    } else if (direction < 0 && level + 1 < neighbor) {
                        neighbor = level + 1;
                        changed = true;
                    }
                }
            }
        }
    }
}

void Editor::brush_cliff_raise() {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);
    i32 r = m_brush_size - 1;
    i32 r2 = r * r;

    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            i32 dx = static_cast<i32>(ix) - m_cursor_vx;
            i32 dy = static_cast<i32>(iy) - m_cursor_vy;
            if (dx * dx + dy * dy > r2) continue;

            u8& level = td.cliff_at(ix, iy);
            if (level < 15) { level++; m_terrain_dirty = true; }
        }
    }
    enforce_cliff_constraint(td, +1);
    cleanup_ramp_flags();
}

void Editor::brush_cliff_lower() {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);
    i32 r = m_brush_size - 1;
    i32 r2 = r * r;

    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            i32 dx = static_cast<i32>(ix) - m_cursor_vx;
            i32 dy = static_cast<i32>(iy) - m_cursor_vy;
            if (dx * dx + dy * dy > r2) continue;

            u8& level = td.cliff_at(ix, iy);
            if (level > 0) { level--; m_terrain_dirty = true; }
        }
    }
    enforce_cliff_constraint(td, -1);
    cleanup_ramp_flags();
}

void Editor::cleanup_ramp_flags() {
    // A vertex keeps RAMP only if it belongs to at least one valid ramp tile
    // (cliff diff = 1 AND all 4 corners have RAMP).
    auto& td = m_map.terrain();
    if (!td.is_valid()) return;

    for (u32 vy = 0; vy <= td.tiles_y; ++vy) {
        for (u32 vx = 0; vx <= td.tiles_x; ++vx) {
            if (!(td.pathing_at(vx, vy) & map::PATHING_RAMP)) continue;

            bool valid = false;
            for (i32 dy = -1; dy <= 0 && !valid; ++dy) {
                for (i32 dx = -1; dx <= 0 && !valid; ++dx) {
                    i32 tx = static_cast<i32>(vx) + dx;
                    i32 ty = static_cast<i32>(vy) + dy;
                    if (tx < 0 || ty < 0 || tx >= static_cast<i32>(td.tiles_x) || ty >= static_cast<i32>(td.tiles_y))
                        continue;
                    u8 c[4] = {
                        td.cliff_at(tx, ty), td.cliff_at(tx+1, ty),
                        td.cliff_at(tx, ty+1), td.cliff_at(tx+1, ty+1)
                    };
                    u8 cmin = std::min({c[0], c[1], c[2], c[3]});
                    u8 cmax = std::max({c[0], c[1], c[2], c[3]});
                    if (cmax - cmin != 1) continue;

                    // All 4 corners of this tile must have RAMP
                    bool all_ramp = (td.pathing_at(tx, ty)     & map::PATHING_RAMP) &&
                                    (td.pathing_at(tx+1, ty)   & map::PATHING_RAMP) &&
                                    (td.pathing_at(tx, ty+1)   & map::PATHING_RAMP) &&
                                    (td.pathing_at(tx+1, ty+1) & map::PATHING_RAMP);
                    if (all_ramp) valid = true;
                }
            }

            if (!valid) {
                td.pathing_at(vx, vy) &= ~map::PATHING_RAMP;
                m_terrain_dirty = true;
            }
        }
    }
}

void Editor::brush_ramp_set() {
    // Ramp operates per-tile: set RAMP on all 4 corners of valid tiles.
    // A tile is valid for ramp if it has exactly 1 cliff level difference.
    auto& td = m_map.terrain();
    i32 r = m_brush_size - 1;
    i32 r2 = r * r;

    // Tiles touching the brush center vertex: (cx-1,cy-1) to (cx,cy)
    for (i32 ty = m_cursor_vy - r; ty <= m_cursor_vy + r - 1; ++ty) {
        for (i32 tx = m_cursor_vx - r; tx <= m_cursor_vx + r - 1; ++tx) {
            if (tx < 0 || ty < 0 || tx >= static_cast<i32>(td.tiles_x) || ty >= static_cast<i32>(td.tiles_y))
                continue;

            // Check brush radius (tile center distance to cursor vertex)
            f32 dx = static_cast<f32>(tx) + 0.5f - static_cast<f32>(m_cursor_vx);
            f32 dy = static_cast<f32>(ty) + 0.5f - static_cast<f32>(m_cursor_vy);
            if (dx * dx + dy * dy > static_cast<f32>(r2) + 0.5f) continue;

            u8 c[4] = {
                td.cliff_at(tx, ty), td.cliff_at(tx+1, ty),
                td.cliff_at(tx, ty+1), td.cliff_at(tx+1, ty+1)
            };
            u8 cmin = std::min({c[0], c[1], c[2], c[3]});
            u8 cmax = std::max({c[0], c[1], c[2], c[3]});
            if (cmax - cmin != 1) continue;  // must be exactly 1 level diff

            // Set RAMP on all 4 corners
            td.pathing_at(tx, ty)     |= map::PATHING_RAMP;
            td.pathing_at(tx+1, ty)   |= map::PATHING_RAMP;
            td.pathing_at(tx, ty+1)   |= map::PATHING_RAMP;
            td.pathing_at(tx+1, ty+1) |= map::PATHING_RAMP;
            m_terrain_dirty = true;
        }
    }
}

void Editor::brush_ramp_clear() {
    // Clear ramp per-tile: remove RAMP from all 4 corners.
    auto& td = m_map.terrain();
    i32 r = m_brush_size - 1;
    i32 r2 = r * r;

    for (i32 ty = m_cursor_vy - r; ty <= m_cursor_vy + r - 1; ++ty) {
        for (i32 tx = m_cursor_vx - r; tx <= m_cursor_vx + r - 1; ++tx) {
            if (tx < 0 || ty < 0 || tx >= static_cast<i32>(td.tiles_x) || ty >= static_cast<i32>(td.tiles_y))
                continue;

            f32 dx = static_cast<f32>(tx) + 0.5f - static_cast<f32>(m_cursor_vx);
            f32 dy = static_cast<f32>(ty) + 0.5f - static_cast<f32>(m_cursor_vy);
            if (dx * dx + dy * dy > static_cast<f32>(r2) + 0.5f) continue;

            td.pathing_at(tx, ty)     &= ~map::PATHING_RAMP;
            td.pathing_at(tx+1, ty)   &= ~map::PATHING_RAMP;
            td.pathing_at(tx, ty+1)   &= ~map::PATHING_RAMP;
            td.pathing_at(tx+1, ty+1) &= ~map::PATHING_RAMP;
            m_terrain_dirty = true;
        }
    }
    // Restore RAMP on vertices that still belong to a valid ramp tile
    cleanup_ramp_flags();
}


// ── Place mode ───────────────────────────────────────────────────────────

void Editor::destroy_preview() {
    if (m_preview_type_id.empty()) return;
    auto& world = m_simulation.world();
    if (world.validate(m_preview_handle)) {
        simulation::destroy(world, m_preview_handle);
    }
    m_preview_handle = {};
    m_preview_type_id.clear();
}

// Sample a fresh effective variation from the active destructable
// type's `models` array. Called when entering destructable mode,
// switching type, toggling Random on, and after each placement (so
// the next placement gets a different variation).
void Editor::roll_destructable_variation() {
    if (!m_place_destructable_random) {
        m_place_destructable_var = m_place_destructable_fixed_var;
        return;
    }
    if (!m_map_loaded || m_place_destructable_type.empty()) {
        m_place_destructable_var = 0;
        return;
    }
    const auto* def = m_simulation.types().get_destructable_type(m_place_destructable_type);
    u32 n = def && !def->models.empty() ? static_cast<u32>(def->models.size()) : 1;
    m_place_destructable_var = static_cast<u8>(std::rand() % n);
}

void Editor::roll_doodad_variation() {
    if (!m_place_doodad_random) {
        m_place_doodad_var = m_place_doodad_fixed_var;
        return;
    }
    if (!m_map_loaded || m_place_doodad_type.empty()) {
        m_place_doodad_var = 0;
        return;
    }
    const auto* def = m_simulation.types().get_doodad_type(m_place_doodad_type);
    u32 n = def && !def->models.empty() ? static_cast<u32>(def->models.size()) : 1;
    m_place_doodad_var = static_cast<u8>(std::rand() % n);
}

// Keep the preview entity in sync with the current Object-mode state.
// Called once per frame after the cursor raycast. The editor doesn't
// run the simulation tick, so the entity's gameplay components are
// inert — it just renders. Used for Unit and Destructable categories
// alike (Item is left as a 2D ring; no static-model preview yet).
void Editor::update_object_preview() {
    if (!m_map_loaded) { destroy_preview(); return; }

    bool over_ui      = ImGui::GetIO().WantCaptureMouse;
    bool category_ok  = (m_object_category == ObjectCategory::Unit         && !m_place_unit_type.empty()) ||
                        (m_object_category == ObjectCategory::Destructable && !m_place_destructable_type.empty()) ||
                        (m_object_category == ObjectCategory::Doodad       && !m_place_doodad_type.empty());
    bool want_preview = m_mode             == EditMode::Object &&
                        m_object_tool      == ObjectTool::Place &&
                        m_cursor_valid && !over_ui &&
                        category_ok;
    if (!want_preview) { destroy_preview(); return; }

    auto& world = m_simulation.world();
    auto& td    = m_map.terrain();
    if (!td.is_valid()) { destroy_preview(); return; }

    if (m_object_category == ObjectCategory::Unit) {
        // Recreate when category or type changes.
        if (m_preview_category != ObjectCategory::Unit || m_preview_type_id != m_place_unit_type) {
            destroy_preview();
            f32 facing_rad = m_place_unit_facing_deg * (glm::pi<f32>() / 180.0f);
            m_preview_handle = simulation::create_unit(
                world, m_place_unit_type,
                simulation::Player{m_place_unit_owner},
                m_cursor_pos.x, m_cursor_pos.y, facing_rad);
            if (!m_preview_handle.is_valid()) return;
            m_preview_type_id = m_place_unit_type;
            m_preview_category = ObjectCategory::Unit;
            // Suppress the birth animation so the preview unit doesn't
            // pop out of the ground every time you switch type. Also
            // half-alpha so the preview reads as a placement ghost
            // rather than a real unit (skinned-mesh path only —
            // static-mesh previews stay opaque until the static
            // pipeline plumbs alpha through InstanceData).
            if (auto* r = world.renderables.get(m_preview_handle.id)) {
                r->skip_birth   = true;
                r->visual_alpha = 0.5f;
            }
        }

        // Move the preview to follow the cursor (snapped if a building).
        const auto* def = m_simulation.types().get_unit_type(m_preview_type_id);
        if (!def) return;
        f32 wx = m_cursor_pos.x, wy = m_cursor_pos.y;
        if (def->pathing_footprint_w > 0 && def->pathing_footprint_h > 0) {
            wx = map::snap_building_x(td, wx, def->pathing_footprint_w);
            wy = map::snap_building_y(td, wy, def->pathing_footprint_h);
        }
        if (auto* t = world.transforms.get(m_preview_handle.id)) {
            t->position.x = wx;
            t->position.y = wy;
            t->position.z = map::sample_height(td, wx, wy);
            t->facing     = m_place_unit_facing_deg * (glm::pi<f32>() / 180.0f);
        }
    } else if (m_object_category == ObjectCategory::Destructable) {
        // Destructable. Recreate when category, type, or variation changes.
        bool stale = m_preview_category != ObjectCategory::Destructable ||
                     m_preview_type_id  != m_place_destructable_type ||
                     m_preview_variation != m_place_destructable_var;
        if (stale) {
            destroy_preview();
            auto h = simulation::create_destructable(
                world, m_place_destructable_type,
                m_cursor_pos.x, m_cursor_pos.y, /*facing=*/0,
                m_place_destructable_var);
            if (!h.is_valid()) return;
            // Layout-compatible cast — m_preview_handle is typed Unit
            // but only stores id+generation, which Destructable also has.
            m_preview_handle.id         = h.id;
            m_preview_handle.generation = h.generation;
            m_preview_type_id           = m_place_destructable_type;
            m_preview_category          = ObjectCategory::Destructable;
            m_preview_variation         = m_place_destructable_var;
            if (auto* r = world.renderables.get(m_preview_handle.id)) {
                r->visual_alpha = 0.5f;
            }
        }

        // Move the preview to follow the cursor (cell-snap regardless
        // of footprint size — destructables visually land on 1/4-tile
        // positions, finer than buildings).
        const auto* def = m_simulation.types().get_destructable_type(m_preview_type_id);
        if (!def) return;
        f32 wx = map::snap_cell_x(td, m_cursor_pos.x);
        f32 wy = map::snap_cell_y(td, m_cursor_pos.y);
        if (auto* t = world.transforms.get(m_preview_handle.id)) {
            t->position.x = wx;
            t->position.y = wy;
            t->position.z = map::sample_height(td, wx, wy);
            t->prev_position = t->position;
        }
    } else {
        // Doodad. Same model+variation pattern as destructable, but no
        // footprint snap and no collision check at all.
        bool stale = m_preview_category != ObjectCategory::Doodad ||
                     m_preview_type_id  != m_place_doodad_type ||
                     m_preview_variation != m_place_doodad_var;
        if (stale) {
            destroy_preview();
            auto h = simulation::create_doodad(
                world, m_place_doodad_type,
                m_cursor_pos.x, m_cursor_pos.y, /*facing=*/0,
                m_place_doodad_var);
            if (!h.is_valid()) return;
            m_preview_handle.id         = h.id;
            m_preview_handle.generation = h.generation;
            m_preview_type_id           = m_place_doodad_type;
            m_preview_category          = ObjectCategory::Doodad;
            m_preview_variation         = m_place_doodad_var;
            if (auto* r = world.renderables.get(m_preview_handle.id)) {
                r->visual_alpha = 0.5f;
            }
        }
        if (auto* t = world.transforms.get(m_preview_handle.id)) {
            t->position.x = m_cursor_pos.x;
            t->position.y = m_cursor_pos.y;
            t->position.z = map::sample_height(td, m_cursor_pos.x, m_cursor_pos.y);
            t->prev_position = t->position;
        }
    }
}

void Editor::clear_selection() {
    m_selected_unit         = {};
    m_selected_item         = {};
    m_selected_destructable = {};
    m_selected_doodad       = {};
}

// Footprint overlap check. fw/fh are TILE units when in_cells=false,
// CELL units when in_cells=true. Both paths query the pathfinder's
// cell grid (is_cell_blocked); the tile path simply expands by
// PATHING_SUBDIV first.
bool Editor::footprint_clear_at(f32 wx, f32 wy, u32 fw, u32 fh, bool in_cells) const {
    if (fw == 0 || fh == 0) return true;
    const auto& td = m_map.terrain();
    if (!td.is_valid()) return true;
    f32 step = in_cells
        ? td.tile_size / static_cast<f32>(simulation::PATHING_SUBDIV)
        : td.tile_size;
    f32 left_f   = (wx - td.origin_x()) / step - 0.5f * static_cast<f32>(fw);
    f32 bottom_f = (wy - td.origin_y()) / step - 0.5f * static_cast<f32>(fh);
    i32 c0x = static_cast<i32>(std::round(left_f));
    i32 c0y = static_cast<i32>(std::round(bottom_f));
    // Iterate at cell granularity. Tile mode multiplies coords + extents.
    i32 step_cells = in_cells ? 1 : static_cast<i32>(simulation::PATHING_SUBDIV);
    i32 cw = static_cast<i32>(fw) * step_cells;
    i32 ch = static_cast<i32>(fh) * step_cells;
    i32 cx0 = c0x * step_cells;
    i32 cy0 = c0y * step_cells;
    for (i32 dy = 0; dy < ch; ++dy) {
        for (i32 dx = 0; dx < cw; ++dx) {
            i32 cx = cx0 + dx;
            i32 cy = cy0 + dy;
            if (cx < 0 || cy < 0) return false;
            if (m_simulation.pathfinder().is_cell_blocked(cx, cy)) return false;
        }
    }
    return true;
}

bool Editor::can_place_at(ObjectCategory cat, std::string_view type,
                          f32 wx, f32 wy) const {
    // Items don't have collision/footprint in our model — always allowed.
    if (cat == ObjectCategory::Item) return true;

    u32 fw = 0, fh = 0;
    f32 my_radius = 0.0f;
    if (cat == ObjectCategory::Unit) {
        const auto* def = m_simulation.types().get_unit_type(std::string(type));
        if (!def) return false;
        fw = def->pathing_footprint_w;
        fh = def->pathing_footprint_h;
        my_radius = def->collision_radius;
    } else if (cat == ObjectCategory::Destructable) {
        const auto* def = m_simulation.types().get_destructable_type(std::string(type));
        if (!def) return false;
        fw = def->pathing_footprint_w;
        fh = def->pathing_footprint_h;
        my_radius = def->collision_radius;
    }

    const auto& td = m_map.terrain();
    if (!td.is_valid()) return false;

    // Footprint clearance. Buildings (Unit category) author in TILES;
    // destructables author in CELLS. Both check against the pathfinder's
    // cell grid — the helper handles the unit conversion.
    if (fw > 0 && fh > 0) {
        bool in_cells = (cat == ObjectCategory::Destructable);
        if (!footprint_clear_at(wx, wy, fw, fh, in_cells)) return false;
    } else if (my_radius > 0) {
        // Non-footprint mobile object: refuse placement inside an
        // existing PathingBlocker's tile.
        i32 tx = static_cast<i32>(std::floor((wx - td.origin_x()) / td.tile_size));
        i32 ty = static_cast<i32>(std::floor((wy - td.origin_y()) / td.tile_size));
        if (tx < 0 || ty < 0) return false;
        if (m_simulation.pathfinder().is_tile_blocked(static_cast<u32>(tx),
                                                      static_cast<u32>(ty))) {
            return false;
        }
    }

    // Collision-radius overlap with any other entity. Skip the preview
    // entity (it sits at the cursor and would always fail). Also skip
    // self-via-handle isn't necessary here because this runs *before*
    // creation, so the new entity doesn't exist yet.
    if (my_radius > 0) {
        const auto& world = m_simulation.world();
        for (u32 i = 0; i < world.movements.count(); ++i) {
            u32 id = world.movements.ids()[i];
            if (id == m_preview_handle.id) continue;
            const auto* t = world.transforms.get(id);
            if (!t) continue;
            const auto& other = world.movements.data()[i];
            f32 dx = t->position.x - wx;
            f32 dy = t->position.y - wy;
            f32 min_dist = my_radius + other.collision_radius;
            if (dx * dx + dy * dy < min_dist * min_dist) return false;
        }
    }

    return true;
}

void Editor::place_unit_at(f32 wx, f32 wy) {
    if (!m_map_loaded || m_place_unit_type.empty()) return;
    auto& sim = m_simulation;
    auto& td  = m_map.terrain();
    if (!td.is_valid()) return;

    const auto* def = sim.types().get_unit_type(m_place_unit_type);
    if (!def) {
        log::warn(TAG, "place_unit_at: unknown type '{}'", m_place_unit_type);
        return;
    }

    // Buildings snap to the tile grid based on footprint parity.
    if (def->pathing_footprint_w > 0 && def->pathing_footprint_h > 0) {
        wx = map::snap_building_x(td, wx, def->pathing_footprint_w);
        wy = map::snap_building_y(td, wy, def->pathing_footprint_h);
    }
    // Unified validity gate: footprint clearance + collision-circle
    // overlap with existing entities.
    if (!can_place_at(ObjectCategory::Unit, m_place_unit_type, wx, wy)) return;

    f32 facing_rad = m_place_unit_facing_deg * (glm::pi<f32>() / 180.0f);
    auto unit = simulation::create_unit(sim.world(), m_place_unit_type,
                                         simulation::Player{m_place_unit_owner},
                                         wx, wy, facing_rad);
    if (!unit.is_valid()) return;

    if (auto* t = sim.world().transforms.get(unit.id)) {
        t->position.z = map::sample_height(td, wx, wy);
    }
    if (auto* mov = sim.world().movements.get(unit.id)) {
        u32 vx = std::min(static_cast<u32>(std::round((wx - td.origin_x()) / td.tile_size)), td.tiles_x);
        u32 vy = std::min(static_cast<u32>(std::round((wy - td.origin_y()) / td.tile_size)), td.tiles_y);
        mov->cliff_level = td.cliff_at(vx, vy);
    }

    // Buildings author footprints in TILES. We block tile-aligned but
    // store the resulting CELL rect in PathingBlocker so unblock callbacks
    // speak cells uniformly.
    if (def->pathing_footprint_w > 0 && def->pathing_footprint_h > 0) {
        f32 left_tx_f   = (wx - td.origin_x()) / td.tile_size - 0.5f * static_cast<f32>(def->pathing_footprint_w);
        f32 bottom_ty_f = (wy - td.origin_y()) / td.tile_size - 0.5f * static_cast<f32>(def->pathing_footprint_h);
        i32 tx0 = static_cast<i32>(std::round(left_tx_f));
        i32 ty0 = static_cast<i32>(std::round(bottom_ty_f));
        simulation::PathingBlocker blocker;
        blocker.cx = tx0 * static_cast<i32>(simulation::PATHING_SUBDIV);
        blocker.cy = ty0 * static_cast<i32>(simulation::PATHING_SUBDIV);
        blocker.w  = def->pathing_footprint_w * simulation::PATHING_SUBDIV;
        blocker.h  = def->pathing_footprint_h * simulation::PATHING_SUBDIV;
        sim.pathfinder().block_cells(blocker.cx, blocker.cy, blocker.w, blocker.h);
        sim.world().pathing_blockers.add(unit.id, std::move(blocker));
    }
    m_selected_unit = unit;
    m_selected_item = {};
    m_selected_destructable = {};
}

void Editor::place_item_at(f32 wx, f32 wy) {
    if (!m_map_loaded || m_place_item_type.empty()) return;
    auto& sim = m_simulation;
    auto& td  = m_map.terrain();
    if (!td.is_valid()) return;

    auto item = simulation::create_item(sim.world(), m_place_item_type, wx, wy);
    if (!item.is_valid()) return;
    if (auto* t = sim.world().transforms.get(item.id)) {
        t->position.z = map::sample_height(td, wx, wy);
        t->prev_position.z = t->position.z;
    }
    m_selected_item = item;
    m_selected_unit = {};
    m_selected_destructable = {};
}

void Editor::place_destructable_at(f32 wx, f32 wy) {
    if (!m_map_loaded || m_place_destructable_type.empty()) return;
    auto& sim = m_simulation;
    auto& td  = m_map.terrain();
    if (!td.is_valid()) return;

    const auto* def = sim.types().get_destructable_type(m_place_destructable_type);
    // Destructables snap to nearest cell (1/4 tile) regardless of
    // footprint size — they're finer-grained than buildings.
    wx = map::snap_cell_x(td, wx);
    wy = map::snap_cell_y(td, wy);
    if (!can_place_at(ObjectCategory::Destructable, m_place_destructable_type, wx, wy)) return;

    auto dest = simulation::create_destructable(sim.world(), m_place_destructable_type,
                                                 wx, wy, /*facing=*/0,
                                                 m_place_destructable_var);
    if (!dest.is_valid()) return;
    if (auto* t = sim.world().transforms.get(dest.id)) {
        t->position.z = map::sample_height(td, wx, wy);
    }

    // Destructables author footprints in CELLS. Center the rect on the
    // destructable's snapped (cell-center) position.
    if (def && def->pathing_footprint_w > 0 && def->pathing_footprint_h > 0) {
        f32 cs = sim.pathfinder().cell_size();
        f32 left_cx_f   = (wx - td.origin_x()) / cs - 0.5f * static_cast<f32>(def->pathing_footprint_w);
        f32 bottom_cy_f = (wy - td.origin_y()) / cs - 0.5f * static_cast<f32>(def->pathing_footprint_h);
        simulation::PathingBlocker blocker;
        blocker.cx = static_cast<i32>(std::round(left_cx_f));
        blocker.cy = static_cast<i32>(std::round(bottom_cy_f));
        blocker.w  = def->pathing_footprint_w;
        blocker.h  = def->pathing_footprint_h;
        sim.pathfinder().block_cells(blocker.cx, blocker.cy, blocker.w, blocker.h);
        sim.world().pathing_blockers.add(dest.id, std::move(blocker));
    }

    m_selected_destructable = dest;
    m_selected_unit = {};
    m_selected_item = {};
    m_selected_doodad = {};

    // Re-roll for the next placement so a series of clicks in random
    // mode lays down a mix of variations (no-op when random is off).
    roll_destructable_variation();
}

void Editor::place_doodad_at(f32 wx, f32 wy) {
    if (!m_map_loaded || m_place_doodad_type.empty()) return;
    auto& sim = m_simulation;
    auto& td  = m_map.terrain();
    if (!td.is_valid()) return;

    // No snap, no footprint check — doodads are decoration.
    auto dood = simulation::create_doodad(sim.world(), m_place_doodad_type,
                                           wx, wy, /*facing=*/0,
                                           m_place_doodad_var);
    if (!dood.is_valid()) return;
    if (auto* t = sim.world().transforms.get(dood.id)) {
        t->position.z = map::sample_height(td, wx, wy);
    }

    m_selected_doodad = dood;
    m_selected_unit = {};
    m_selected_item = {};
    m_selected_destructable = {};
    roll_doodad_variation();
}

// Pick the first entity within `pick_radius` game units of (wx, wy).
// O(N) scan — N is small for the editor's typical scene, fine for v1.
static u32 pick_entity_at(const simulation::World& world, f32 wx, f32 wy) {
    constexpr f32 PICK_RADIUS_SQ = 96.0f * 96.0f;
    u32 best_id = UINT32_MAX;
    f32 best_d2 = PICK_RADIUS_SQ;
    for (u32 i = 0; i < world.handle_infos.count(); ++i) {
        u32 id = world.handle_infos.ids()[i];
        const auto* t = world.transforms.get(id);
        if (!t) continue;
        f32 dx = t->position.x - wx;
        f32 dy = t->position.y - wy;
        f32 d2 = dx*dx + dy*dy;
        if (d2 < best_d2) { best_d2 = d2; best_id = id; }
    }
    return best_id;
}

void Editor::place_mode_on_left_click(f32 screen_x, f32 screen_y) {
    glm::vec3 hit;
    if (!raycast_terrain(screen_x, screen_y, hit)) return;

    if (m_object_tool == ObjectTool::Place) {
        switch (m_object_category) {
        case ObjectCategory::Unit:         place_unit_at(hit.x, hit.y); break;
        case ObjectCategory::Item:         place_item_at(hit.x, hit.y); break;
        case ObjectCategory::Destructable: place_destructable_at(hit.x, hit.y); break;
        case ObjectCategory::Doodad:       place_doodad_at(hit.x, hit.y); break;
        }
        m_has_last_placement = true;
        m_last_placement_pos = {hit.x, hit.y};
    } else {
        // Select: pick nearest entity within a fixed click radius.
        clear_selection();
        u32 picked = pick_entity_at(m_simulation.world(), hit.x, hit.y);
        if (picked == UINT32_MAX) return;
        const auto* info = m_simulation.world().handle_infos.get(picked);
        if (!info) return;
        switch (info->category) {
        case simulation::Category::Unit:         m_selected_unit.id         = picked; m_selected_unit.generation         = info->generation; break;
        case simulation::Category::Item:         m_selected_item.id         = picked; m_selected_item.generation         = info->generation; break;
        case simulation::Category::Destructable: m_selected_destructable.id = picked; m_selected_destructable.generation = info->generation; break;
        case simulation::Category::Doodad:       m_selected_doodad.id       = picked; m_selected_doodad.generation       = info->generation; break;
        default: break;
        }
    }
}

// Continuous-mode tick. Called every frame while the left mouse is
// held in Object/Place mode. For footprint objects, dedup is implicit:
// place_*_at refuses overlaps because footprint_clear_at fails. For
// free-form objects (mobile units, items), the cursor must move at
// least one "step" away from the last placement to avoid spamming.
void Editor::place_mode_continuous_tick(f32 screen_x, f32 screen_y) {
    if (m_object_tool != ObjectTool::Place) return;

    glm::vec3 hit;
    if (!raycast_terrain(screen_x, screen_y, hit)) return;

    // Pick a per-category step gating dedup for non-footprint objects.
    // Footprint objects (buildings, destructables with footprint > 0)
    // self-dedup via footprint_clear_at, so this threshold is moot for
    // them — but using a small value here ensures the cursor still has
    // to move a hair to trigger another attempt.
    f32 step_sq = 32.0f * 32.0f;
    bool has_footprint = false;
    if (m_object_category == ObjectCategory::Unit) {
        const auto* def = m_simulation.types().get_unit_type(m_place_unit_type);
        if (def) {
            has_footprint = (def->pathing_footprint_w > 0 && def->pathing_footprint_h > 0);
            if (!has_footprint && def->collision_radius > 0) {
                f32 r = def->collision_radius * 2.0f;
                step_sq = r * r;
            }
        }
    } else if (m_object_category == ObjectCategory::Destructable) {
        const auto* def = m_simulation.types().get_destructable_type(m_place_destructable_type);
        if (def) has_footprint = (def->pathing_footprint_w > 0 && def->pathing_footprint_h > 0);
    } else if (m_object_category == ObjectCategory::Doodad) {
        // Doodads use a fixed distance threshold; they have no
        // collision model to derive a "natural" step from. A tile-ish
        // step (128 game units) feels right for forest-scattering.
        step_sq = 128.0f * 128.0f;
    }

    if (m_has_last_placement && !has_footprint) {
        f32 dx = hit.x - m_last_placement_pos.x;
        f32 dy = hit.y - m_last_placement_pos.y;
        if (dx * dx + dy * dy < step_sq) return;
    }

    switch (m_object_category) {
    case ObjectCategory::Unit:         place_unit_at(hit.x, hit.y); break;
    case ObjectCategory::Item:         place_item_at(hit.x, hit.y); break;
    case ObjectCategory::Destructable: place_destructable_at(hit.x, hit.y); break;
    case ObjectCategory::Doodad:       place_doodad_at(hit.x, hit.y); break;
    }
    m_has_last_placement = true;
    m_last_placement_pos = {hit.x, hit.y};
}

void Editor::place_mode_on_delete() {
    auto& world = m_simulation.world();
    if (m_selected_unit.is_valid() && world.validate(m_selected_unit)) {
        simulation::destroy(world, m_selected_unit);
    } else if (m_selected_item.is_valid() && world.validate(m_selected_item)) {
        simulation::destroy(world, m_selected_item);
    } else if (m_selected_doodad.is_valid() && world.validate(m_selected_doodad)) {
        simulation::destroy(world, m_selected_doodad);
    } else if (m_selected_destructable.is_valid() && world.validate(m_selected_destructable)) {
        simulation::destroy(world, m_selected_destructable);
    }
    clear_selection();
}

void Editor::place_mode_draw_panel() {
    ImGui::Text("Object");
    ImGui::Separator();

    // Selection-edit mode: when the user has something selected in the
    // Select tool, the panel switches to editing THAT entity. Category
    // and Type are shown disabled (the entity is what it is); the
    // per-category editable fields write directly to the entity's
    // components. Place state (m_place_*_*) is left untouched.
    u32  sel_id    = UINT32_MAX;
    bool in_select = (m_object_tool == ObjectTool::Select);
    ObjectCategory sel_cat = ObjectCategory::Unit;
    if (in_select) {
        if      (m_selected_unit.is_valid())         { sel_id = m_selected_unit.id;         sel_cat = ObjectCategory::Unit; }
        else if (m_selected_item.is_valid())         { sel_id = m_selected_item.id;         sel_cat = ObjectCategory::Item; }
        else if (m_selected_destructable.is_valid()) { sel_id = m_selected_destructable.id; sel_cat = ObjectCategory::Destructable; }
        else if (m_selected_doodad.is_valid())       { sel_id = m_selected_doodad.id;       sel_cat = ObjectCategory::Doodad; }
    }
    bool has_selection = (sel_id != UINT32_MAX);
    ObjectCategory eff_cat = has_selection ? sel_cat : m_object_category;

    // Category dropdown (disabled while editing a selection — type-
    // changing an existing entity isn't supported).
    static const char* CAT_NAMES[] = { "Unit", "Item", "Destructable", "Doodad" };
    int cat = static_cast<int>(eff_cat);
    ImGui::BeginDisabled(has_selection);
    if (ImGui::Combo("Category", &cat, CAT_NAMES, IM_ARRAYSIZE(CAT_NAMES))) {
        m_object_category = static_cast<ObjectCategory>(cat);
    }
    ImGui::EndDisabled();

    // Tool radio (always active).
    int tool = static_cast<int>(m_object_tool);
    ImGui::RadioButton("Place",  &tool, static_cast<int>(ObjectTool::Place));  ImGui::SameLine();
    ImGui::RadioButton("Select", &tool, static_cast<int>(ObjectTool::Select));
    m_object_tool = static_cast<ObjectTool>(tool);

    // Continuous: only meaningful in Place mode.
    ImGui::BeginDisabled(m_object_tool != ObjectTool::Place);
    ImGui::Checkbox("Continuous", &m_place_continuous);
    ImGui::EndDisabled();

    ImGui::Separator();

    if (!m_map_loaded) {
        ImGui::TextDisabled("No map loaded");
        return;
    }
    auto& types = m_simulation.types();
    auto& world = m_simulation.world();

    auto type_combo = [&](const char* label, std::string& current, auto& type_map) {
        if (ImGui::BeginCombo(label, current.empty() ? "<none>" : current.c_str())) {
            for (const auto& [id, def] : type_map) {
                bool selected = (id == current);
                if (ImGui::Selectable(id.c_str(), selected)) current = id;
            }
            ImGui::EndCombo();
        }
    };

    // Type field: editable in Place mode, disabled in Select mode (so
    // the user can see what they have without being able to change it).
    auto disabled_type_combo = [&](const char* label, const std::string& fixed, auto& type_map) {
        ImGui::BeginDisabled(true);
        std::string t = fixed;
        type_combo(label, t, type_map);
        ImGui::EndDisabled();
    };

    // Per-category controls. Two flavors:
    //   • has_selection: bind editable widgets directly to the
    //     selected entity's components (writes are immediate).
    //   • otherwise:    edit the m_place_*_* placement state.
    switch (eff_cat) {
    case ObjectCategory::Unit: {
        if (has_selection) {
            const auto* info = world.handle_infos.get(sel_id);
            disabled_type_combo("Type", info ? info->type_id : std::string{}, types.unit_types());

            u32 nplayers = static_cast<u32>(m_map.manifest().players.size());
            if (nplayers == 0) nplayers = 1;
            if (auto* o = world.owners.get(sel_id)) {
                int owner = static_cast<int>(o->player.id);
                if (ImGui::SliderInt("Owner", &owner, 0, static_cast<int>(nplayers - 1))) {
                    o->player.id = static_cast<u32>(owner);
                }
            }
            if (auto* t = world.transforms.get(sel_id)) {
                f32 facing_deg = t->facing * (180.0f / glm::pi<f32>());
                if (ImGui::SliderFloat("Facing", &facing_deg, 0.0f, 360.0f, "%.0f°")) {
                    t->facing = facing_deg * (glm::pi<f32>() / 180.0f);
                    t->prev_facing = t->facing;
                }
            }
        } else {
            type_combo("Type", m_place_unit_type, types.unit_types());
            int owner = static_cast<int>(m_place_unit_owner);
            u32 nplayers = static_cast<u32>(m_map.manifest().players.size());
            if (nplayers == 0) nplayers = 1;
            ImGui::SliderInt("Owner", &owner, 0, static_cast<int>(nplayers - 1));
            m_place_unit_owner = static_cast<u32>(owner);
            ImGui::SliderFloat("Facing", &m_place_unit_facing_deg, 0.0f, 360.0f, "%.0f°");
        }
        break;
    }
    case ObjectCategory::Item: {
        if (has_selection) {
            const auto* info = world.handle_infos.get(sel_id);
            disabled_type_combo("Type", info ? info->type_id : std::string{}, types.item_types());
            // Items have no editable runtime properties yet.
        } else {
            type_combo("Type", m_place_item_type, types.item_types());
        }
        break;
    }
    case ObjectCategory::Destructable: {
        if (has_selection) {
            const auto* info = world.handle_infos.get(sel_id);
            std::string type_id = info ? info->type_id : std::string{};
            disabled_type_combo("Type", type_id, types.destructable_types());

            const auto* def = types.get_destructable_type(type_id);
            auto* dc = world.destructables.get(sel_id);
            auto* r  = world.renderables.get(sel_id);
            if (def && !def->models.empty() && dc) {
                int variation = static_cast<int>(dc->variation);
                int max_var = std::max(0, static_cast<int>(def->models.size()) - 1);
                if (ImGui::SliderInt("Variation", &variation, 0, max_var)) {
                    dc->variation = static_cast<u8>(variation);
                    if (r) r->model_path = def->models[static_cast<u32>(variation) % def->models.size()];
                }
            }
        } else {
            std::string prev_type = m_place_destructable_type;
            type_combo("Type", m_place_destructable_type, types.destructable_types());
            bool type_changed = (prev_type != m_place_destructable_type);
            const auto* def = types.get_destructable_type(m_place_destructable_type);
            int variations = def && !def->models.empty() ? static_cast<int>(def->models.size()) : 1;
            bool was_random = m_place_destructable_random;
            ImGui::Checkbox("Random variation", &m_place_destructable_random);
            bool random_toggled = (was_random != m_place_destructable_random);
            ImGui::BeginDisabled(m_place_destructable_random);
            int fixed = static_cast<int>(m_place_destructable_fixed_var);
            int max_var = std::max(0, variations - 1);
            if (ImGui::SliderInt("Variation", &fixed, 0, max_var)) {
                m_place_destructable_fixed_var = static_cast<u8>(fixed);
                if (!m_place_destructable_random) {
                    m_place_destructable_var = m_place_destructable_fixed_var;
                }
            }
            ImGui::EndDisabled();
            if (type_changed || random_toggled) {
                roll_destructable_variation();
            }
        }
        break;
    }
    case ObjectCategory::Doodad: {
        if (has_selection) {
            const auto* info = world.handle_infos.get(sel_id);
            std::string type_id = info ? info->type_id : std::string{};
            disabled_type_combo("Type", type_id, types.doodad_types());

            const auto* def = types.get_doodad_type(type_id);
            auto* dc = world.doodads.get(sel_id);
            auto* r  = world.renderables.get(sel_id);
            if (def && !def->models.empty() && dc) {
                int variation = static_cast<int>(dc->variation);
                int max_var = std::max(0, static_cast<int>(def->models.size()) - 1);
                if (ImGui::SliderInt("Variation", &variation, 0, max_var)) {
                    dc->variation = static_cast<u8>(variation);
                    if (r) r->model_path = def->models[static_cast<u32>(variation) % def->models.size()];
                }
            }
        } else {
            std::string prev_type = m_place_doodad_type;
            type_combo("Type", m_place_doodad_type, types.doodad_types());
            bool type_changed = (prev_type != m_place_doodad_type);
            const auto* def = types.get_doodad_type(m_place_doodad_type);
            int variations = def && !def->models.empty() ? static_cast<int>(def->models.size()) : 1;
            bool was_random = m_place_doodad_random;
            ImGui::Checkbox("Random variation", &m_place_doodad_random);
            bool random_toggled = (was_random != m_place_doodad_random);
            ImGui::BeginDisabled(m_place_doodad_random);
            int fixed = static_cast<int>(m_place_doodad_fixed_var);
            int max_var = std::max(0, variations - 1);
            if (ImGui::SliderInt("Variation", &fixed, 0, max_var)) {
                m_place_doodad_fixed_var = static_cast<u8>(fixed);
                if (!m_place_doodad_random) {
                    m_place_doodad_var = m_place_doodad_fixed_var;
                }
            }
            ImGui::EndDisabled();
            if (type_changed || random_toggled) {
                roll_doodad_variation();
            }
        }
        break;
    }
    }

    ImGui::Separator();
    if (has_selection) {
        if (auto* t = world.transforms.get(sel_id)) {
            ImGui::Text("Pos: (%.0f, %.0f)", t->position.x, t->position.y);
        }
        ImGui::TextDisabled("Press Delete to remove");
    } else {
        ImGui::TextDisabled("Click on terrain to place;");
        ImGui::TextDisabled("switch to Select to pick existing.");
    }
}


// ── Undo/Redo ────────────────────────────────────────────────────────────

void Editor::begin_stroke() {
    if (m_stroke_active || !m_map_loaded) return;
    m_stroke_active = true;

    // Generous padded region around cursor — large enough to cover a typical drag.
    // Uses a fixed padding so even if the user drags, most edits fit.
    auto& td = m_map.terrain();
    static constexpr i32 PAD = 32;  // vertices of padding beyond brush
    i32 r = m_brush_size + PAD;
    u32 min_vx = static_cast<u32>(std::max(0, m_cursor_vx - r));
    u32 min_vy = static_cast<u32>(std::max(0, m_cursor_vy - r));
    u32 max_vx = static_cast<u32>(std::min(static_cast<i32>(td.tiles_x), m_cursor_vx + r));
    u32 max_vy = static_cast<u32>(std::min(static_cast<i32>(td.tiles_y), m_cursor_vy + r));

    TerrainEdit edit;
    edit.min_vx = min_vx;
    edit.min_vy = min_vy;
    edit.max_vx = max_vx;
    edit.max_vy = max_vy;
    copy_region(td, min_vx, min_vy, max_vx, max_vy,
                edit.old_heightmap, edit.old_cliff, edit.old_tile_layer, edit.old_pathing);

    m_undo_stack.push_back(std::move(edit));
}

void Editor::end_stroke() {
    if (!m_stroke_active || m_undo_stack.empty()) return;
    m_stroke_active = false;

    auto& td = m_map.terrain();
    auto& edit = m_undo_stack.back();

    // Capture new state of the same region
    copy_region(td, edit.min_vx, edit.min_vy, edit.max_vx, edit.max_vy,
                edit.new_heightmap, edit.new_cliff, edit.new_tile_layer, edit.new_pathing);

    // Discard if nothing changed
    if (edit.old_heightmap == edit.new_heightmap &&
        edit.old_cliff == edit.new_cliff &&
        edit.old_tile_layer == edit.new_tile_layer &&
        edit.old_pathing == edit.new_pathing) {
        m_undo_stack.pop_back();
        return;
    }

    m_redo_stack.clear();
    if (m_undo_stack.size() > MAX_UNDO) {
        m_undo_stack.erase(m_undo_stack.begin());
    }
}

void Editor::apply_edit(const TerrainEdit& edit, bool use_new) {
    auto& td = m_map.terrain();
    if (use_new) {
        paste_region(td, edit.min_vx, edit.min_vy, edit.max_vx, edit.max_vy,
                     edit.new_heightmap, edit.new_cliff, edit.new_tile_layer, edit.new_pathing);
    } else {
        paste_region(td, edit.min_vx, edit.min_vy, edit.max_vx, edit.max_vy,
                     edit.old_heightmap, edit.old_cliff, edit.old_tile_layer, edit.old_pathing);
    }
    m_terrain_dirty = true;
}

void Editor::undo() {
    if (m_undo_stack.empty()) return;
    auto edit = std::move(m_undo_stack.back());
    m_undo_stack.pop_back();
    apply_edit(edit, false);  // restore old values
    m_redo_stack.push_back(std::move(edit));
}

void Editor::redo() {
    if (m_redo_stack.empty()) return;
    auto edit = std::move(m_redo_stack.back());
    m_redo_stack.pop_back();
    apply_edit(edit, true);  // restore new values
    m_undo_stack.push_back(std::move(edit));
}

// ── Terrain mesh rebuild ─────────────────────────────────────────────────

void Editor::rebuild_terrain_mesh() {
    if (!m_map_loaded) return;
    m_rhi.wait_idle();
    m_renderer.set_terrain(&m_map.terrain());
    m_simulation.set_terrain(&m_map.terrain());

    // Update entities after terrain edit: adjust Z, remove units on impassable tiles
    auto& td = m_map.terrain();
    auto& world = m_simulation.world();
    std::vector<simulation::Unit> to_remove;

    for (u32 i = 0; i < world.transforms.count(); ++i) {
        u32 id = world.transforms.ids()[i];
        auto& t = world.transforms.data()[i];

        auto* info = world.handle_infos.get(id);
        if (!info || info->category != simulation::Category::Unit) continue;

        glm::ivec2 tile = td.world_to_tile(t.position.x, t.position.y);
        if (!td.is_tile_passable(tile.x, tile.y)) {
            simulation::Unit u;
            u.id = id;
            u.generation = info->generation;
            to_remove.push_back(u);
        } else {
            t.position.z = map::sample_height(td, t.position.x, t.position.y);
        }
    }

    for (auto u : to_remove) {
        simulation::destroy(world, u);
    }
}

void Editor::switch_scene(const std::string& scene_name) {
    if (!m_map_loaded || scene_name == m_current_scene) return;

    m_rhi.wait_idle();

    if (m_map.switch_scene(scene_name, m_asset, m_simulation)) {
        m_current_scene = scene_name;
        if (m_map.terrain().is_valid()) {
            m_renderer.set_terrain(&m_map.terrain());
            m_simulation.set_terrain(&m_map.terrain());
        }
        // Region focus tied to the previous scene's index space — drop
        // it so the panel doesn't try to read a deleted region.
        region_set_focus(-1);
        m_region_drag_active = false;
        log::info(TAG, "Switched to scene: {}", scene_name);
    }
}

void Editor::open_map(const std::string& path) {
    // Drop entity handles while the outgoing world is still populated so
    // destroy_preview's validate() can still find the preview entity.
    destroy_preview();
    m_preview_type_id.clear();
    m_preview_variation = 0;
    m_selected_unit = {};
    m_selected_item = {};
    m_selected_destructable = {};
    m_selected_doodad = {};

    // Mid-stroke brush / drag / placement / region interaction state would
    // otherwise carry into the new map (e.g. a paint stroke continuing on
    // a fresh tile layer, an in-progress region rect snapping to new geometry).
    m_brush_applied      = false;
    m_last_brush_vx      = -1;
    m_last_brush_vy      = -1;
    m_cursor_valid       = false;
    m_stroke_active      = false;
    m_terrain_dirty      = false;
    m_drag_active        = false;
    m_place_continuous   = false;
    m_has_last_placement = false;
    m_region_focus       = -1;
    m_region_drag_active = false;

    // TerrainEdit records hold vertex indices into the outgoing terrain;
    // replaying them against a different-size new terrain writes OOB.
    m_undo_stack.clear();
    m_redo_stack.clear();

    // Null out renderer + simulation terrain pointers before unload_map
    // destroys the TerrainData they currently reference.
    m_renderer.set_terrain(nullptr);
    m_simulation.set_terrain(nullptr);

    // Renderer model cache + bone buffers + mega-buffer rollback — without
    // this, same-relative-path models in the new map return stale cached
    // entries pointing into the old map's bytes (collapsed meshes).
    m_renderer.end_session();

    // Type / ability registries accumulate across maps because
    // load_unit_types_from_doc inserts via operator[] (overwrites duplicate
    // keys, leaves orphans). Dev binary handles this via
    // GameServer::shutdown → Simulation::shutdown; editor owns its own
    // Simulation and must wipe + re-init explicitly.
    m_simulation.shutdown();
    m_simulation.init(m_asset);

    m_map.unload_map();
    m_map_loaded = false;
    m_scenes.clear();
    m_current_scene.clear();

    m_map_path = path;
    if (m_map.load_map(m_map_path, m_asset, m_simulation, /*allow_directory=*/true)) {
        m_map_loaded = true;
        m_scenes = m_map.list_scenes();
        m_current_scene = m_map.manifest().start_scene;
        m_renderer.set_map_root(m_map.map_root());
        m_renderer.load_tileset_textures(m_map.tileset());
        m_renderer.set_environment(m_map.manifest().environment);
        if (m_map.terrain().is_valid()) {
            std::vector<u8> shallow, deep;
            for (auto& layer : m_map.tileset().layers) {
                if (layer.type == map::LayerType::WaterShallow)
                    shallow.push_back(static_cast<u8>(layer.id));
                else if (layer.type == map::LayerType::WaterDeep)
                    deep.push_back(static_cast<u8>(layer.id));
            }
            m_map.terrain().set_water_layers(shallow, deep);
            m_renderer.set_terrain(&m_map.terrain());
            m_simulation.set_terrain(&m_map.terrain());
        }
        log::info(TAG, "Opened map: {}", m_map_path);
    } else {
        log::error(TAG, "Failed to open map: {}", m_map_path);
    }
}

// Win32 folder picker
static std::string pick_folder(HWND hwnd) {
    std::string result;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                   IID_IFileDialog, reinterpret_cast<void**>(&dialog)))) {
        DWORD options;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS);
        dialog->SetTitle(L"Open Map Folder (.uldmap)");

        if (SUCCEEDED(dialog->Show(hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path_w = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path_w))) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, path_w, -1, nullptr, 0, nullptr, nullptr);
                    result.resize(len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, path_w, -1, result.data(), len, nullptr, nullptr);
                    CoTaskMemFree(path_w);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    CoUninitialize();
    return result;
}

// Run uldum_pack.exe (sibling of this exe) with the given argument tail.
// Returns the subprocess exit code, or -1 if launch failed.
static int run_uldum_pack(const std::string& args) {
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::filesystem::path pack = std::filesystem::path(exe_path).parent_path() / "uldum_pack.exe";

    std::string cmd = "\"" + pack.string() + "\" " + args;

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(rc);
}

// Run a sibling exe (looked up next to uldum_editor.exe), piping its
// stdout + stderr into `output`. Returns the process exit code, or -1
// if launch failed. Synchronous — fine for short-running tools like
// basisu (~hundreds of ms for a typical texture).
static int run_capture(const std::string& exe_name, const std::string& args,
                       std::string& output) {
    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::filesystem::path tool =
        std::filesystem::path(exe_path).parent_path() / exe_name;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE read_h = nullptr, write_h = nullptr;
    if (!CreatePipe(&read_h, &write_h, &sa, 0)) return -1;
    SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "\"" + tool.string() + "\" " + args;

    STARTUPINFOA si{ sizeof(si) };
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = write_h;
    si.hStdError  = write_h;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(read_h); CloseHandle(write_h);
        return -1;
    }
    // Drop the parent's copy of the write end so ReadFile returns EOF
    // once the child exits — otherwise we'd hang waiting on a handle
    // we still own.
    CloseHandle(write_h);

    char buf[1024];
    DWORD n = 0;
    while (ReadFile(read_h, buf, sizeof(buf), &n, nullptr) && n > 0) {
        output.append(buf, n);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_h);
    return static_cast<int>(rc);
}

// Win32 open-file picker (for opening a packed .uldmap).
static std::string pick_open_file(HWND hwnd, const wchar_t* title,
                                  const wchar_t* filter_label, const wchar_t* filter_pattern) {
    std::string result;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                   IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog)))) {
        dialog->SetTitle(title);
        COMDLG_FILTERSPEC spec = { filter_label, filter_pattern };
        dialog->SetFileTypes(1, &spec);

        if (SUCCEEDED(dialog->Show(hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path_w = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path_w))) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, path_w, -1, nullptr, 0, nullptr, nullptr);
                    result.resize(len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, path_w, -1, result.data(), len, nullptr, nullptr);
                    CoTaskMemFree(path_w);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    CoUninitialize();
    return result;
}

// Win32 save-file picker. Returns selected path (may lack extension).
static std::string pick_save_file(HWND hwnd, const wchar_t* title, const wchar_t* default_name,
                                  const wchar_t* filter_label, const wchar_t* filter_pattern) {
    std::string result;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IFileSaveDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL,
                                   IID_IFileSaveDialog, reinterpret_cast<void**>(&dialog)))) {
        dialog->SetTitle(title);
        if (default_name && *default_name) dialog->SetFileName(default_name);
        COMDLG_FILTERSPEC spec = { filter_label, filter_pattern };
        dialog->SetFileTypes(1, &spec);
        dialog->SetDefaultExtension(L"uldmap");

        if (SUCCEEDED(dialog->Show(hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path_w = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path_w))) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, path_w, -1, nullptr, 0, nullptr, nullptr);
                    result.resize(len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, path_w, -1, result.data(), len, nullptr, nullptr);
                    CoTaskMemFree(path_w);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    CoUninitialize();
    return result;
}

// ── Overlays (grid, brush cursor) ────────────────────────────────────────

bool Editor::world_to_screen(glm::vec3 world_pos, ImVec2& screen_pos) const {
    glm::mat4 vp = m_renderer.camera().view_projection();
    glm::vec4 clip = vp * glm::vec4(world_pos, 1.0f);
    if (clip.w <= 0.0f) return false;  // behind camera

    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    f32 w = static_cast<f32>(m_rhi.extent().width);
    f32 h = static_cast<f32>(m_rhi.extent().height);

    screen_pos.x = (ndc.x * 0.5f + 0.5f) * w;
    screen_pos.y = (ndc.y * 0.5f + 0.5f) * h;  // Vulkan NDC: top = -1, bottom = +1
    return ndc.z >= 0.0f && ndc.z <= 1.0f;
}

void Editor::draw_overlays() {
    if (!m_map_loaded || !m_map.terrain().is_valid()) return;

    auto* draw_list = ImGui::GetForegroundDrawList();
    auto& td = m_map.terrain();

    // Helper: closed polyline that traces a horizontal circle, sampled
    // along terrain so it hugs hills. Emitted through EditorOverlays
    // (depth-tested 3D lines) — terrain, units, and destructable
    // models all occlude it properly.
    auto add_world_circle = [&](glm::vec3 center, f32 radius, glm::vec4 color) {
        constexpr u32 SEG = 48;
        std::vector<glm::vec3> samples;
        samples.reserve(SEG);
        for (u32 i = 0; i < SEG; ++i) {
            f32 a = (static_cast<f32>(i) / SEG) * 2.0f * glm::pi<f32>();
            f32 wx = center.x + std::cos(a) * radius;
            f32 wy = center.y + std::sin(a) * radius;
            samples.push_back({ wx, wy, map::sample_height(td, wx, wy) });
        }
        m_overlays.add_polyline(samples, color, /*closed=*/true);
    };

    // Brush overlays: only when mouse is not over UI
    if (m_cursor_valid && !ImGui::GetIO().WantCaptureMouse && m_mode == EditMode::Terrain) {
        auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);

        const glm::vec4 grid_color{ 1.0f, 1.0f, 1.0f, 0.35f };

        // Grid: tile grid centered on cursor vertex — drawn through
        // EditorOverlays so the wires hide behind hills correctly.
        i32 brush_r = m_brush_size - 1;
        i32 grid_min_x = std::max(0, m_cursor_vx - brush_r);
        i32 grid_max_x = std::min(static_cast<i32>(td.tiles_x), m_cursor_vx + brush_r);
        i32 grid_min_y = std::max(0, m_cursor_vy - brush_r);
        i32 grid_max_y = std::min(static_cast<i32>(td.tiles_y), m_cursor_vy + brush_r);

        for (i32 iy = grid_min_y; iy <= grid_max_y; ++iy) {
            for (i32 ix = grid_min_x; ix < grid_max_x; ++ix) {
                glm::vec3 a{td.vertex_world_x(ix),     td.vertex_world_y(iy), td.world_z_at(ix, iy)};
                glm::vec3 b{td.vertex_world_x(ix + 1), td.vertex_world_y(iy), td.world_z_at(ix + 1, iy)};
                m_overlays.add_line(a, b, grid_color);
            }
        }
        for (i32 ix = grid_min_x; ix <= grid_max_x; ++ix) {
            for (i32 iy = grid_min_y; iy < grid_max_y; ++iy) {
                glm::vec3 a{td.vertex_world_x(ix), td.vertex_world_y(iy),     td.world_z_at(ix, iy)};
                glm::vec3 b{td.vertex_world_x(ix), td.vertex_world_y(iy + 1), td.world_z_at(ix, iy + 1)};
                m_overlays.add_line(a, b, grid_color);
            }
        }

        // Highlight affected vertices as dots with falloff. Screen-
        // space ImGui is fine for these — they're small disc markers,
        // not lines that need terrain occlusion.
        for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
            for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
                i32 dx = static_cast<i32>(ix) - m_cursor_vx;
                i32 dy = static_cast<i32>(iy) - m_cursor_vy;
                f32 w = tile_falloff(dx, dy, m_brush_size);
                if (w <= 0.0f) continue;

                glm::vec3 vpos{td.vertex_world_x(ix), td.vertex_world_y(iy), td.world_z_at(ix, iy) + 2.0f};
                ImVec2 sp;
                if (world_to_screen(vpos, sp)) {
                    f32 dot_r = 2.0f + w * 4.0f;
                    u8 alpha = static_cast<u8>(100 + w * 155);
                    draw_list->AddCircleFilled(sp, dot_r, IM_COL32(0, 255, 128, alpha));
                }
            }
        }

        // Center vertex marker
        {
            glm::vec3 cpos{td.vertex_world_x(m_cursor_vx), td.vertex_world_y(m_cursor_vy),
                           td.world_z_at(m_cursor_vx, m_cursor_vy) + 3.0f};
            ImVec2 sp;
            if (world_to_screen(cpos, sp)) {
                draw_list->AddCircle(sp, 8.0f, IM_COL32(255, 255, 0, 255), 0, 2.0f);
            }
        }
    }

    // Object-mode preview: ghost of what would be placed under the
    // cursor. Buildings show their snapped footprint rectangle so the
    // user sees exactly which tiles will be occupied; units show a
    // collision-radius circle; items show a small dot. Select tool
    // skips the preview (you're picking, not placing).
    if (m_cursor_valid && !ImGui::GetIO().WantCaptureMouse &&
        m_mode == EditMode::Object && m_object_tool == ObjectTool::Place) {

        // Tile-aligned grid covering a (fw × fh) footprint at (cx, cy).
        // Each line is subdivided so it hugs terrain curvature. step =
        // tile_size for buildings, cell_size for destructables.
        auto add_footprint_grid = [&](f32 cx, f32 cy, u32 fw, u32 fh, glm::vec4 color, f32 step) {
            const f32 ts = step;
            const f32 x0 = cx - 0.5f * fw * ts;
            const f32 y0 = cy - 0.5f * fh * ts;
            constexpr u32 SUB = 4;

            std::vector<glm::vec3> samples;
            for (u32 i = 0; i <= fw; ++i) {
                f32 x = x0 + static_cast<f32>(i) * ts;
                samples.clear();
                samples.reserve(fh * SUB + 1);
                for (u32 j = 0; j <= fh * SUB; ++j) {
                    f32 y = y0 + (static_cast<f32>(j) / SUB) * ts;
                    samples.push_back({ x, y, map::sample_height(td, x, y) });
                }
                m_overlays.add_polyline(samples, color);
            }
            for (u32 j = 0; j <= fh; ++j) {
                f32 y = y0 + static_cast<f32>(j) * ts;
                samples.clear();
                samples.reserve(fw * SUB + 1);
                for (u32 i = 0; i <= fw * SUB; ++i) {
                    f32 x = x0 + (static_cast<f32>(i) / SUB) * ts;
                    samples.push_back({ x, y, map::sample_height(td, x, y) });
                }
                m_overlays.add_polyline(samples, color);
            }
        };

        f32 px = m_cursor_pos.x, py = m_cursor_pos.y;
        // Footprint grid colors: green = placement valid, red = blocked.
        // Items don't have footprints; pink ring stays neutral.
        const glm::vec4 ok_color  { 0.39f, 0.90f, 0.47f, 0.86f };
        const glm::vec4 bad_color { 0.90f, 0.31f, 0.31f, 0.86f };
        const glm::vec4 item_color{ 1.00f, 0.39f, 0.86f, 0.86f };

        switch (m_object_category) {
        case ObjectCategory::Unit: {
            const auto* def = m_simulation.types().get_unit_type(m_place_unit_type);
            if (!def) break;
            f32 sx = px, sy = py;
            if (def->pathing_footprint_w > 0 && def->pathing_footprint_h > 0) {
                sx = map::snap_building_x(td, px, def->pathing_footprint_w);
                sy = map::snap_building_y(td, py, def->pathing_footprint_h);
            }
            bool ok = can_place_at(ObjectCategory::Unit, m_place_unit_type, sx, sy);
            if (def->pathing_footprint_w > 0 && def->pathing_footprint_h > 0) {
                add_footprint_grid(sx, sy, def->pathing_footprint_w, def->pathing_footprint_h,
                                   ok ? ok_color : bad_color, td.tile_size);
            } else if (!ok && def->collision_radius > 0) {
                add_world_circle({ sx, sy, 0 }, def->collision_radius, bad_color);
            }
            break;
        }
        case ObjectCategory::Item:
            add_world_circle({ px, py, 0 }, 24.0f, item_color);
            break;
        case ObjectCategory::Destructable: {
            const auto* def = m_simulation.types().get_destructable_type(m_place_destructable_type);
            if (!def) break;
            f32 sx = map::snap_cell_x(td, px);
            f32 sy = map::snap_cell_y(td, py);
            bool ok = can_place_at(ObjectCategory::Destructable, m_place_destructable_type, sx, sy);
            if (def->pathing_footprint_w > 0 && def->pathing_footprint_h > 0) {
                f32 cs = td.tile_size / static_cast<f32>(simulation::PATHING_SUBDIV);
                add_footprint_grid(sx, sy, def->pathing_footprint_w, def->pathing_footprint_h,
                                   ok ? ok_color : bad_color, cs);
            } else if (!ok && def->collision_radius > 0) {
                add_world_circle({ sx, sy, 0 }, def->collision_radius, bad_color);
            }
            break;
        }
        case ObjectCategory::Doodad:
            // Doodads have no collision/footprint — the ghost model
            // following the cursor is the only indicator.
            break;
        }
    }

    // Selection ring: ground-hugging circle around the currently
    // selected entity. Only drawn in the Select tool — during Place
    // the ghost preview is the relevant indicator. Radius prefers
    // `selection_radius` (the authored visual extent, matches HUD).
    // Falls back to `collision_radius * 1.2` so the ring sits just
    // outside the physical collider when the type didn't set a
    // selection radius (e.g. destructables).
    if (m_mode == EditMode::Object && m_object_tool == ObjectTool::Select) {
        u32 sel_id = UINT32_MAX;
        if      (m_selected_unit.is_valid())         sel_id = m_selected_unit.id;
        else if (m_selected_item.is_valid())         sel_id = m_selected_item.id;
        else if (m_selected_destructable.is_valid()) sel_id = m_selected_destructable.id;
        else if (m_selected_doodad.is_valid())       sel_id = m_selected_doodad.id;
        if (sel_id != UINT32_MAX) {
            const auto& world = m_simulation.world();
            const auto* t = world.transforms.get(sel_id);
            if (t) {
                f32 radius = 32.0f;
                if (auto* s = world.selectables.get(sel_id); s && s->selection_radius > 1.5f) {
                    radius = s->selection_radius;
                } else if (auto* m = world.movements.get(sel_id); m && m->collision_radius > 0) {
                    radius = m->collision_radius * 1.2f;
                }
                const glm::vec4 sel_color{ 1.00f, 0.90f, 0.31f, 1.00f };
                add_world_circle({ t->position.x, t->position.y, 0 }, radius, sel_color);
            }
        }
    }

    // Region outlines: every authored region is drawn whenever the
    // editor is in Region mode. The focused region gets a brighter
    // tint; others are dim so they don't fight the foreground. Rects
    // are subdivided so the wire follows terrain instead of flying
    // straight through hills.
    if (m_mode == EditMode::Region) {
        const auto& regions = m_map.scene().regions;
        const glm::vec4 col_idle    { 0.40f, 0.70f, 1.00f, 0.55f };
        const glm::vec4 col_focused { 0.20f, 0.95f, 1.00f, 1.00f };
        const glm::vec4 col_preview { 0.95f, 0.95f, 0.40f, 0.95f };
        for (i32 i = 0; i < static_cast<i32>(regions.size()); ++i) {
            glm::vec4 c = (i == m_region_focus) ? col_focused : col_idle;
            for (const auto& rc : regions[i].rects) {
                // Sample each edge so the line hugs terrain curvature.
                constexpr u32 SUB = 8;
                std::vector<glm::vec3> samples;
                samples.reserve(SUB * 4 + 1);
                auto edge = [&](f32 ax, f32 ay, f32 bx, f32 by) {
                    for (u32 s = 0; s < SUB; ++s) {
                        f32 t = static_cast<f32>(s) / SUB;
                        f32 x = ax + (bx - ax) * t;
                        f32 y = ay + (by - ay) * t;
                        samples.push_back({ x, y, map::sample_height(td, x, y) });
                    }
                };
                edge(rc.x0, rc.y0, rc.x1, rc.y0);
                edge(rc.x1, rc.y0, rc.x1, rc.y1);
                edge(rc.x1, rc.y1, rc.x0, rc.y1);
                edge(rc.x0, rc.y1, rc.x0, rc.y0);
                m_overlays.add_polyline(samples, c, /*closed=*/true);
            }
            for (const auto& circ : regions[i].circles) {
                add_world_circle({ circ.cx, circ.cy, 0 }, circ.r, c);
            }
        }
        // Live preview of the in-progress drag (yellow). Released on
        // mouse-up; the committed shape then renders through the loop
        // above on subsequent frames.
        if (m_region_drag_active) {
            const glm::vec2 a = m_region_drag_start;
            const glm::vec2 b = m_region_drag_current;
            if (m_region_tool == RegionTool::AddRect) {
                f32 x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
                f32 x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
                constexpr u32 SUB = 8;
                std::vector<glm::vec3> samples;
                samples.reserve(SUB * 4 + 1);
                auto edge = [&](f32 ax, f32 ay, f32 bx, f32 by) {
                    for (u32 s = 0; s < SUB; ++s) {
                        f32 t = static_cast<f32>(s) / SUB;
                        f32 x = ax + (bx - ax) * t;
                        f32 y = ay + (by - ay) * t;
                        samples.push_back({ x, y, map::sample_height(td, x, y) });
                    }
                };
                edge(x0, y0, x1, y0);
                edge(x1, y0, x1, y1);
                edge(x1, y1, x0, y1);
                edge(x0, y1, x0, y0);
                m_overlays.add_polyline(samples, col_preview, /*closed=*/true);
            } else if (m_region_tool == RegionTool::AddCircle) {
                f32 dx = b.x - a.x, dy = b.y - a.y;
                f32 r = std::sqrt(dx * dx + dy * dy);
                if (r > 0.5f) add_world_circle({ a.x, a.y, 0 }, r, col_preview);
            }
        }
    }
}

// ── UI ───────────────────────────────────────────────────────────────────

void Editor::draw_ui() {
    // Main menu bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Map Folder...")) {
                std::string path = pick_folder(static_cast<HWND>(m_platform->native_window_handle()));
                if (!path.empty()) {
                    open_map(path);
                }
            }
            if (ImGui::MenuItem("Open Packed Map...")) {
                std::string path = pick_open_file(
                    static_cast<HWND>(m_platform->native_window_handle()),
                    L"Open Packed Map", L"Uldum map package (*.uldmap)", L"*.uldmap");
                if (!path.empty()) {
                    open_map(path);
                }
            }
            if (ImGui::MenuItem("Save Map", nullptr, false, m_map_loaded)) {
                namespace fs = std::filesystem;
                std::string map_root_abs = fs::absolute(m_map.map_root()).string();

                // The Object-mode ghost preview is a real sim entity;
                // destroy it before save_objects iterates the world,
                // otherwise it leaks into objects.json. Recreated by
                // update_object_preview on the next frame.
                destroy_preview();

                if (fs::is_directory(m_map.map_root())) {
                    // Source-folder mode: write terrain.bin + objects.json
                    // directly into the source tree.
                    std::string terrain_path = m_map.map_root() + "/scenes/" +
                        m_current_scene + "/terrain.bin";
                    map::save_terrain(m_map.terrain(), terrain_path);
                    log::info(TAG, "Saved terrain to {}", terrain_path);
                    m_map.save_objects(m_simulation.world(), m_current_scene);
                } else {
                    // Normal mode (packed .uldmap): unpack → overwrite terrain.bin
                    // + objects.json → repack → reload.
                    fs::path staging = fs::temp_directory_path() /
                        ("uldum_save_" + std::to_string(GetCurrentProcessId()));
                    std::error_code ec;
                    fs::remove_all(staging, ec);
                    fs::create_directories(staging, ec);

                    int rc = run_uldum_pack("unpack \"" + map_root_abs + "\" \"" + staging.string() + "\"");
                    if (rc != 0) {
                        log::error(TAG, "Save: unpack failed (exit {})", rc);
                    } else {
                        std::string terrain_path = (staging / "scenes" / m_current_scene / "terrain.bin").string();
                        bool ok = map::save_terrain(m_map.terrain(), terrain_path);
                        if (!ok) {
                            log::error(TAG, "Save: failed to write terrain to '{}'", terrain_path);
                        } else {
                            // Temporarily point map_root at the staging dir so save_objects
                            // writes into the unpacked folder rather than the .uldmap path.
                            std::string original_root = m_map.map_root();
                            m_map.set_map_root_for_save(staging.string());
                            m_map.save_objects(m_simulation.world(), m_current_scene);
                            m_map.set_map_root_for_save(original_root);

                            rc = run_uldum_pack("pack \"" + staging.string() + "\" \"" + map_root_abs + "\"");
                            if (rc != 0) {
                                log::error(TAG, "Save: pack failed (exit {})", rc);
                            } else {
                                log::info(TAG, "Saved packed map '{}'", map_root_abs);
                                // Reload so in-memory package entries match the new file.
                                open_map(map_root_abs);
                            }
                        }
                    }
                    fs::remove_all(staging, ec);
                }
            }
            // Export to packed .uldmap — only meaningful when editing a source folder.
            bool can_export = m_map_loaded && std::filesystem::is_directory(m_map.map_root());
            if (ImGui::MenuItem("Export Packed Map...", nullptr, false, can_export)) {
                std::filesystem::path src(m_map.map_root());
                std::wstring default_name = src.filename().wstring();
                if (!default_name.ends_with(L".uldmap")) default_name += L".uldmap";

                std::string out = pick_save_file(
                    static_cast<HWND>(m_platform->native_window_handle()),
                    L"Export Packed Map", default_name.c_str(),
                    L"Uldum map package (*.uldmap)", L"*.uldmap");
                if (!out.empty()) {
                    int rc = run_uldum_pack("pack \"" + std::filesystem::absolute(m_map.map_root()).string()
                                            + "\" \"" + out + "\"");
                    if (rc == 0) log::info(TAG, "Exported packed map to '{}'", out);
                    else         log::error(TAG, "Export failed (exit {})", rc);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Import PNG...", nullptr, false, m_map_loaded)) {
                m_import_open = true;
                // Sensible defaults: textures land under textures/ unless
                // the author overrides. Filename gets derived from the
                // source PNG once one is picked.
                if (m_import_path[0] == '\0') {
                    constexpr const char* def = "textures";
                    std::memcpy(m_import_path.data(), def, std::strlen(def) + 1);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_undo_stack.empty())) undo();
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_redo_stack.empty())) redo();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Mode")) {
            if (ImGui::MenuItem("Terrain", "T", m_mode == EditMode::Terrain)) {
                m_mode = EditMode::Terrain;
                clear_selection();
            }
            if (ImGui::MenuItem("Object",  "U", m_mode == EditMode::Object)) {
                m_mode = EditMode::Object;
            }
            if (ImGui::MenuItem("Region",  "R", m_mode == EditMode::Region)) {
                m_mode = EditMode::Region;
                clear_selection();
                m_region_drag_active = false;
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Scene window
    if (m_map_loaded && !m_scenes.empty()) {
        ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        for (auto& scene : m_scenes) {
            bool selected = (scene == m_current_scene);
            if (ImGui::RadioButton(scene.c_str(), selected)) {
                if (!selected) switch_scene(scene);
            }
        }
        ImGui::End();
    }

    // Tool panel — content depends on the active edit mode.
    ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260, 460), ImGuiCond_FirstUseEver);
    ImGui::Begin("Tools");

    if (m_mode == EditMode::Object) {
        place_mode_draw_panel();
    } else if (m_mode == EditMode::Region) {
        region_mode_panel();
    } else {
    ImGui::Text("Terrain");
    ImGui::Separator();

    int tool = static_cast<int>(m_tool);
    ImGui::RadioButton("Raise",       &tool, static_cast<int>(Tool::Raise));
    ImGui::RadioButton("Lower",       &tool, static_cast<int>(Tool::Lower));
    ImGui::RadioButton("Smooth",      &tool, static_cast<int>(Tool::Smooth));
    ImGui::RadioButton("Flatten",     &tool, static_cast<int>(Tool::Flatten));
    ImGui::RadioButton("Paint",       &tool, static_cast<int>(Tool::Paint));
    ImGui::RadioButton("Cliff Raise", &tool, static_cast<int>(Tool::CliffRaise));
    ImGui::RadioButton("Cliff Lower", &tool, static_cast<int>(Tool::CliffLower));
    ImGui::RadioButton("Ramp Set",    &tool, static_cast<int>(Tool::RampSet));
    ImGui::RadioButton("Ramp Clear",  &tool, static_cast<int>(Tool::RampClear));
    m_tool = static_cast<Tool>(tool);

    ImGui::Separator();
    ImGui::SliderInt("Size", &m_brush_size, 1, 11);

    bool is_one_shot = m_tool == Tool::CliffRaise || m_tool == Tool::CliffLower ||
                       m_tool == Tool::RampSet || m_tool == Tool::RampClear ||
                       m_tool == Tool::Paint;
    if (!is_one_shot) {
        ImGui::SliderFloat("Amount", &m_brush_amount, 1.0f, 32.0f, "%.0f");
        ImGui::Checkbox("Continuous", &m_continuous);
        if (m_continuous) {
            ImGui::SliderFloat("Speed", &m_brush_speed, 1.0f, 200.0f, "%.0f u/s");
        }
    }

    if (m_tool == Tool::Flatten) {
        ImGui::SliderFloat("Target H", &m_flatten_height, -256.0f, 256.0f);
    }

    if (m_tool == Tool::Paint) {
        auto& tl = m_map.tileset().layers;
        if (!tl.empty()) {
            // Build combo from tileset layer names
            std::string combo_str;
            for (auto& l : tl) {
                combo_str += std::to_string(l.id) + ": " + l.name;
                combo_str += '\0';
            }
            combo_str += '\0';
            ImGui::Combo("Layer", &m_paint_layer, combo_str.c_str());
        } else {
            const char* fallback[] = {"Layer 0", "Layer 1", "Layer 2", "Layer 3"};
            ImGui::Combo("Layer", &m_paint_layer, fallback, 4);
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("Right-click drag to pan camera");
    } // end Terrain-mode tool palette

    ImGui::End();

    // Info panel
    ImGui::SetNextWindowPos(ImVec2(10, 440), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 160), ImGuiCond_FirstUseEver);
    ImGui::Begin("Info");
    if (m_map_loaded) {
        auto& td = m_map.terrain();
        ImGui::Text("Map: %s", m_map.manifest().name.c_str());
        std::string abs_path = std::filesystem::absolute(m_map.map_root()).string();
        bool is_dir = std::filesystem::is_directory(m_map.map_root());
        ImGui::TextWrapped("Path: %s", abs_path.c_str());
        ImGui::Text("Mode: %s", is_dir ? "source folder" : "packed .uldmap");
        ImGui::Text("Scene: %s", m_current_scene.c_str());
        ImGui::Text("Terrain: %ux%u tiles", td.tiles_x, td.tiles_y);
    }
    if (m_cursor_valid && m_map_loaded) {
        auto& td = m_map.terrain();
        u8 flags = td.pathing_at(m_cursor_vx, m_cursor_vy);
        ImGui::Text("Cursor: %d, %d", m_cursor_vx, m_cursor_vy);
        ImGui::Text("World: %.0f, %.0f, %.0f",
                     m_cursor_pos.x, m_cursor_pos.y, m_cursor_pos.z);
        ImGui::Text("Height: %.1f  Cliff: %d",
                     td.height_at(m_cursor_vx, m_cursor_vy),
                     td.cliff_at(m_cursor_vx, m_cursor_vy));
        ImGui::Text("Ramp:%s", (flags & map::PATHING_RAMP) ? "Y" : "N");

        // Layer at the cursor vertex — id, tileset name, and LayerType.
        u32 vi = m_cursor_vy * td.verts_x() + m_cursor_vx;
        u8 layer_id = (vi < td.tile_layer.size()) ? td.tile_layer[vi] : 0;
        const map::TilesetLayer* tl = m_map.tileset().get_layer(layer_id);
        const char* type_str = "(unknown)";
        const char* name_str = "(missing)";
        if (tl) {
            name_str = tl->name.c_str();
            switch (tl->type) {
                case map::LayerType::Ground:       type_str = "ground"; break;
                case map::LayerType::WaterShallow: type_str = "water_shallow"; break;
                case map::LayerType::WaterDeep:    type_str = "water_deep"; break;
                case map::LayerType::Grass:        type_str = "grass"; break;
            }
        }
        ImGui::Text("Layer: %u '%s' [%s]", layer_id, name_str, type_str);
        ImGui::Text("Water: %s%s",
                     td.is_water(m_cursor_vx, m_cursor_vy) ? "yes" : "no",
                     td.is_deep_water(m_cursor_vx, m_cursor_vy) ? " (deep)" : "");
    } else {
        ImGui::Text("Cursor: (off terrain)");
    }
    ImGui::Text("FPS: %.0f", ImGui::GetIO().Framerate);
    ImGui::End();

    draw_import_dialog();
}

// ── Asset import ─────────────────────────────────────────────────────────

void Editor::draw_import_dialog() {
    if (m_import_open) {
        ImGui::OpenPopup("Import PNG");
        m_import_open = false;  // single-shot — popup state lives in ImGui from here
    }

    ImGui::SetNextWindowSize(ImVec2(560, 240), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Import PNG", nullptr,
                               ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("Convert a PNG to KTX2 (UASTC, mipmapped) and write "
                           "it into the loaded map's directory tree.");
        ImGui::Separator();

        // Source row — text input + Browse button.
        ImGui::InputText("##src", m_import_src.data(), m_import_src.size());
        ImGui::SameLine();
        if (ImGui::Button("Browse##src")) {
            std::string path = pick_open_file(
                static_cast<HWND>(m_platform->native_window_handle()),
                L"Pick a PNG", L"PNG image (*.png)", L"*.png");
            if (!path.empty()) {
                std::size_t n = std::min(path.size(), m_import_src.size() - 1);
                std::memcpy(m_import_src.data(), path.data(), n);
                m_import_src[n] = '\0';
                // Default the output filename from the source stem on
                // first pick — leave it alone if the author already
                // typed something so we don't trample their edit.
                if (m_import_filename[0] == '\0') {
                    std::filesystem::path src(path);
                    std::string stem = src.stem().string() + ".ktx2";
                    std::size_t m = std::min(stem.size(), m_import_filename.size() - 1);
                    std::memcpy(m_import_filename.data(), stem.data(), m);
                    m_import_filename[m] = '\0';
                }
                // Detect source dimensions (header-only read). Used to
                // seed the resize fields with a sensible default; if
                // detection fails the prior values (or 256×256) stay.
                int sw = 0, sh = 0, comp = 0;
                if (stbi_info(path.c_str(), &sw, &sh, &comp) && sw > 0 && sh > 0) {
                    m_import_src_w = sw;
                    m_import_src_h = sh;
                    if (!m_import_resize) {
                        m_import_w = sw;
                        m_import_h = sh;
                    }
                } else {
                    m_import_src_w = 0;
                    m_import_src_h = 0;
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Source PNG");

        ImGui::InputText("Path",
                         m_import_path.data(), m_import_path.size());
        ImGui::SameLine();
        ImGui::TextDisabled("(relative to map root, e.g. textures/icons/abilities)");

        ImGui::InputText("Filename",
                         m_import_filename.data(), m_import_filename.size());

        ImGui::Separator();
        int cs = m_import_linear ? 1 : 0;
        ImGui::RadioButton("sRGB (color)", &cs, 0); ImGui::SameLine();
        ImGui::RadioButton("Linear (normal/data)", &cs, 1);
        m_import_linear = (cs == 1);

        // Resize: opt-in. basisu's -resample uses a box filter; fine
        // for downscaling icons / texture atlases. Source dimensions
        // show as a hint when detected via stbi_info on Browse.
        ImGui::Checkbox("Resize", &m_import_resize);
        if (m_import_src_w > 0 && m_import_src_h > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(source: %d \xC3\x97 %d)", m_import_src_w, m_import_src_h);
        }
        if (!m_import_resize) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("##rw", &m_import_w, 0, 0);
        ImGui::SameLine();
        ImGui::TextUnformatted("\xC3\x97");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("##rh", &m_import_h, 0, 0);
        if (m_import_w < 1) m_import_w = 1;
        if (m_import_h < 1) m_import_h = 1;
        if (!m_import_resize) ImGui::EndDisabled();

        ImGui::Separator();
        bool can_convert = m_map_loaded
                        && m_import_src[0] != '\0'
                        && m_import_filename[0] != '\0';
        if (!can_convert) ImGui::BeginDisabled();
        if (ImGui::Button("Convert", ImVec2(120, 0))) {
            run_png_import();
            // Close the import dialog and queue the result modal so
            // the author gets an unambiguous Success / Fail readout
            // instead of a tool-output dump.
            ImGui::CloseCurrentPopup();
            m_import_show_result = true;
        }
        if (!can_convert) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(80, 0))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (m_import_show_result) {
        ImGui::OpenPopup("Import Result");
        m_import_show_result = false;
    }

    ImGui::SetNextWindowSize(ImVec2(440, 200), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Import Result", nullptr,
                               ImGuiWindowFlags_NoSavedSettings)) {
        if (m_import_result == ImportResult::Success) {
            ImGui::TextColored(ImVec4(0.40f, 0.95f, 0.40f, 1.0f), "Success");
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f), "Failed");
        }
        ImGui::Separator();
        ImGui::TextWrapped("%s", m_import_result_msg.c_str());
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            m_import_result = ImportResult::None;
            m_import_result_msg.clear();
        }
        ImGui::EndPopup();
    }
}

void Editor::run_png_import() {
    namespace fs = std::filesystem;
    m_import_result_msg.clear();

    fs::path src(m_import_src.data());
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) {
        m_import_result = ImportResult::Failure;
        m_import_result_msg = "Source file not found:\n  " + src.string();
        return;
    }
    if (!m_map_loaded || m_map.map_root().empty()) {
        m_import_result = ImportResult::Failure;
        m_import_result_msg = "No map loaded.";
        return;
    }
    fs::path dest_dir = fs::path(m_map.map_root()) / m_import_path.data();
    fs::create_directories(dest_dir, ec);
    if (ec) {
        m_import_result = ImportResult::Failure;
        m_import_result_msg = "Failed to create directory:\n  "
                            + dest_dir.string() + "\n  " + ec.message();
        return;
    }
    fs::path dest = dest_dir / m_import_filename.data();

    // Match scripts/png_to_ktx2.ps1: zstd is always off (runtime
    // transcoder limitation), UASTC level 2, mipmaps on. -linear
    // flips colorspace handling for data textures.
    std::string args = "-ktx2 -ktx2_no_zstandard -uastc -uastc_level 2 -mipmap";
    if (m_import_linear) args += " -linear";
    if (m_import_resize && m_import_w > 0 && m_import_h > 0) {
        args += " -resample " + std::to_string(m_import_w)
              + " "           + std::to_string(m_import_h);
    }
    args += " -output_file \"" + dest.string() + "\" \"" + src.string() + "\"";

    std::string captured;
    int rc = run_capture("basisu.exe", args, captured);
    if (rc == 0) {
        m_import_result = ImportResult::Success;
        m_import_result_msg = "Wrote " + dest.string();
        log::info(TAG, "Import: wrote '{}'", dest.string());
    } else {
        m_import_result = ImportResult::Failure;
        // Surface basisu's own message on failure so the user can act
        // on a missing file / bad format / unwritable destination
        // without digging through logs.
        m_import_result_msg = "basisu exit code " + std::to_string(rc);
        if (!captured.empty()) m_import_result_msg += "\n\n" + captured;
        log::error(TAG, "Import: basisu failed (exit {}) for '{}'",
                   rc, src.string());
    }
}

// ── ImGui teardown ───────────────────────────────────────────────────────

void Editor::shutdown_imgui() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (m_imgui_pool) {
        vkDestroyDescriptorPool(m_rhi.device(), m_imgui_pool, nullptr);
        m_imgui_pool = VK_NULL_HANDLE;
    }
}

// ── Region mode ──────────────────────────────────────────────────────────

void Editor::region_set_focus(i32 index) {
    auto& regions = m_map.mutable_regions();
    if (index < 0 || index >= static_cast<i32>(regions.size())) {
        m_region_focus = -1;
        m_region_id_buf[0] = '\0';
        return;
    }
    m_region_focus = index;
    const std::string& id = regions[index].id;
    std::size_t n = std::min(id.size(), m_region_id_buf.size() - 1);
    std::memcpy(m_region_id_buf.data(), id.data(), n);
    m_region_id_buf[n] = '\0';
}

bool Editor::region_id_in_use(std::string_view id, i32 ignore_index) const {
    const auto& regions = m_map.scene().regions;
    for (i32 i = 0; i < static_cast<i32>(regions.size()); ++i) {
        if (i == ignore_index) continue;
        if (regions[i].id == id) return true;
    }
    return false;
}

// Pick an unused "region_N" id. N starts at 1 and increments past
// every collision; avoids stepping on the user's hand-typed ids.
static std::string make_unique_region_id(const std::vector<map::Region>& regions) {
    for (u32 n = 1;; ++n) {
        std::string candidate = "region_" + std::to_string(n);
        bool clash = false;
        for (const auto& r : regions) {
            if (r.id == candidate) { clash = true; break; }
        }
        if (!clash) return candidate;
    }
}

void Editor::region_mode_on_left_press() {
    auto& regions = m_map.mutable_regions();
    const f32 wx = m_cursor_pos.x;
    const f32 wy = m_cursor_pos.y;

    if (m_region_tool == RegionTool::Select) {
        // Hit-test in reverse (top-of-list draws first; clicking
        // through overlapping regions should pick the visually
        // most-recent one).
        for (i32 i = static_cast<i32>(regions.size()) - 1; i >= 0; --i) {
            const auto& r = regions[i];
            for (const auto& rc : r.rects) {
                if (wx >= rc.x0 && wx <= rc.x1 && wy >= rc.y0 && wy <= rc.y1) {
                    region_set_focus(i);
                    return;
                }
            }
            for (const auto& c : r.circles) {
                f32 dx = wx - c.cx, dy = wy - c.cy;
                if (dx * dx + dy * dy <= c.r * c.r) {
                    region_set_focus(i);
                    return;
                }
            }
        }
        // Empty space click clears focus.
        region_set_focus(-1);
        return;
    }

    // AddRect / AddCircle: ensure a region is focused (auto-create one
    // if not). Then begin the drag — release commits the shape.
    if (m_region_focus < 0) {
        map::Region r;
        r.id = make_unique_region_id(regions);
        regions.push_back(std::move(r));
        region_set_focus(static_cast<i32>(regions.size()) - 1);
    }
    m_region_drag_active  = true;
    m_region_drag_start   = {wx, wy};
    m_region_drag_current = {wx, wy};
}

void Editor::region_mode_on_left_release() {
    m_region_drag_active = false;
    if (m_region_focus < 0) return;
    auto& regions = m_map.mutable_regions();
    if (m_region_focus >= static_cast<i32>(regions.size())) return;
    auto& region = regions[m_region_focus];

    const glm::vec2 a = m_region_drag_start;
    const glm::vec2 b = m_region_drag_current;

    if (m_region_tool == RegionTool::AddRect) {
        map::RegionRect rc{
            std::min(a.x, b.x), std::min(a.y, b.y),
            std::max(a.x, b.x), std::max(a.y, b.y),
        };
        // Reject zero-area drags so a stray click doesn't litter the
        // region with degenerate shapes.
        if ((rc.x1 - rc.x0) < 1.0f || (rc.y1 - rc.y0) < 1.0f) return;
        region.rects.push_back(rc);
    } else if (m_region_tool == RegionTool::AddCircle) {
        f32 dx = b.x - a.x, dy = b.y - a.y;
        f32 r = std::sqrt(dx * dx + dy * dy);
        if (r < 1.0f) return;
        region.circles.push_back({a.x, a.y, r});
    }
}

void Editor::region_mode_on_delete() {
    if (m_region_focus < 0) return;
    auto& regions = m_map.mutable_regions();
    if (m_region_focus >= static_cast<i32>(regions.size())) return;
    regions.erase(regions.begin() + m_region_focus);
    region_set_focus(-1);
}

void Editor::region_mode_panel() {
    ImGui::Text("Region");
    ImGui::Separator();

    int tool = static_cast<int>(m_region_tool);
    ImGui::RadioButton("Select",     &tool, static_cast<int>(RegionTool::Select));
    ImGui::RadioButton("Add Rect",   &tool, static_cast<int>(RegionTool::AddRect));
    ImGui::RadioButton("Add Circle", &tool, static_cast<int>(RegionTool::AddCircle));
    m_region_tool = static_cast<RegionTool>(tool);

    ImGui::Separator();

    auto& regions = m_map.mutable_regions();
    ImGui::Text("Regions (%d)", static_cast<int>(regions.size()));
    if (ImGui::Button("New Region")) {
        map::Region r;
        r.id = make_unique_region_id(regions);
        regions.push_back(std::move(r));
        region_set_focus(static_cast<i32>(regions.size()) - 1);
    }

    ImGui::BeginChild("region_list", ImVec2(0, 100), true);
    for (i32 i = 0; i < static_cast<i32>(regions.size()); ++i) {
        bool selected = (i == m_region_focus);
        ImGui::PushID(i);
        if (ImGui::Selectable(regions[i].id.c_str(), selected)) {
            region_set_focus(i);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (m_region_focus < 0 || m_region_focus >= static_cast<i32>(regions.size())) {
        ImGui::TextDisabled("(no region selected)");
        return;
    }

    auto& region = regions[m_region_focus];

    // ID field with live dup-id rejection. Red-tint and refuse the
    // commit when the typed string collides with another region.
    bool dup = region_id_in_use(m_region_id_buf.data(), m_region_focus);
    if (dup) ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(110, 30, 30, 255));
    if (ImGui::InputText("ID", m_region_id_buf.data(), m_region_id_buf.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string candidate(m_region_id_buf.data());
        if (!candidate.empty() && !region_id_in_use(candidate, m_region_focus)) {
            region.id = std::move(candidate);
        } else {
            // Revert buffer to the canonical id on bad commit.
            std::size_t n = std::min(region.id.size(), m_region_id_buf.size() - 1);
            std::memcpy(m_region_id_buf.data(), region.id.data(), n);
            m_region_id_buf[n] = '\0';
        }
    }
    if (dup) ImGui::PopStyleColor();
    if (dup) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "duplicate");
    }

    ImGui::Separator();
    ImGui::Text("Shapes:");
    // Iterate rects then circles, each row with a delete button.
    for (i32 i = 0; i < static_cast<i32>(region.rects.size()); ++i) {
        const auto& rc = region.rects[i];
        ImGui::PushID(i + 0x10000);
        ImGui::Text("Rect [%.0f,%.0f]–[%.0f,%.0f]", rc.x0, rc.y0, rc.x1, rc.y1);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            region.rects.erase(region.rects.begin() + i);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    for (i32 i = 0; i < static_cast<i32>(region.circles.size()); ++i) {
        const auto& c = region.circles[i];
        ImGui::PushID(i + 0x20000);
        ImGui::Text("Circle (%.0f,%.0f) r=%.0f", c.cx, c.cy, c.r);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            region.circles.erase(region.circles.begin() + i);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }

    if (region.rects.empty() && region.circles.empty()) {
        ImGui::TextDisabled("  (empty — use Add Rect / Add Circle)");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Delete key: remove selected region");
    ImGui::TextDisabled("Right-click drag to pan");
}

// ── Cleanup ──────────────────────────────────────────────────────────────

void Editor::shutdown() {
    log::info(TAG, "=== Shutting down Editor ===");

    shutdown_imgui();
    m_map.shutdown();
    m_simulation.shutdown();
    m_overlays.shutdown();
    m_renderer.shutdown();
    m_asset.shutdown();
    m_rhi.shutdown();
    m_platform->shutdown();

    log::info(TAG, "=== Editor shut down ===");
}

} // namespace uldum::editor
