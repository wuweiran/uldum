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
    Cliff,
    Ramp,
    Pathing,
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
    void brush_pathing_toggle();
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

    // Snapped cursor (center vertex of brush)
    i32   m_cursor_vx = 0;
    i32   m_cursor_vy = 0;

    // Brush cursor
    glm::vec3 m_cursor_pos{0.0f};
    bool      m_cursor_valid = false;
};

} // namespace uldum::editor
