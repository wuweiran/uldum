#include "input/picking.h"

#include "map/terrain_data.h"
#include "simulation/vision.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace uldum::input {

// World-position → fog visibility lookup for the local player. Entities
// whose tile is hidden are excluded from picking so the player can't
// click on or smart-target things they aren't currently scouting. With
// no fog reference (editor, server-side internal queries) everything
// reads as visible.
static bool fog_visible(const simulation::Vision* vision,
                        simulation::Player local,
                        const map::TerrainData* terrain,
                        const glm::vec3& world_pos) {
    if (!vision || !terrain || !terrain->is_valid()) return true;
    f32 ts = terrain->tile_size;
    if (ts <= 0.0f) return true;
    i32 tx = static_cast<i32>((world_pos.x - terrain->origin_x()) / ts);
    i32 ty = static_cast<i32>((world_pos.y - terrain->origin_y()) / ts);
    if (tx < 0 || ty < 0 ||
        static_cast<u32>(tx) >= terrain->tiles_x ||
        static_cast<u32>(ty) >= terrain->tiles_y) return true;
    return vision->is_visible(local, static_cast<u32>(tx), static_cast<u32>(ty));
}

void Picker::init(const render::Camera* camera, const map::TerrainData* terrain,
                  const simulation::World* world, u32 screen_w, u32 screen_h) {
    m_camera   = camera;
    m_terrain  = terrain;
    m_world    = world;
    m_screen_w = screen_w;
    m_screen_h = screen_h;
}

glm::vec3 Picker::ray_origin(f32 sx, f32 sy) const {
    f32 w = static_cast<f32>(m_screen_w);
    f32 h = static_cast<f32>(m_screen_h);
    f32 ndc_x = (2.0f * sx / w) - 1.0f;
    // Vulkan: view_projection bakes a Y-flip, so screen top (sy=0) maps to
    // NDC y=-1 directly. GLES: no flip, so we invert here to keep the
    // screen-top-origin convention.
#if defined(ULDUM_BACKEND_GLES)
    f32 ndc_y = 1.0f - (2.0f * sy / h);
#else
    f32 ndc_y = (2.0f * sy / h) - 1.0f;
#endif

    glm::mat4 inv_vp = glm::inverse(m_camera->view_projection());
    // Vulkan NDC z: [0,1] (0=near). GLES NDC z: [-1,+1] (-1=near).
#if defined(ULDUM_BACKEND_GLES)
    glm::vec4 near_clip{ndc_x, ndc_y, -1.0f, 1.0f};
#else
    glm::vec4 near_clip{ndc_x, ndc_y,  0.0f, 1.0f};
#endif
    glm::vec4 near_world = inv_vp * near_clip;
    near_world /= near_world.w;
    return glm::vec3(near_world);
}

glm::vec3 Picker::screen_to_ray(f32 sx, f32 sy) const {
    f32 w = static_cast<f32>(m_screen_w);
    f32 h = static_cast<f32>(m_screen_h);
    f32 ndc_x = (2.0f * sx / w) - 1.0f;
#if defined(ULDUM_BACKEND_GLES)
    f32 ndc_y = 1.0f - (2.0f * sy / h);
#else
    f32 ndc_y = (2.0f * sy / h) - 1.0f;
#endif

    glm::mat4 inv_vp = glm::inverse(m_camera->view_projection());
#if defined(ULDUM_BACKEND_GLES)
    glm::vec4 near_clip{ndc_x, ndc_y, -1.0f, 1.0f};
    glm::vec4 far_clip {ndc_x, ndc_y,  1.0f, 1.0f};
#else
    glm::vec4 near_clip{ndc_x, ndc_y,  0.0f, 1.0f};
    glm::vec4 far_clip {ndc_x, ndc_y,  1.0f, 1.0f};
#endif

    glm::vec4 near_world = inv_vp * near_clip;
    glm::vec4 far_world  = inv_vp * far_clip;
    near_world /= near_world.w;
    far_world  /= far_world.w;

    return glm::normalize(glm::vec3(far_world) - glm::vec3(near_world));
}

