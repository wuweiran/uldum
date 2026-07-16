#pragma once

#include "platform/platform.h"
#include "rhi/rhi.h"
#include "asset/asset.h"
#include "audio/audio.h"
#include "render/renderer.h"
#include "editor/editor_overlays.h"
#include "editor/file_explorer.h"
#include "simulation/simulation.h"
#include "map/map.h"
#include "core/types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <vulkan/vulkan.h>
#include <imgui.h>
#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace uldum::editor {

// Top-level edit mode. The right panel + click handlers + selection
// semantics all change per mode. Hotkeys come from the menu.
enum class EditMode : u8 {
    Terrain,   // existing brushes — heightmap / cliffs / ramps / paint
    Object,    // unit / item / destructable placement — WC3's "Object" vocab
    Region,    // named scene zones — drag rect / circle, edit id, etc.
    // Camera, Trigger come later.
};

// Sub-tool within Region mode.
enum class RegionTool : u8 {
    Select,      // click a region to focus it; drag shapes to move; delete to remove
    AddRect,     // click-drag in world → append a rect to the focused region
    AddCircle,   // click center, drag radius → append a circle to the focused region
};

// Tools within Terrain mode.
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
    Count
};

// Categories within Object mode. A dropdown in the right panel
// switches between them; each pulls its type list from the active
// map's type registries. WC3 splits Unit (with Item filter) and
// Doodad (=Destructable) into separate palettes; we collapse all
// three into one mode for a smaller editor.
enum class ObjectCategory : u8 {
    Unit,
    Item,
    Destructable,
    Doodad,
};

// Object-mode sub-tool.
enum class ObjectTool : u8 {
    Place,   // left-click on ground → spawn the selected type at cursor
    Select,  // left-click on entity → select; Delete key removes
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
    void imgui_render(rhi::CommandList& cmd);
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
    void cleanup_ramp_flags();  // remove RAMP from vertices with no adjacent cliff difference

    // Modules
    std::unique_ptr<platform::Platform> m_platform;
    rhi::Rhi          m_rhi;
    asset::AssetManager     m_asset;
    audio::AudioEngine      m_audio;
    render::Renderer        m_renderer;
    EditorOverlays          m_overlays;
    simulation::Simulation  m_simulation;
    map::MapManager         m_map;

    // Map Explorer window (browse / preview / inspect the map's files).
    FileExplorer        m_files;
    FileExplorerContext file_ctx();

    // View-menu window visibility. World-editing panels default on; the
    // Map Explorer is available but hidden until the maker opens it.
    bool m_show_scene  = true;
    bool m_show_tools  = true;
    bool m_show_info   = true;
    bool m_show_assets = false;

    // ImGui
    VkDescriptorPool m_imgui_pool = VK_NULL_HANDLE;

    // Editor state
    std::string m_map_path = "maps/test_map.uldmap";
    bool m_map_loaded = false;
    std::vector<std::string> m_scenes;
    std::string m_current_scene;

    void reset_editing_state();
    void switch_scene(const std::string& scene_name);
    void open_map(const std::string& path);

    // Lua validation via uldum_scriptcheck (dev/editor only), surfaced in the
    // Map Explorer's inspector. Syntax is per-file; full validation (syntax +
    // undefined engine calls) is per-scene, over the script set that shares the
    // scene's runtime. Both return display lines.
    std::vector<std::string> check_script_syntax(const std::string& rel);
    std::vector<std::string> validate_scene_scripts(const std::string& scene);

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

    // Right-click drag pan: anchor point on ground plane
    bool      m_drag_active = false;
    glm::vec3 m_drag_anchor{0.0f};  // world-space ground point pinned under cursor

    // ── Mode + Place state ────────────────────────────────────────────────
    EditMode      m_mode             = EditMode::Terrain;
    ObjectCategory m_object_category   = ObjectCategory::Unit;
    ObjectTool     m_object_tool       = ObjectTool::Place;

    // Selected type per category (sticky across category switches so
    // toggling Unit → Item → Unit remembers what you had picked).
    std::string m_place_unit_type;
    std::string m_place_item_type;
    std::string m_place_destructable_type;
    std::string m_place_doodad_type;

    u32  m_place_unit_owner       = 0;
    f32  m_place_unit_facing_deg  = 0.0f;

    // Variation control for destructable placement. Random rolls a
    // fresh variation per click; Fixed uses m_place_destructable_fixed_var.
    // `m_place_destructable_var` is the EFFECTIVE value the preview +
    // next placement use — kept in sync by toggle / type changes.
    bool m_place_destructable_random    = true;
    u8   m_place_destructable_fixed_var = 0;
    u8   m_place_destructable_var       = 0;

    // Same Random / Fixed variation scheme for doodads.
    bool m_place_doodad_random    = true;
    u8   m_place_doodad_fixed_var = 0;
    u8   m_place_doodad_var       = 0;

    // Continuous brush: hold left mouse to place repeatedly. Deduplicated
    // so the user can drag without spamming entities on the same spot.
    // Footprint objects dedup via "footprint already blocked" check;
    // non-footprint objects dedup by distance from the last placement.
    bool      m_place_continuous   = false;
    bool      m_has_last_placement = false;
    glm::vec2 m_last_placement_pos{0.0f};

