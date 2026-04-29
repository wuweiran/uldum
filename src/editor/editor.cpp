#include "editor/editor.h"
#include "map/map.h"
#include "map/terrain_data.h"
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
            m_renderer.set_terrain(m_map.terrain());
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

    VkFormat color_format = m_rhi.swapchain_format();
    VkFormat depth_format = m_rhi.depth_format();
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

        // Apply brush when left-clicking on terrain (and not over ImGui)
        if (input.mouse_left && m_cursor_valid && !over_ui) {
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
        draw_overlays();

        // Render. Minimized window → cmd is null / extent is zero; we still
        // need to balance the ImGui::NewFrame() above with EndFrame() so
        // next loop iteration doesn't assert on a stale frame.
        VkCommandBuffer cmd = m_rhi.begin_frame();
        if (cmd && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
            m_renderer.draw_shadows(cmd, m_simulation.world());
            m_rhi.begin_rendering();
            m_renderer.draw(cmd, m_rhi.extent(), m_simulation.world());
            imgui_render(cmd);
            m_rhi.end_frame();
        } else {
            ImGui::EndFrame();
        }

        frame_count++;
    }

    vkDeviceWaitIdle(m_rhi.device());
    log::info(TAG, "Exiting editor loop ({} frames)", frame_count);
}

void Editor::imgui_new_frame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Editor::imgui_render(VkCommandBuffer cmd) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

// ── Ray-terrain intersection ─────────────────────────────────────────────

bool Editor::raycast_terrain(f32 screen_x, f32 screen_y, glm::vec3& hit) const {
    if (!m_map_loaded || !m_map.terrain().is_valid()) return false;

    auto& cam = m_renderer.camera();
    f32 w = static_cast<f32>(m_rhi.extent().width);
    f32 h = static_cast<f32>(m_rhi.extent().height);
    if (w <= 0 || h <= 0) return false;

    // NDC (-1 to 1)
    f32 ndc_x = (2.0f * screen_x / w) - 1.0f;
    f32 ndc_y = (2.0f * screen_y / h) - 1.0f;  // Vulkan: top=0, bottom=h

    // Unproject to world ray
    glm::mat4 inv_vp = glm::inverse(cam.view_projection());
    glm::vec4 near_clip{ndc_x, ndc_y, 0.0f, 1.0f};
    glm::vec4 far_clip{ndc_x, ndc_y, 1.0f, 1.0f};

    glm::vec4 near_world = inv_vp * near_clip;
    glm::vec4 far_world  = inv_vp * far_clip;
    near_world /= near_world.w;
    far_world  /= far_world.w;

    glm::vec3 ray_origin = glm::vec3(near_world);
    glm::vec3 ray_dir = glm::normalize(glm::vec3(far_world) - ray_origin);

    // Step along ray until we hit the terrain (simple marching)
    auto& td = m_map.terrain();
    f32 step = td.tile_size * 0.5f;
    f32 max_dist = 100000.0f;

    for (f32 t = 0.0f; t < max_dist; t += step) {
        glm::vec3 p = ray_origin + ray_dir * t;

        // Out of terrain bounds?
        if (p.x < td.origin_x() || p.y < td.origin_y()
            || p.x > td.origin_x() + td.world_width()
            || p.y > td.origin_y() + td.world_height())
            continue;

        // Sample terrain height at this XY (grid coords are 0-based; shift
        // from centered world to grid space).
        f32 fx = (p.x - td.origin_x()) / td.tile_size;
        f32 fy = (p.y - td.origin_y()) / td.tile_size;
        u32 ix = std::min(static_cast<u32>(fx), td.tiles_x - 1);
        u32 iy = std::min(static_cast<u32>(fy), td.tiles_y - 1);
        f32 lx = fx - static_cast<f32>(ix);
        f32 ly = fy - static_cast<f32>(iy);

        f32 h00 = td.world_z_at(ix, iy);
        f32 h10 = td.world_z_at(ix + 1, iy);
        f32 h01 = td.world_z_at(ix, iy + 1);
        f32 h11 = td.world_z_at(ix + 1, iy + 1);
        f32 terrain_z = h00 + lx * (h10 - h00) + ly * (h01 - h00) + lx * ly * (h00 - h10 - h01 + h11);

        if (p.z <= terrain_z) {
            hit = p;
            hit.z = terrain_z;
            return true;
        }
    }

    return false;
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
    case Tool::PathingBlock:
        if (!m_brush_applied) { brush_pathing_block(); m_brush_applied = true; }
        break;
    case Tool::PathingAllow:
        if (!m_brush_applied) { brush_pathing_allow(); m_brush_applied = true; }
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


void Editor::brush_pathing_block() {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);
    i32 r = m_brush_size - 1;
    i32 r2 = r * r;

    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            i32 dx = static_cast<i32>(ix) - m_cursor_vx;
            i32 dy = static_cast<i32>(iy) - m_cursor_vy;
            if (dx * dx + dy * dy > r2) continue;

            td.pathing_at(ix, iy) &= ~map::PATHING_WALKABLE;
            m_terrain_dirty = true;
            m_pathing_cache_dirty = true;
        }
    }
}