bool Picker::screen_to_world(f32 screen_x, f32 screen_y, glm::vec3& world_pos) const {
    if (!m_terrain || !m_terrain->is_valid()) return false;

    // Over the minimap: the click is a world ground point mapped through the
    // minimap transform, not a camera raycast. Mirrors hud/minimap.h's
    // minimap_screen_to_world (aspect-preserving letterbox fit + north-at-top
    // flip); duplicated because input/ can't depend on hud/. A click in the
    // letterbox margin clamps to the nearest map edge. Z is terrain-sampled.
    if (over_minimap(screen_x, screen_y)) {
        const f32 mw = m_terrain->world_width(), mh = m_terrain->world_height();
        // Aspect-fit content sub-rect of the panel (see minimap_content_rect).
        f32 cx = m_minimap_x, cy = m_minimap_y, cw = m_minimap_w, ch = m_minimap_h;
        if (mw > 0.0f && mh > 0.0f) {
            const f32 map_aspect = mw / mh, panel_aspect = m_minimap_w / m_minimap_h;
            if (map_aspect > panel_aspect) {
                ch = m_minimap_w / map_aspect; cy = m_minimap_y + (m_minimap_h - ch) * 0.5f;
            } else {
                cw = m_minimap_h * map_aspect; cx = m_minimap_x + (m_minimap_w - cw) * 0.5f;
            }
        }
        f32 fx = (cw > 0.0f) ? (screen_x - cx) / cw : 0.0f;
        f32 fy = (ch > 0.0f) ? (screen_y - cy) / ch : 0.0f;
        fx = std::clamp(fx, 0.0f, 1.0f);
        fy = std::clamp(fy, 0.0f, 1.0f);
        world_pos.x = m_terrain->origin_x() + fx * mw;
        world_pos.y = m_terrain->origin_y() + (1.0f - fy) * mh;
        world_pos.z = map::sample_height(*m_terrain, world_pos.x, world_pos.y);
        return true;
    }

    if (!m_camera) return false;
    glm::vec3 origin = ray_origin(screen_x, screen_y);
    glm::vec3 dir    = screen_to_ray(screen_x, screen_y);
    // Delegate to the shared DDA + per-tile bilinear raycast. Same
    // accuracy and smoothness as the editor cursor; eliminates the
    // march-and-bisect duplicate that used to live here.
    return map::raycast_terrain(*m_terrain, origin, dir, world_pos);
}

// Ray-cylinder intersection: find closest distance between ray and a vertical
// line segment (the unit's selection axis). Returns distance from ray to axis,
// and sets ray_t to the ray parameter at closest approach.
static f32 ray_cylinder_dist(glm::vec3 ray_origin, glm::vec3 ray_dir,
                              glm::vec3 cyl_base, f32 cyl_height, f32& ray_t) {
    // Project to XY plane (cylinder axis is Z)
    glm::vec2 ro{ray_origin.x - cyl_base.x, ray_origin.y - cyl_base.y};
    glm::vec2 rd{ray_dir.x, ray_dir.y};

    // Closest approach of ray to Z-axis in XY
    f32 a = glm::dot(rd, rd);
    if (a < 1e-8f) {
        // Ray is nearly vertical — distance is just XY offset
        ray_t = 0;
        return glm::length(ro);
    }
    f32 b = glm::dot(ro, rd);
    ray_t = -b / a;
    if (ray_t < 0) ray_t = 0;

    // Check Z bounds: ray must be within cylinder height at closest approach
    f32 z_at_closest = ray_origin.z + ray_dir.z * ray_t;
    if (z_at_closest < cyl_base.z || z_at_closest > cyl_base.z + cyl_height) {
        // Clamp Z and recompute
        f32 target_z = std::clamp(z_at_closest, cyl_base.z, cyl_base.z + cyl_height);
        if (std::abs(ray_dir.z) > 1e-6f) {
            ray_t = (target_z - ray_origin.z) / ray_dir.z;
            if (ray_t < 0) ray_t = 0;
        }
    }

    glm::vec3 closest_on_ray = ray_origin + ray_dir * ray_t;
    f32 dx = closest_on_ray.x - cyl_base.x;
    f32 dy = closest_on_ray.y - cyl_base.y;
    return std::sqrt(dx * dx + dy * dy);
}