    // Selected entity (Select tool). 0 = none.
    simulation::Unit         m_selected_unit{};
    simulation::Item         m_selected_item{};
    simulation::Destructable m_selected_destructable{};
    simulation::Doodad       m_selected_doodad{};

    // Place-mode handlers
    void place_mode_on_left_click(f32 screen_x, f32 screen_y);
    void place_mode_on_delete();
    void place_mode_draw_panel();
    void place_unit_at(f32 wx, f32 wy);
    void place_item_at(f32 wx, f32 wy);
    void place_destructable_at(f32 wx, f32 wy);
    void place_doodad_at(f32 wx, f32 wy);
    void clear_selection();

    // Continuous-mode tick: while left mouse is held, attempts to place
    // at the cursor per frame, gated by dedup (footprint clear for sized
    // objects, distance threshold for free-form ones).
    void place_mode_continuous_tick(f32 screen_x, f32 screen_y);

    // Unified validity check used by both the visual indicator and the
    // actual placement path. Returns true if (cat, type) can be placed
    // at world position (wx, wy) without overlap:
    //   • objects with a pathing footprint  → all covered tiles clear.
    //   • objects with a collision radius   → no overlap with another
    //                                          entity's circle, and the
    //                                          center tile isn't blocked.
    //   • items                              → always allowed.
    // The preview entity is excluded from the check.
    bool can_place_at(ObjectCategory cat, std::string_view type,
                      f32 wx, f32 wy) const;

    // Footprint overlap check. fw/fh are in TILES when in_cells=false
    // (buildings) or CELLS when in_cells=true (destructables). Returns
    // true iff every covered cell is occupiable by move_type and not
    // currently blocked.
    bool footprint_clear_at(f32 wx, f32 wy, u32 fw, u32 fh, bool in_cells,
                            simulation::MoveType move_type) const;

    // Ghost-entity preview rendered at the cursor while in Object/Place.
    // A real entity in the sim (the editor never ticks, so gameplay
    // components are inert) with its transform overwritten each frame
    // to track the snapped cursor position. Used for both Unit and
    // Destructable categories — Unit/Destructable handles share layout,
    // and simulation::destroy() dispatches the same component cleanup.
    // Destroyed before save_objects so it doesn't leak into objects.json.
    simulation::Unit m_preview_handle{};
    ObjectCategory   m_preview_category = ObjectCategory::Unit;
    std::string      m_preview_type_id;
    // The variation the preview entity was built with — used to detect
    // staleness when the user changes Random / Fixed. Shared by
    // Destructable and Doodad previews (active one is selected by
    // m_preview_category).
    u8               m_preview_variation = 0;
    void update_object_preview();
    void destroy_preview();
    void roll_destructable_variation();
    void roll_doodad_variation();

    // ── Region state ─────────────────────────────────────────────────────
    RegionTool m_region_tool = RegionTool::Select;
    // Index into m_map.mutable_regions(); -1 = no region focused. New
    // shapes append to the focused region; deletion / id-edit also act
    // on it. -1 disables the Add* tools.
    i32        m_region_focus = -1;

    // ImGui InputText buffer for the focused region's id. Synced from
    // the region whenever focus changes; written back on Enter / blur
    // (with dup-id rejection).
    std::array<char, 64> m_region_id_buf{};

    // Drag state for AddRect / AddCircle. World coords; only one shape
    // at a time.
    bool       m_region_drag_active = false;
    glm::vec2  m_region_drag_start{0.0f};
    glm::vec2  m_region_drag_current{0.0f};

    void region_mode_panel();
    void region_mode_on_left_press();
    void region_mode_on_left_release();
    void region_mode_on_delete();
    void region_set_focus(i32 index);
    bool region_id_in_use(std::string_view id, i32 ignore_index) const;

    // ── Asset import dialog ──────────────────────────────────────────────
    // PNG → KTX2 conversion via basisu. Modal opened from the File menu;
    // Convert runs the tool synchronously and surfaces a Success / Fail
    // result modal so the author doesn't have to read raw tool output.
    enum class ImportResult : u8 { None, Success, Failure };

    bool m_import_open        = false;
    bool m_import_linear      = false;  // -linear flag (normal maps / data textures)
    bool m_import_show_result = false;  // raise the result modal next frame
    bool m_import_resize      = false;  // pass -resample W H to basisu
    std::array<char, 512> m_import_src{};
    std::array<char, 256> m_import_path{};
    std::array<char, 256> m_import_filename{};
    // Detected source dimensions (0 = not detected yet). Set when the
    // user picks a file via Browse; used to default the resize fields.
    i32                   m_import_src_w = 0;
    i32                   m_import_src_h = 0;
    // Target dimensions when m_import_resize is true.
    i32                   m_import_w     = 256;
    i32                   m_import_h     = 256;
    ImportResult          m_import_result = ImportResult::None;
    std::string           m_import_result_msg;

    void draw_import_dialog();
    void run_png_import();
};

} // namespace uldum::editor
