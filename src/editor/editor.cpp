#include "editor/editor.h"
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

    if (m_map.load_map(m_map_path, m_asset, m_simulation)) {
        m_map_loaded = true;
        m_scenes = m_map.list_scenes();
        m_current_scene = m_map.manifest().start_scene;
        m_renderer.set_map_root(m_map.map_root());
        if (m_map.terrain().is_valid()) {
            m_renderer.set_terrain(m_map.terrain());
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
    init_info.MSAASamples    = VK_SAMPLE_COUNT_1_BIT;
    init_info.UseDynamicRendering = true;

    VkFormat color_format = m_rhi.swapchain_format();
    VkFormat depth_format = m_rhi.depth_format();
    init_info.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    init_info.PipelineRenderingCreateInfo.depthAttachmentFormat = depth_format;

    ImGui_ImplVulkan_Init(&init_info);
    ImGui_ImplVulkan_CreateFontsTexture();

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
            m_cursor_vx = static_cast<i32>(std::round(m_cursor_pos.x / td.tile_size));
            m_cursor_vy = static_cast<i32>(std::round(m_cursor_pos.y / td.tile_size));
            m_cursor_vx = std::clamp(m_cursor_vx, 0, static_cast<i32>(td.tiles_x));
            m_cursor_vy = std::clamp(m_cursor_vy, 0, static_cast<i32>(td.tiles_y));
        }

        // Right-click drag: pan camera
        bool over_ui = ImGui::GetIO().WantCaptureMouse;
        if (input.mouse_right && !over_ui) {
            m_renderer.camera().pan(input.mouse_dx, input.mouse_dy);
        }

        // Apply brush when left-clicking on terrain (and not over ImGui)
        if (input.mouse_left && m_cursor_valid && !over_ui) {
            if (m_continuous) {
                apply_brush(frame_dt);
            } else if (!m_brush_applied) {
                apply_brush(1.0f);
                m_brush_applied = true;
            }
        }
        if (!input.mouse_left) {
            m_brush_applied = false;
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

        // Render
        VkCommandBuffer cmd = m_rhi.begin_frame();
        if (cmd && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
            m_renderer.draw_shadows(cmd, m_simulation.world());
            m_rhi.begin_rendering();
            m_renderer.draw(cmd, m_rhi.extent(), m_simulation.world());
            imgui_render(cmd);
            m_rhi.end_frame();
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
        if (p.x < 0 || p.y < 0 || p.x > td.world_width() || p.y > td.world_height())
            continue;

        // Sample terrain height at this XY
        f32 fx = p.x / td.tile_size;
        f32 fy = p.y / td.tile_size;
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

void Editor::brush_paint(f32 strength, f32 dt) {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);

    f32 rate = std::min(1.0f, strength * dt * 0.5f);
    i32 layer = std::clamp(m_paint_layer, 0, 3);

    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            f32 w = tile_falloff(static_cast<i32>(ix) - m_cursor_vx,
                                 static_cast<i32>(iy) - m_cursor_vy, m_brush_size);
            if (w <= 0.0f) continue;

            u32 idx = iy * td.verts_x() + ix;
            f32 add = 255.0f * w * rate;

            f32 cur = static_cast<f32>(td.splatmap[layer][idx]);
            f32 new_val = std::min(255.0f, cur + add);
            f32 delta = new_val - cur;
            td.splatmap[layer][idx] = static_cast<u8>(new_val);

            // Reduce other layers proportionally
            f32 others_total = 0.0f;
            for (i32 l = 0; l < 4; ++l)
                if (l != layer) others_total += static_cast<f32>(td.splatmap[l][idx]);
            if (others_total > 0.0f) {
                for (i32 l = 0; l < 4; ++l) {
                    if (l != layer) {
                        f32 v = static_cast<f32>(td.splatmap[l][idx]);
                        v -= delta * (v / others_total);
                        td.splatmap[l][idx] = static_cast<u8>(std::max(0.0f, v));
                    }
                }
            }
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
    auto& td = m_map.terrain();
    if (!td.is_valid()) return;

    for (u32 vy = 0; vy <= td.tiles_y; ++vy) {
        for (u32 vx = 0; vx <= td.tiles_x; ++vx) {
            if (!(td.pathing_at(vx, vy) & map::PATHING_RAMP)) continue;

            // Check all tiles touching this vertex for a cliff difference of 1
            bool has_cliff = false;
            for (i32 dy = -1; dy <= 0 && !has_cliff; ++dy) {
                for (i32 dx = -1; dx <= 0 && !has_cliff; ++dx) {
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
                    if (cmax - cmin == 1) has_cliff = true;
                }
            }

            if (!has_cliff) {
                td.pathing_at(vx, vy) &= ~map::PATHING_RAMP;
                m_terrain_dirty = true;
            }
        }
    }
}

void Editor::brush_ramp_set() {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);
    i32 r = m_brush_size - 1;
    i32 r2 = r * r;

    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            i32 dx = static_cast<i32>(ix) - m_cursor_vx;
            i32 dy = static_cast<i32>(iy) - m_cursor_vy;
            if (dx * dx + dy * dy > r2) continue;

            // Only allow ramp where an adjacent tile has cliff difference of 1
            bool has_cliff = false;
            for (i32 oy = -1; oy <= 0 && !has_cliff; ++oy) {
                for (i32 ox = -1; ox <= 0 && !has_cliff; ++ox) {
                    i32 tx = static_cast<i32>(ix) + ox;
                    i32 ty = static_cast<i32>(iy) + oy;
                    if (tx < 0 || ty < 0 || tx >= static_cast<i32>(td.tiles_x) || ty >= static_cast<i32>(td.tiles_y))
                        continue;
                    u8 c[4] = {
                        td.cliff_at(tx, ty), td.cliff_at(tx+1, ty),
                        td.cliff_at(tx, ty+1), td.cliff_at(tx+1, ty+1)
                    };
                    u8 cmin = std::min({c[0], c[1], c[2], c[3]});
                    u8 cmax = std::max({c[0], c[1], c[2], c[3]});
                    if (cmax - cmin == 1) has_cliff = true;
                }
            }
            if (!has_cliff) continue;
            td.pathing_at(ix, iy) |= map::PATHING_RAMP;
            m_terrain_dirty = true;
        }
    }
}

