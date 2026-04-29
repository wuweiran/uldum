#include "input/picking.h"

#include "simulation/fog_of_war.h"

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
static bool fog_visible(const simulation::FogOfWar* fog,
                        simulation::Player local,
                        const map::TerrainData* terrain,
                        const glm::vec3& world_pos) {
    if (!fog || !terrain || !terrain->is_valid()) return true;
    f32 ts = terrain->tile_size;
    if (ts <= 0.0f) return true;
    i32 tx = static_cast<i32>((world_pos.x - terrain->origin_x()) / ts);
    i32 ty = static_cast<i32>((world_pos.y - terrain->origin_y()) / ts);
    if (tx < 0 || ty < 0 ||
        static_cast<u32>(tx) >= terrain->tiles_x ||
        static_cast<u32>(ty) >= terrain->tiles_y) return true;
    return fog->is_visible(local, static_cast<u32>(tx), static_cast<u32>(ty));
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
    f32 ndc_y = (2.0f * sy / h) - 1.0f;

    glm::mat4 inv_vp = glm::inverse(m_camera->view_projection());
    glm::vec4 near_clip{ndc_x, ndc_y, 0.0f, 1.0f};
    glm::vec4 near_world = inv_vp * near_clip;
    near_world /= near_world.w;
    return glm::vec3(near_world);
}

glm::vec3 Picker::screen_to_ray(f32 sx, f32 sy) const {
    f32 w = static_cast<f32>(m_screen_w);
    f32 h = static_cast<f32>(m_screen_h);
    f32 ndc_x = (2.0f * sx / w) - 1.0f;
    f32 ndc_y = (2.0f * sy / h) - 1.0f;

    glm::mat4 inv_vp = glm::inverse(m_camera->view_projection());
    glm::vec4 near_clip{ndc_x, ndc_y, 0.0f, 1.0f};
    glm::vec4 far_clip{ndc_x, ndc_y, 1.0f, 1.0f};

    glm::vec4 near_world = inv_vp * near_clip;
    glm::vec4 far_world  = inv_vp * far_clip;
    near_world /= near_world.w;
    far_world  /= far_world.w;

    return glm::normalize(glm::vec3(far_world) - glm::vec3(near_world));
}

