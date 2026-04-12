#pragma once

#include "platform/platform.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "render/renderer.h"
#include "simulation/simulation.h"
#include "map/map.h"
#include "core/types.h"

#include <glm/vec3.hpp>
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <memory>
#include <string>

namespace uldum::editor {

enum class Tool : u8 {
    Raise = 0,
    Lower,
    Smooth,
    Flatten,
    Paint,
    CliffRaise,
    CliffLower,
    RampSet,
    RampClear,
    PathingBlock,
    PathingAllow,
    Count
};

class Editor {
public:
    bool init(const std::string& map_path = {});
    void run();
    void shutdown();

private:
    bool init_imgui();
    void shutdown_imgui();
    void imgui_new_frame();
    void imgui_render(VkCommandBuffer cmd);
    void draw_ui();
    void draw_overlays();  // grid lines, brush circle (via ImGui draw list)

    // World-to-screen projection helper
    bool world_to_screen(glm::vec3 world_pos, ImVec2& screen_pos) const;

    // Terrain editing
    bool raycast_terrain(f32 screen_x, f32 screen_y, glm::vec3& hit) const;
    void apply_brush(f32 dt);
    void rebuild_terrain_mesh();

    // Brush operations
    void brush_raise(f32 strength, f32 dt);
    void brush_lower(f32 strength, f32 dt);
    void brush_smooth(f32 strength, f32 dt);
    void brush_flatten(f32 strength, f32 dt);
    void brush_paint(f32 strength, f32 dt);
    void brush_cliff_raise();
    void brush_cliff_lower();
    void brush_ramp_set();
    void brush_ramp_clear();
    void brush_pathing_block();
    void brush_pathing_allow();
    void cleanup_ramp_flags();  // remove RAMP from vertices with no adjacent cliff difference

    // Modules
    std::unique_ptr<platform::Platform> m_platform;
    rhi::VulkanRhi          m_rhi;
    asset::AssetManager     m_asset;
    render::Renderer        m_renderer;
    simulation::Simulation  m_simulation;
    map::MapManager         m_map;

    // ImGui
    VkDescriptorPool m_imgui_pool = VK_NULL_HANDLE;

    // Editor state
    std::string m_map_path = "maps/test_map.uldmap";
    bool m_map_loaded = false;
    std::vector<std::string> m_scenes;
    std::string m_current_scene;

    void switch_scene(const std::string& scene_name);
    void open_map(const std::string& path);

    // Tool state
    Tool  m_tool           = Tool::Raise;
    i32   m_brush_size     = 2;     // WC3 convention: 1 = single vertex, 2 = 1-radius, etc.
    f32   m_brush_amount   = 10.0f; // fixed amount per click (game units)
    f32   m_brush_speed    = 30.0f; // continuous mode: game units per second
    bool  m_continuous     = false; // continuous mode (hold to keep applying)
    f32   m_flatten_height = 0.0f;
    i32   m_paint_layer    = 0;
    bool  m_terrain_dirty  = false;
    bool  m_brush_applied  = false; // one-shot: already applied this click
    i32   m_last_brush_vx = -1;   // last vertex where brush was applied (for drag)
    i32   m_last_brush_vy = -1;

    // Snapped cursor (center vertex of brush)
    i32   m_cursor_vx = 0;
    i32   m_cursor_vy = 0;

    // Brush cursor
    glm::vec3 m_cursor_pos{0.0f};
    bool      m_cursor_valid = false;

    // Undo/redo system
    struct TerrainEdit {
        u32 min_vx, min_vy, max_vx, max_vy;  // affected vertex region (inclusive)
        std::vector<f32> old_heightmap, new_heightmap;
        std::vector<u8>  old_cliff,     new_cliff;
        std::vector<u8>  old_tile_layer,new_tile_layer;
        std::vector<u8>  old_pathing,   new_pathing;
    };
    std::vector<TerrainEdit> m_undo_stack;
    std::vector<TerrainEdit> m_redo_stack;
    bool m_stroke_active = false;
    u32  m_stroke_min_vx = 0, m_stroke_min_vy = 0;
    u32  m_stroke_max_vx = 0, m_stroke_max_vy = 0;
    static constexpr u32 MAX_UNDO = 16;

    void begin_stroke();   // snapshot before editing
    void end_stroke();     // capture new state, push to undo stack
    void undo();
    void redo();
    void apply_edit(const TerrainEdit& edit, bool use_new);
    bool m_prev_undo_key = false;
    bool m_prev_redo_key = false;

    // View options
    bool m_show_pathing = true;

    // Cached blocked tile/vertex lists — rebuilt only when pathing changes
    struct BlockedTile { u32 tx, ty; };
    struct BlockedVertex { u32 vx, vy; };
    std::vector<BlockedTile>   m_blocked_tiles;
    std::vector<BlockedVertex> m_blocked_verts;
    bool m_pathing_cache_dirty = true;
    void rebuild_pathing_cache();

    // Right-click drag pan: anchor point on ground plane
    bool      m_drag_active = false;
    glm::vec3 m_drag_anchor{0.0f};  // world-space ground point pinned under cursor
};

} // namespace uldum::editor
