#pragma once

#include "simulation/entity_types.h"
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

    // Minimap proxy. When the pointer is over the minimap panel, a click is
    // a *world ground point* (via the minimap transform), never a unit — so
    // screen_to_world routes through the minimap and pick_widget/target/item
    // return invalid. This makes ground orders and ground-target abilities
    // work off the minimap through the exact same handle_orders path a world
    // click uses (WC3 behavior). Rect is in PHYSICAL pixels — the same space
    // as the pointer coords handed to the pick functions. A zero-width rect
    // (default) disables the proxy: nothing is ever "over the minimap".
    void set_minimap_rect(f32 x, f32 y, f32 w, f32 h) {
        m_minimap_x = x; m_minimap_y = y; m_minimap_w = w; m_minimap_h = h;
    }
    bool over_minimap(f32 screen_x, f32 screen_y) const {
        return m_minimap_w > 0.0f && m_minimap_h > 0.0f &&
               screen_x >= m_minimap_x && screen_x <= m_minimap_x + m_minimap_w &&
               screen_y >= m_minimap_y && screen_y <= m_minimap_y + m_minimap_h;
    }

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

    // Collect all selectable units within a screen-space rectangle,
    // priority-sorted, regardless of ownership. Caller applies the
    // own-vs-foreign policy.
    std::vector<simulation::Unit> pick_units_in_box(f32 x0, f32 y0, f32 x1, f32 y1) const;

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
    // Minimap panel in physical pixels; zero-size = no minimap (proxy off).
    f32 m_minimap_x = 0.0f, m_minimap_y = 0.0f, m_minimap_w = 0.0f, m_minimap_h = 0.0f;
};

} // namespace uldum::input