void Editor::brush_ramp_clear() {
    auto& td = m_map.terrain();
    auto br = compute_brush_range(td, m_cursor_vx, m_cursor_vy, m_brush_size);
    i32 r = m_brush_size - 1;
    i32 r2 = r * r;

    for (u32 iy = br.min_iy; iy < br.max_iy; ++iy) {
        for (u32 ix = br.min_ix; ix < br.max_ix; ++ix) {
            i32 dx = static_cast<i32>(ix) - m_cursor_vx;
            i32 dy = static_cast<i32>(iy) - m_cursor_vy;
            if (dx * dx + dy * dy > r2) continue;

            td.pathing_at(ix, iy) &= ~map::PATHING_RAMP;
            m_terrain_dirty = true;
        }
    }
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
        }
    }
}

// ── Terrain mesh rebuild ─────────────────────────────────────────────────

void Editor::rebuild_terrain_mesh() {
    if (!m_map_loaded) return;
    vkDeviceWaitIdle(m_rhi.device());  // wait for GPU to finish using old buffers
    m_renderer.set_terrain(m_map.terrain());
}

void Editor::switch_scene(const std::string& scene_name) {
    if (!m_map_loaded || scene_name == m_current_scene) return;

    vkDeviceWaitIdle(m_rhi.device());

    if (m_map.switch_scene(scene_name, m_asset, m_simulation)) {
        m_current_scene = scene_name;
        if (m_map.terrain().is_valid()) {
            m_renderer.set_terrain(m_map.terrain());
        }
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
    if (m_map.load_map(m_map_path, m_asset, m_simulation)) {
        m_map_loaded = true;
        m_scenes = m_map.list_scenes();
        m_current_scene = m_map.manifest().start_scene;
        m_renderer.set_map_root(m_map.map_root());
        if (m_map.terrain().is_valid()) {
            m_renderer.set_terrain(m_map.terrain());
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
    if (ImGui::GetIO().WantCaptureMouse) return;

    auto* draw_list = ImGui::GetForegroundDrawList();
    auto& td = m_map.terrain();

    // Surface height at a vertex as rendered: max cliff level of touching tiles + heightmap.
    // This matches the per-tile surface mesh which uses max(4 corners).
    if (m_cursor_valid) {
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
                glm::vec3 a{static_cast<f32>(ix) * td.tile_size,
                            static_cast<f32>(iy) * td.tile_size,
                            td.world_z_at(ix, iy) + 1.0f};
                glm::vec3 b{static_cast<f32>(ix + 1) * td.tile_size,
                            static_cast<f32>(iy) * td.tile_size,
                            td.world_z_at(ix + 1, iy) + 1.0f};
                ImVec2 sa, sb;
                if (world_to_screen(a, sa) && world_to_screen(b, sb))
                    draw_list->AddLine(sa, sb, grid_color);
            }
        }
        // Vertical lines
        for (i32 ix = grid_min_x; ix <= grid_max_x; ++ix) {
            for (i32 iy = grid_min_y; iy < grid_max_y; ++iy) {
                glm::vec3 a{static_cast<f32>(ix) * td.tile_size,
                            static_cast<f32>(iy) * td.tile_size,
                            td.world_z_at(ix, iy) + 1.0f};
                glm::vec3 b{static_cast<f32>(ix) * td.tile_size,
                            static_cast<f32>(iy + 1) * td.tile_size,
                            td.world_z_at(ix, iy + 1) + 1.0f};
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

                glm::vec3 vpos{static_cast<f32>(ix) * td.tile_size,
                               static_cast<f32>(iy) * td.tile_size,
                               td.world_z_at(ix, iy) + 2.0f};
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
            glm::vec3 cpos{static_cast<f32>(m_cursor_vx) * td.tile_size,
                           static_cast<f32>(m_cursor_vy) * td.tile_size,
                           td.world_z_at(m_cursor_vx, m_cursor_vy) + 3.0f};
            ImVec2 sp;
            if (world_to_screen(cpos, sp)) {
                draw_list->AddCircle(sp, 8.0f, IM_COL32(255, 255, 0, 255), 0, 2.0f);
            }
        }
    }
}

// ── UI ───────────────────────────────────────────────────────────────────

void Editor::draw_ui() {
    // Main menu bar
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Map...")) {
                std::string path = pick_folder(static_cast<HWND>(m_platform->native_window_handle()));
                if (!path.empty()) {
                    open_map(path);
                }
            }
            if (ImGui::MenuItem("Save Map")) {
                if (m_map_loaded) {
                    std::string terrain_path = m_map.map_root() + "/scenes/" +
                        m_current_scene + "/terrain.bin";
                    map::save_terrain(m_map.terrain(), terrain_path);
                    log::info(TAG, "Saved terrain to {}", terrain_path);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }
        if (m_map_loaded && !m_scenes.empty() && ImGui::BeginMenu("Scene")) {
            for (auto& scene : m_scenes) {
                bool selected = (scene == m_current_scene);
                if (ImGui::MenuItem(scene.c_str(), nullptr, selected)) {
                    if (!selected) switch_scene(scene);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
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
                       m_tool == Tool::PathingBlock || m_tool == Tool::PathingAllow;
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
        const char* layers[] = {"Layer 0 (Grass)", "Layer 1 (Dirt)", "Layer 2 (Stone)", "Layer 3 (Water)"};
        ImGui::Combo("Layer", &m_paint_layer, layers, 4);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Right-click drag to pan camera");

    ImGui::End();

    // Info panel
    ImGui::SetNextWindowPos(ImVec2(10, 440), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(220, 120), ImGuiCond_FirstUseEver);
    ImGui::Begin("Info");
    if (m_map_loaded) {
        auto& td = m_map.terrain();
        ImGui::Text("Map: %s", m_map.manifest().name.c_str());
        ImGui::Text("Scene: %s", m_current_scene.c_str());
        ImGui::Text("Terrain: %ux%u tiles", td.tiles_x, td.tiles_y);
    }
    if (m_cursor_valid && m_map_loaded) {
        auto& td = m_map.terrain();
        u8 flags = td.pathing_at(m_cursor_vx, m_cursor_vy);
        ImGui::Text("Cursor: %d, %d", m_cursor_vx, m_cursor_vy);
        ImGui::Text("Height: %.1f  Cliff: %d",
                     td.height_at(m_cursor_vx, m_cursor_vy),
                     td.cliff_at(m_cursor_vx, m_cursor_vy));
        ImGui::Text("Walk:%s Fly:%s Ramp:%s",
                     (flags & map::PATHING_WALKABLE) ? "Y" : "N",
                     (flags & map::PATHING_FLYABLE) ? "Y" : "N",
                     (flags & map::PATHING_RAMP) ? "Y" : "N");
        ImGui::Text("Water: %s", td.is_water(m_cursor_vx, m_cursor_vy) ? "yes" : "no");
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