simulation::Unit Picker::pick_widget(f32 screen_x, f32 screen_y,
                                     simulation::Player player,
                                     bool selectable_only) const {
    if (!m_camera || !m_world) return {};
    // A minimap click is a ground point, never a unit — let orders fall
    // through to the ground-point branch of handle_orders.
    if (over_minimap(screen_x, screen_y)) return {};

    glm::vec3 origin = ray_origin(screen_x, screen_y);
    glm::vec3 dir = screen_to_ray(screen_x, screen_y);

    simulation::Unit best{};
    f32 best_ray_t = 1e9f;

    auto& transforms = m_world->transforms;
    auto& selectables = m_world->selectables;
    auto& owners = m_world->owners;
    auto& handle_infos = m_world->handle_infos;

    for (u32 i = 0; i < selectables.count(); ++i) {
        u32 id = selectables.ids()[i];
        auto& sel = selectables.data()[i];

        auto* transform = transforms.get(id);
        if (!transform) continue;

        auto* info = handle_infos.get(id);
        if (!info || (info->category != simulation::Category::Unit &&
                      info->category != simulation::Category::Destructable)) continue;
        // Selection excludes non-selectable destructables (trees). Attack/cast
        // targeting passes selectable_only=false so it can still hit them.
        if (selectable_only) {
            if (auto* d = m_world->destructables.get(id); d && !d->selectable) continue;
        }
        if (m_world->dead_states.has(id)) continue;
        if (auto* sf = m_world->status_flags.get(id);
            sf && (sf->flags & simulation::status::Untargetable)) continue;

        if (player.is_valid()) {
            auto* own = owners.get(id);
            if (!own || own->id != player.id) continue;
        }

        // Fog filter — entities under the fog of war can't be clicked
        // on. player's own units pass through (fog hides what the
        // player hasn't scouted, not their own troops).
        if (m_local_player.is_valid()) {
            auto* own = owners.get(id);
            bool is_own = own && own->id == m_local_player.id;
            if (!is_own && !fog_visible(m_vision, m_local_player, m_terrain, transform->position)) continue;
        }

        f32 ray_t;
        // Lift the hit-cylinder to the unit's visual height (fly_height for air,
        // 0 otherwise) so clicking the flying model selects it — the sim
        // position is at ground Z but the mesh + ring are up at hull height.
        glm::vec3 base = transform->position;
        base.z += simulation::unit_fly_height(*m_world, id);
        f32 dist = ray_cylinder_dist(origin, dir, base, sel.selection_height, ray_t);


        // Pick radius covers both the ground circle and the visual model body.
        // transform->scale approximates the model's visual width.
        f32 pick_radius = std::max(sel.selection_radius, transform->scale);
        if (dist > pick_radius) continue;

        // Depth-only: the frontmost unit under the cursor wins (smallest ray_t
        // = nearest the camera). "Click what you see" — a unit behind another
        // can never steal the click, regardless of selection priority. Priority
        // is a GROUP concept (box-select lead ordering), not a click rule.
        if (ray_t < best_ray_t) {
            best.id = id;
            best_ray_t = ray_t;
        }
    }
    return best;
}

simulation::Unit Picker::pick_target(f32 screen_x, f32 screen_y) const {
    return pick_widget(screen_x, screen_y, {}); // no player filter — pick any widget
}