bool Picker::screen_to_world(f32 screen_x, f32 screen_y, glm::vec3& world_pos) const {
    if (!m_camera || !m_terrain || !m_terrain->is_valid()) return false;

    glm::vec3 origin = ray_origin(screen_x, screen_y);
    glm::vec3 dir = screen_to_ray(screen_x, screen_y);

    const f32 ox = m_terrain->origin_x();
    const f32 oy = m_terrain->origin_y();
    const f32 wx = m_terrain->world_width();
    const f32 wy = m_terrain->world_height();

    // Bilinear terrain Z lookup at world (x, y). Returns the height of
    // the height-map sampled across the cell containing (x, y); used by
    // both the march and the bisection refinement.
    auto sample_z = [&](f32 wx_, f32 wy_) -> f32 {
        f32 fx = (wx_ - ox) / m_terrain->tile_size;
        f32 fy = (wy_ - oy) / m_terrain->tile_size;
        u32 ix = std::min(static_cast<u32>(fx), m_terrain->tiles_x - 1);
        u32 iy = std::min(static_cast<u32>(fy), m_terrain->tiles_y - 1);
        f32 lx = fx - static_cast<f32>(ix);
        f32 ly = fy - static_cast<f32>(iy);
        f32 h00 = m_terrain->world_z_at(ix, iy);
        f32 h10 = m_terrain->world_z_at(ix + 1, iy);
        f32 h01 = m_terrain->world_z_at(ix, iy + 1);
        f32 h11 = m_terrain->world_z_at(ix + 1, iy + 1);
        return h00 + lx * (h10 - h00) + ly * (h01 - h00) + lx * ly * (h00 - h10 - h01 + h11);
    };

    auto in_bounds = [&](glm::vec3 p) {
        return p.x >= ox && p.y >= oy && p.x <= ox + wx && p.y <= oy + wy;
    };

    // Coarse march finds the bracket: last `t` where the ray is *above*
    // terrain (`signed_dist > 0`) and first `t` where it has crossed
    // *below* (`signed_dist <= 0`). Step size = half a tile; finer than
    // tile-size avoids skipping over a cell ridge but coarse enough
    // that a 100k-unit ray finishes in ~1500 iterations.
    f32 step = m_terrain->tile_size * 0.5f;
    f32 prev_t = -1.0f;     // last in-bounds t (above terrain)
    f32 hit_t  = -1.0f;     // first in-bounds t (below terrain)
    for (f32 t = 0.0f; t < 100000.0f; t += step) {
        glm::vec3 p = origin + dir * t;
        if (!in_bounds(p)) continue;
        f32 sd = p.z - sample_z(p.x, p.y);  // >0 above, <=0 below
        if (sd <= 0.0f) {
            hit_t = t;
            break;
        }
        prev_t = t;
    }
    if (hit_t < 0.0f) return false;
    if (prev_t < 0.0f) {
        // Ray started below terrain in the very first step (camera dipped
        // under the ground? unlikely). Just return the hit point as-is.
        glm::vec3 p = origin + dir * hit_t;
        world_pos = p;
        world_pos.z = sample_z(p.x, p.y);
        return true;
    }

    // Bisect until the bracket is sub-tile to remove the 64-unit
    // quantization the march alone produces. A handful of iterations
    // brings (hit_t - prev_t) into floating-point noise territory.
    constexpr int kBisectIters = 16;
    for (int i = 0; i < kBisectIters; ++i) {
        f32 mid_t = 0.5f * (prev_t + hit_t);
        glm::vec3 p = origin + dir * mid_t;
        if (!in_bounds(p)) {
            // Shouldn't happen — both endpoints are in-bounds.
            break;
        }
        f32 sd = p.z - sample_z(p.x, p.y);
        if (sd > 0.0f) prev_t = mid_t;
        else           hit_t  = mid_t;
    }
    glm::vec3 hit = origin + dir * hit_t;
    world_pos = hit;
    world_pos.z = sample_z(hit.x, hit.y);
    return true;
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

simulation::Unit Picker::pick_unit(f32 screen_x, f32 screen_y,
                                    simulation::Player player) const {
    if (!m_camera || !m_world) return {};

    glm::vec3 origin = ray_origin(screen_x, screen_y);
    glm::vec3 dir = screen_to_ray(screen_x, screen_y);

    simulation::Unit best{};
    f32 best_ray_t = 1e9f;
    i32 best_priority = -1;

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
        if (!info || info->category != simulation::Category::Unit) continue;
        if (m_world->dead_states.has(id)) continue;

        if (player.is_valid()) {
            auto* own = owners.get(id);
            if (!own || own->player.id != player.id) continue;
        }

        // Fog filter — entities under the fog of war can't be clicked
        // on. Player's own units pass through (fog hides what the
        // player hasn't scouted, not their own troops).
        if (m_local_player.is_valid()) {
            auto* own = owners.get(id);
            bool is_own = own && own->player.id == m_local_player.id;
            if (!is_own && !fog_visible(m_fog, m_local_player, m_terrain, transform->position)) continue;
        }

        f32 ray_t;
        f32 dist = ray_cylinder_dist(origin, dir, transform->position, sel.selection_height, ray_t);


        // Pick radius covers both the ground circle and the visual model body.
        // transform->scale approximates the model's visual width.
        f32 pick_radius = std::max(sel.selection_radius, transform->scale);
        if (dist > pick_radius) continue;

        // Prefer higher priority, then closer along the ray
        if (sel.priority > best_priority ||
            (sel.priority == best_priority && ray_t < best_ray_t)) {
            best.id = id;
            best.generation = info->generation;
            best_ray_t = ray_t;
            best_priority = sel.priority;
        }
    }
    return best;
}

simulation::Unit Picker::pick_target(f32 screen_x, f32 screen_y) const {
    return pick_unit(screen_x, screen_y, {}); // no player filter — pick any unit
}

simulation::Item Picker::pick_item(f32 screen_x, f32 screen_y) const {
    if (!m_camera || !m_world) return {};

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
        // Skip items currently being carried (they're "inside" a unit).
        auto* car = carriables.get(id);
        if (car && car->carried_by.is_valid()) continue;

        auto* transform = transforms.get(id);
        if (!transform) continue;

        // Fog filter — items in unscouted tiles aren't clickable.
        if (!fog_visible(m_fog, m_local_player, m_terrain, transform->position)) continue;

        f32 ray_t;
        f32 dist = ray_cylinder_dist(origin, dir, transform->position,
                                     sel.selection_height, ray_t);
        f32 pick_radius = std::max(sel.selection_radius, transform->scale);
        if (dist > pick_radius) continue;

        if (ray_t < best_ray_t) {
            best.id = id;
            best.generation = info->generation;
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

        auto* own = owners.get(id);
        if (!own || own->player.id != player.id) continue;

        auto* transform = transforms.get(id);
        if (!transform) continue;

        // Project world position to screen
        glm::vec4 clip = vp * glm::vec4(transform->position, 1.0f);
        if (clip.w <= 0) continue;
        f32 sx = (clip.x / clip.w + 1.0f) * hw;
        f32 sy = (clip.y / clip.w + 1.0f) * hh;  // Vulkan: Y not flipped in projection

        if (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) {
            simulation::Unit u;
            u.id = id;
            u.generation = info->generation;
            result.push_back(u);
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