void Editor::brush_pathing_allow() {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);
    i32 r = m_brush_size - 1;
    i32 r2 = r * r;

    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            i32 dx = static_cast<i32>(ix) - m_cursor_vx;
            i32 dy = static_cast<i32>(iy) - m_cursor_vy;
            if (dx * dx + dy * dy > r2) continue;

            td.pathing_at(ix, iy) |= map::PATHING_WALKABLE;
            m_terrain_dirty = true;
            m_pathing_cache_dirty = true;
        }
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
    m_pathing_cache_dirty = true;
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
    vkDeviceWaitIdle(m_rhi.device());
    m_renderer.set_terrain(m_map.terrain());
    m_simulation.set_terrain(&m_map.terrain());
    m_pathing_cache_dirty = true;

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

void Editor::rebuild_pathing_cache() {
    m_blocked_tiles.clear();
    m_blocked_verts.clear();
    if (!m_map_loaded || !m_map.terrain().is_valid()) return;
    auto& td = m_map.terrain();

    for (u32 ty = 0; ty < td.tiles_y; ++ty) {
        for (u32 tx = 0; tx < td.tiles_x; ++tx) {
            bool w00 = (td.pathing_at(tx, ty)     & map::PATHING_WALKABLE) != 0;
            bool w10 = (td.pathing_at(tx+1, ty)   & map::PATHING_WALKABLE) != 0;
            bool w01 = (td.pathing_at(tx, ty+1)   & map::PATHING_WALKABLE) != 0;
            bool w11 = (td.pathing_at(tx+1, ty+1) & map::PATHING_WALKABLE) != 0;
            if (!w00 || !w10 || !w01 || !w11)
                m_blocked_tiles.push_back({tx, ty});
        }
    }

    for (u32 vy = 0; vy <= td.tiles_y; ++vy) {
        for (u32 vx = 0; vx <= td.tiles_x; ++vx) {
            if (!(td.pathing_at(vx, vy) & map::PATHING_WALKABLE))
                m_blocked_verts.push_back({vx, vy});
        }
    }

    m_pathing_cache_dirty = false;
}

void Editor::switch_scene(const std::string& scene_name) {
    if (!m_map_loaded || scene_name == m_current_scene) return;

    vkDeviceWaitIdle(m_rhi.device());

    if (m_map.switch_scene(scene_name, m_asset, m_simulation)) {
        m_current_scene = scene_name;
        if (m_map.terrain().is_valid()) {
            m_renderer.set_terrain(m_map.terrain());
            m_simulation.set_terrain(&m_map.terrain());
        }
        m_pathing_cache_dirty = true;
        log::info(TAG, "Switched to scene: {}", scene_name);
    }
}