simulation::Item Picker::pick_item(f32 screen_x, f32 screen_y) const {
    if (!m_camera || !m_world) return {};
    if (over_minimap(screen_x, screen_y)) return {};  // minimap click = ground point, no item

    glm::vec3 origin = ray_origin(screen_x, screen_y);
    glm::vec3 dir = screen_to_ray(screen_x, screen_y);

    simulation::Item best{};
    f32 best_ray_t = 1e9f;

    auto& transforms = m_world->transforms;
    auto& selectables = m_world->selectables;
    auto& handle_infos = m_world->handle_infos;
    auto& carriables = m_world->carriables;

    for (u32 i = 0; i < selectables.count(); ++i) {
        u32 id = selectables.ids()[i];
        auto& sel = selectables.data()[i];

        auto* info = handle_infos.get(id);
        if (!info || info->category != simulation::Category::Item) continue;
        if (m_world->dead_states.has(id)) continue;
        // Skip items currently being carried (they're "inside" a unit).
        auto* car = carriables.get(id);
        if (car && simulation::is_non_null_handle(car->carried_by)) continue;

        auto* transform = transforms.get(id);
        if (!transform) continue;

        // Fog filter — items in unscouted tiles aren't clickable.
        if (!fog_visible(m_vision, m_local_player, m_terrain, transform->position)) continue;

        f32 ray_t;
        f32 dist = ray_cylinder_dist(origin, dir, transform->position,
                                     sel.selection_height, ray_t);
        f32 pick_radius = std::max(sel.selection_radius, transform->scale);
        if (dist > pick_radius) continue;

        if (ray_t < best_ray_t) {
            best.id = id;
            best_ray_t = ray_t;
        }
    }
    return best;
}

std::vector<simulation::Unit> Picker::pick_units_in_box(f32 x0, f32 y0, f32 x1, f32 y1,
                                                         simulation::Player player) const {
    std::vector<simulation::Unit> result;
    if (!m_camera || !m_world) return result;

    // Normalize box
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);

    auto& transforms = m_world->transforms;
    auto& selectables = m_world->selectables;
    auto& owners = m_world->owners;
    auto& handle_infos = m_world->handle_infos;

    glm::mat4 vp = m_camera->view_projection();
    f32 hw = static_cast<f32>(m_screen_w) * 0.5f;
    f32 hh = static_cast<f32>(m_screen_h) * 0.5f;

    for (u32 i = 0; i < selectables.count(); ++i) {
        u32 id = selectables.ids()[i];

        auto* info = handle_infos.get(id);
        if (!info || info->category != simulation::Category::Unit) continue;
        if (m_world->dead_states.has(id)) continue;
        if (auto* sf = m_world->status_flags.get(id);
            sf && (sf->flags & simulation::status::Untargetable)) continue;

        auto* own = owners.get(id);
        if (!own || own->id != player.id) continue;

        auto* transform = transforms.get(id);
        if (!transform) continue;

        // Project world position to screen.
        // Lift by fly_height so an air unit projects from its visual hull
        // position (up in the sky) — the marquee must hit where the model
        // is drawn, not its ground-Z sim position. Matches pick_widget.
        // Vulkan's projection has a baked Y-flip, so clip.y matches the
        // screen's top-left origin directly. GLES doesn't flip in the
        // projection, so we invert here to land in screen-space.
        glm::vec3 proj_pos = transform->position;
        proj_pos.z += simulation::unit_fly_height(*m_world, id);
        glm::vec4 clip = vp * glm::vec4(proj_pos, 1.0f);
        if (clip.w <= 0) continue;
        f32 sx = (clip.x / clip.w + 1.0f) * hw;
#if defined(ULDUM_BACKEND_GLES)
        f32 sy = (1.0f - clip.y / clip.w) * hh;
#else
        f32 sy = (clip.y / clip.w + 1.0f) * hh;
#endif

        if (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) {
            result.push_back(simulation::Unit{id});
        }
    }

    // Sort by priority (descending)
    std::sort(result.begin(), result.end(), [&](const simulation::Unit& a, const simulation::Unit& b) {
        auto* sa = selectables.get(a.id);
        auto* sb = selectables.get(b.id);
        i32 pa = sa ? sa->priority : 0;
        i32 pb = sb ? sb->priority : 0;
        return pa > pb;
    });

    return result;
}

} // namespace uldum::input
