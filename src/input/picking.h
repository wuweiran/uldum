#pragma once

#include "simulation/handle_types.h"
#include "simulation/world.h"
#include "render/camera.h"
#include "map/terrain_data.h"
#include "core/types.h"

#include <glm/vec3.hpp>

#include <vector>

namespace uldum::simulation { class Vision; }

namespace uldum::input {

// Picking helpers: screen-to-world conversion and unit selection queries.
// Depends on camera (for unprojection), terrain (for ground intersection),
// and world (for entity positions and selectable components).
class Picker {
public:
    void init(const render::Camera* camera, const map::TerrainData* terrain,
              const simulation::World* world, u32 screen_w, u32 screen_h);

    void set_screen_size(u32 w, u32 h) { m_screen_w = w; m_screen_h = h; }

    // Hook up the local player's vision subsystem so pick_widget /
    // pick_target / pick_item / pick_units_in_box skip entities the
    // player cannot see (fogged tiles + invisible-without-detector).
    // Set once at session start; pickers without a vision reference
    // (editor, server-side queries) skip the filter and act as if
    // everything is visible.
    void set_vision(const simulation::Vision* vision, simulation::Player local) {
        m_vision = vision; m_local_player = local;
    }

    // Convert screen position to world position on terrain. Returns false if off-terrain.
    bool screen_to_world(f32 screen_x, f32 screen_y, glm::vec3& world_pos) const;

    // Pick the best widget under screen coordinates (closest to cursor, highest priority).
    // A "widget" is the gameplay-selectable/targetable set: Unit or Destructable
    // (matches widget_kind / GetTriggerWidget). Doodads are editor-only and never
    // picked here. Only picks widgets owned by the given player, or any if invalid.
    // selectable_only: skip destructables flagged non-selectable (trees) — used by
    // click-selection so a left-click can't select a tree, while attack/cast
    // targeting (pick_target) still hits it.
    simulation::Unit pick_widget(f32 screen_x, f32 screen_y,
                                 simulation::Player player = {},
                                 bool selectable_only = false) const;

    // Pick the best targetable unit under screen coordinates (any ownership — for attack/cast targeting).
    simulation::Unit pick_target(f32 screen_x, f32 screen_y) const;

    // Pick the closest ground item under screen coordinates. Used for
    // smart-order routing: right-click on an item issues PickupItem.
    // Returns invalid Item if no item is hit.
    simulation::Item pick_item(f32 screen_x, f32 screen_y) const;

    // Collect all own units within a screen-space rectangle.
    std::vector<simulation::Unit> pick_units_in_box(f32 x0, f32 y0, f32 x1, f32 y1,
                                                     simulation::Player player) const;

private:
    glm::vec3 screen_to_ray(f32 sx, f32 sy) const;
    glm::vec3 ray_origin(f32 sx, f32 sy) const;

    const render::Camera*         m_camera  = nullptr;
    const map::TerrainData*       m_terrain = nullptr;
    const simulation::World*      m_world   = nullptr;
    const simulation::Vision*     m_vision  = nullptr;
    simulation::Player            m_local_player{};
    u32 m_screen_w = 1;
    u32 m_screen_h = 1;
};

} // namespace uldum::input