void Editor::open_map(const std::string& path) {
    vkDeviceWaitIdle(m_rhi.device());

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
            m_renderer.set_terrain(m_map.terrain());
            m_simulation.set_terrain(&m_map.terrain());
        }
        m_pathing_cache_dirty = true;
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

    // Brush overlays: only when mouse is not over UI
    if (m_cursor_valid && !ImGui::GetIO().WantCaptureMouse) {
        auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);

        ImU32 grid_color = IM_COL32(255, 255, 255, 50);

        // Grid: tile grid centered on cursor vertex
        i32 brush_r = m_brush_size - 1;
        i32 grid_min_x = std::max(0, m_cursor_vx - brush_r);
        i32 grid_max_x = std::min(static_cast<i32>(td.tiles_x), m_cursor_vx + brush_r);
        i32 grid_min_y = std::max(0, m_cursor_vy - brush_r);
        i32 grid_max_y = std::min(static_cast<i32>(td.tiles_y), m_cursor_vy + brush_r);

        // Horizontal lines
        for (i32 iy = grid_min_y; iy <= grid_max_y; ++iy) {
            for (i32 ix = grid_min_x; ix < grid_max_x; ++ix) {
                glm::vec3 a{td.vertex_world_x(ix),     td.vertex_world_y(iy), td.world_z_at(ix, iy) + 1.0f};
                glm::vec3 b{td.vertex_world_x(ix + 1), td.vertex_world_y(iy), td.world_z_at(ix + 1, iy) + 1.0f};
                ImVec2 sa, sb;
                if (world_to_screen(a, sa) && world_to_screen(b, sb))
                    draw_list->AddLine(sa, sb, grid_color);
            }
        }
        // Vertical lines
        for (i32 ix = grid_min_x; ix <= grid_max_x; ++ix) {
            for (i32 iy = grid_min_y; iy < grid_max_y; ++iy) {
                glm::vec3 a{td.vertex_world_x(ix), td.vertex_world_y(iy),     td.world_z_at(ix, iy) + 1.0f};
                glm::vec3 b{td.vertex_world_x(ix), td.vertex_world_y(iy + 1), td.world_z_at(ix, iy + 1) + 1.0f};
                ImVec2 sa, sb;
                if (world_to_screen(a, sa) && world_to_screen(b, sb))
                    draw_list->AddLine(sa, sb, grid_color);
            }
        }

        // Highlight affected vertices as dots with falloff
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

    // Pathing visualization — toggled via View menu
    if (m_show_pathing) {
        if (m_pathing_cache_dirty) rebuild_pathing_cache();

        auto* fg = ImGui::GetForegroundDrawList();
        ImU32 fill_color = IM_COL32(0, 0, 0, 60);
        ImU32 dot_color  = IM_COL32(0, 0, 0, 200);
        f32 z_off = 2.0f;
        f32 sw = static_cast<f32>(m_rhi.extent().width);
        f32 sh = static_cast<f32>(m_rhi.extent().height);

        for (auto& [tx, ty] : m_blocked_tiles) {
            glm::vec3 c00{td.vertex_world_x(tx),     td.vertex_world_y(ty),     td.world_z_at(tx, ty) + z_off};
            glm::vec3 c10{td.vertex_world_x(tx + 1), td.vertex_world_y(ty),     td.world_z_at(tx+1, ty) + z_off};
            glm::vec3 c01{td.vertex_world_x(tx),     td.vertex_world_y(ty + 1), td.world_z_at(tx, ty+1) + z_off};
            glm::vec3 c11{td.vertex_world_x(tx + 1), td.vertex_world_y(ty + 1), td.world_z_at(tx+1, ty+1) + z_off};

            ImVec2 s00, s10, s01, s11;
            if (!world_to_screen(c00, s00) || !world_to_screen(c10, s10) ||
                !world_to_screen(c01, s01) || !world_to_screen(c11, s11)) continue;

            // Quick screen bounds check
            f32 min_x = std::min({s00.x, s10.x, s01.x, s11.x});
            f32 max_x = std::max({s00.x, s10.x, s01.x, s11.x});
            f32 min_y = std::min({s00.y, s10.y, s01.y, s11.y});
            f32 max_y = std::max({s00.y, s10.y, s01.y, s11.y});
            if (max_x < 0 || min_x > sw || max_y < 0 || min_y > sh) continue;

            ImVec2 quad[] = {s00, s10, s11, s01};
            fg->AddConvexPolyFilled(quad, 4, fill_color);
        }

        for (auto& [vx, vy] : m_blocked_verts) {
            glm::vec3 pos{td.vertex_world_x(vx), td.vertex_world_y(vy), td.world_z_at(vx, vy) + z_off};
            ImVec2 sp;
            if (!world_to_screen(pos, sp)) continue;
            if (sp.x < 0 || sp.x > sw || sp.y < 0 || sp.y > sh) continue;
            fg->AddCircleFilled(sp, 4.0f, dot_color);
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

                if (fs::is_directory(m_map.map_root())) {
                    // Source-folder mode: write terrain.bin directly into the source tree.
                    std::string terrain_path = m_map.map_root() + "/scenes/" +
                        m_current_scene + "/terrain.bin";
                    map::save_terrain(m_map.terrain(), terrain_path);
                    log::info(TAG, "Saved terrain to {}", terrain_path);
                } else {
                    // Normal mode (packed .uldmap): unpack → overwrite terrain.bin → repack → reload.
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
                        if (!map::save_terrain(m_map.terrain(), terrain_path)) {
                            log::error(TAG, "Save: failed to write terrain to '{}'", terrain_path);
                        } else {
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
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Show Pathing", nullptr, &m_show_pathing);
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

    // Tool panel
    ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(220, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("Tools");

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
    ImGui::RadioButton("Path Block",  &tool, static_cast<int>(Tool::PathingBlock));
    ImGui::RadioButton("Path Allow",  &tool, static_cast<int>(Tool::PathingAllow));
    m_tool = static_cast<Tool>(tool);

    ImGui::Separator();
    ImGui::SliderInt("Size", &m_brush_size, 1, 11);

    bool is_one_shot = m_tool == Tool::CliffRaise || m_tool == Tool::CliffLower ||
                       m_tool == Tool::RampSet || m_tool == Tool::RampClear ||
                       m_tool == Tool::PathingBlock || m_tool == Tool::PathingAllow ||
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
        ImGui::Text("Walk:%s Fly:%s Ramp:%s",
                     (flags & map::PATHING_WALKABLE) ? "Y" : "N",
                     (flags & map::PATHING_FLYABLE) ? "Y" : "N",
                     (flags & map::PATHING_RAMP) ? "Y" : "N");

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
}

// ── Cleanup ──────────────────────────────────────────────────────────────

void Editor::shutdown_imgui() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (m_imgui_pool) {
        vkDestroyDescriptorPool(m_rhi.device(), m_imgui_pool, nullptr);
        m_imgui_pool = VK_NULL_HANDLE;
    }
}

void Editor::shutdown() {
    log::info(TAG, "=== Shutting down Editor ===");

    shutdown_imgui();
    m_map.shutdown();
    m_simulation.shutdown();
    m_renderer.shutdown();
    m_asset.shutdown();
    m_rhi.shutdown();
    m_platform->shutdown();

    log::info(TAG, "=== Editor shut down ===");
}

} // namespace uldum::editor
