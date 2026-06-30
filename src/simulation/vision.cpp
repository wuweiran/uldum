#include "simulation/vision.h"
#include "simulation/world.h"
#include "simulation/simulation.h"
#include "simulation/components.h"
#include "simulation/spatial_query.h"
#include "map/terrain_data.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace uldum::simulation {

// Brightness targets for each visibility state
static constexpr f32 BRIGHTNESS_UNEXPLORED = 0.0f;
static constexpr f32 BRIGHTNESS_EXPLORED   = 0.4f;
static constexpr f32 BRIGHTNESS_VISIBLE    = 1.0f;

// Lerp speeds (units per second)
static constexpr f32 REVEAL_SPEED = 4.0f;   // unexplored/explored → visible (fast reveal)
static constexpr f32 FADE_SPEED   = 2.0f;   // visible → explored (slower fade)

// Feather zone: tiles within this distance of the vision edge get partial brightness.
// 1.5 tiles of falloff gives a smooth circular edge.
static constexpr f32 FEATHER_TILES = 1.5f;

void Vision::init(u32 tiles_x, u32 tiles_y, f32 tile_size, u32 player_count, FogMode mode,
                    const map::TerrainData* terrain) {
    m_tiles_x = tiles_x;
    m_tiles_y = tiles_y;
    m_tile_size = tile_size;
    m_player_count = player_count;
    m_mode = mode;
    m_authored_mode = mode;
    m_terrain = terrain;

    if (mode == FogMode::None) {
        m_grids.clear();
        m_targets.clear();
        m_visual.clear();
        return;
    }

    u32 total = player_count * tiles_x * tiles_y;

    if (mode == FogMode::Explored) {
        // Map starts pre-explored: terrain visible (dimmed), enemies hidden outside vision
        m_grids.assign(total, static_cast<u8>(Visibility::Explored));
        m_targets.assign(total, BRIGHTNESS_EXPLORED);
        m_visual.assign(total, BRIGHTNESS_EXPLORED);
    } else {
        // Unexplored: everything starts black
        m_grids.assign(total, static_cast<u8>(Visibility::Unexplored));
        m_targets.assign(total, BRIGHTNESS_UNEXPLORED);
        m_visual.assign(total, BRIGHTNESS_UNEXPLORED);
    }
}

void Vision::update(World& world, const Simulation& sim) {
    if (m_mode == FogMode::None) return;

    const u32 grid_size = m_tiles_x * m_tiles_y;

    // Phase 1: Decay all Visible → Explored, reset targets to explored/unexplored base
    for (u32 p = 0; p < m_player_count; ++p) {
        u8* grid = &m_grids[p * grid_size];
        f32* target = &m_targets[p * grid_size];
        for (u32 i = 0; i < grid_size; ++i) {
            if (grid[i] == static_cast<u8>(Visibility::Visible)) {
                grid[i] = static_cast<u8>(Visibility::Explored);
            }
            // Reset target to base state — vision circles will raise it
            target[i] = (grid[i] >= static_cast<u8>(Visibility::Explored))
                ? BRIGHTNESS_EXPLORED : BRIGHTNESS_UNEXPLORED;
        }
    }

    // Phase 2: Mark vision circles with feathered edges
    for (u32 i = 0; i < world.sights.count(); ++i) {
        u32 id = world.sights.ids()[i];
        const auto& sight = world.sights.data()[i];

        const auto* owner = world.owners.get(id);
        const auto* transform = world.transforms.get(id);
        if (!owner || !transform) continue;
        if (world.dead_states.has(id)) continue;

        u32 player_id = owner->id;
        if (player_id >= m_player_count) continue;

        // Sub-tile precision: use exact position within the tile grid.
        // Shift by terrain origin so world (0,0) = map center maps correctly
        // to grid (tiles_x/2, tiles_y/2).
        f32 ox = m_terrain ? m_terrain->origin_x() : 0.0f;
        f32 oy = m_terrain ? m_terrain->origin_y() : 0.0f;
        f32 cx = (transform->position.x - ox) / m_tile_size;
        f32 cy = (transform->position.y - oy) / m_tile_size;
        f32 radius = sight.sight_range / m_tile_size;

        // Viewer's cliff level (from Movement component or terrain).
        // Air units see over everything — they fly above the terrain, so no
        // cliff blocks their line of sight. Max cliff level → has_cliff_los
        // never finds a higher intermediate tile.
        u8 viewer_cliff = 0;
        const auto* mov = world.movements.get(id);
        if (mov && mov->type == MoveType::Air) {
            viewer_cliff = 255;
        } else if (mov) {
            viewer_cliff = mov->cliff_level;
        } else if (m_terrain) {
            viewer_cliff = m_terrain->cliff_level_at(transform->position.x, transform->position.y);
        }

        mark_vision_circle(player_id, cx, cy, radius, viewer_cliff);

        // Shared vision: only if explicitly enabled via alliance flags
        for (u32 p = 0; p < m_player_count; ++p) {
            if (p == player_id) continue;
            if (sim.has_shared_vision(Player{p}, Player{player_id})) {
                mark_vision_circle(p, cx, cy, radius, viewer_cliff);
            }
        }

        // Per-unit vision share (UnitShareVision).
        for (u32 share_p : sight.share_to_players) {
            if (share_p == player_id || share_p >= m_player_count) continue;
            mark_vision_circle(share_p, cx, cy, radius, viewer_cliff);
        }
    }

    // Phase 2.5: Fog modifiers — persistent area overrides on top of
    // the unit-vision pass. Author-controlled cinematic reveal / conceal.
    for (auto& fm : m_fog_modifiers) {
        if (fm.active) apply_fog_modifier(fm);
    }

    // Phase 3: True-sight detection. For every unit with numeric
    // attribute `true_sight > 0`, find enemy units within that radius
    // carrying UNIT_STATUS_INVISIBLE and stamp the detector's player
    // bit onto their TrueSightVisibility component. This is the only
    // mechanism that lets a non-allied player see through invisibility
    // — both the renderer cull AND the server snapshot filter read it.
    auto& tsv = world.true_sight_vis;
    tsv.clear();
    const auto& attrs = world.attribute_blocks;
    const auto& grid_q = sim.spatial_grid();
    for (u32 i = 0; i < attrs.count(); ++i) {
        u32 detector_id = attrs.ids()[i];
        const auto& ab = attrs.data()[i];
        auto it = ab.numeric.find("true_sight");
        if (it == ab.numeric.end() || it->second <= 0.0f) continue;
        if (world.dead_states.has(detector_id)) continue;

        const auto* d_owner = world.owners.get(detector_id);
        const auto* d_transform = world.transforms.get(detector_id);
        if (!d_owner || !d_transform) continue;
        if (d_owner->id >= 32) continue;  // mask is u32

        UnitFilter filter;
        filter.enemy_of  = *d_owner;
        filter.alive_only = true;
        filter.include_untargetable = true;  // wind walk often pairs with untargetable
        filter.predicate = [&world](Unit u) -> bool {
            const auto* sf = world.status_flags.get(u.id);
            return sf && (sf->flags & status::Invisible);
        };

        auto revealed = grid_q.units_in_range(world, d_transform->position,
                                              it->second, filter);
        u32 bit = 1u << d_owner->id;
        for (Unit target : revealed) {
            auto* existing = tsv.get(target.id);
            if (existing) existing->revealed_to_mask |= bit;
            else tsv.add(target.id, TrueSightVisibility{bit});
        }
    }
}

bool Vision::is_unit_visible_to(const World& world, const Simulation& sim,
                                  u32 entity_id, Player player,
                                  bool remembered_ok) const {
    const u32 player_bit = 1u << player.id;

    // Friendly (own / allied) — always visible
    const auto* owner = world.owners.get(entity_id);
    if (owner && owner->id == player.id) return true;
    if (owner && sim.is_allied(player, *owner)) return true;

    // UnitReveal — explicit per-player override, bypasses invisibility + fog
    const auto* fv = world.forced_vis.get(entity_id);
    if (fv && (fv->revealed_to_mask & player_bit)) return true;

    // Invisibility — hidden unless this player has a true-sight
    // detector covering the unit this tick
    const auto* sf = world.status_flags.get(entity_id);
    if (sf && (sf->flags & status::Invisible)) {
        const auto* tv = world.true_sight_vis.get(entity_id);
        const bool revealed = tv && (tv->revealed_to_mask & player_bit);
        if (!revealed) return false;
    }

    // Fog of war (tile state)
    if (m_mode == FogMode::None) return true;
    const auto* transform = world.transforms.get(entity_id);
    if (!transform) return false;
    if (!m_terrain) return true;
    auto tile = m_terrain->world_to_tile(transform->position.x,
                                         transform->position.y);
    u32 tx = static_cast<u32>(tile.x);
    u32 ty = static_cast<u32>(tile.y);
    return remembered_ok ? is_explored(player, tx, ty)
                         : is_visible(player, tx, ty);
}

void Vision::mark_vision_circle(u32 player_id, f32 cx, f32 cy, f32 radius_tiles, u8 viewer_cliff) {
    // Scan bounding box including feather zone
    f32 outer = radius_tiles + FEATHER_TILES;
    u32 min_x = static_cast<u32>(std::max(0.0f, cx - outer));
    u32 max_x = std::min(static_cast<u32>(cx + outer), m_tiles_x - 1);
    u32 min_y = static_cast<u32>(std::max(0.0f, cy - outer));
    u32 max_y = std::min(static_cast<u32>(cy + outer), m_tiles_y - 1);

    f32 inner = radius_tiles;  // full brightness inside this radius
    u8* grid = &m_grids[player_id * (m_tiles_x * m_tiles_y)];
    f32* target = &m_targets[player_id * (m_tiles_x * m_tiles_y)];

    u32 src_tx = static_cast<u32>(std::clamp(cx, 0.0f, static_cast<f32>(m_tiles_x - 1)));
    u32 src_ty = static_cast<u32>(std::clamp(cy, 0.0f, static_cast<f32>(m_tiles_y - 1)));

    for (u32 ty = min_y; ty <= max_y; ++ty) {
        for (u32 tx = min_x; tx <= max_x; ++tx) {
            f32 dx = static_cast<f32>(tx) - cx;
            f32 dy = static_cast<f32>(ty) - cy;
            f32 dist = std::sqrt(dx * dx + dy * dy);

            if (dist <= outer) {
                // Check cliff LOS — skip tiles blocked by higher cliffs
                if (m_terrain && !has_cliff_los(src_tx, src_ty, tx, ty, viewer_cliff)) continue;

                u32 idx = ty * m_tiles_x + tx;

                // Mark logical state as visible (inside the full circle)
                if (dist <= inner) {
                    grid[idx] = static_cast<u8>(Visibility::Visible);
                    target[idx] = BRIGHTNESS_VISIBLE;
                } else {
                    // Feather zone: smoothstep falloff from visible to explored
                    f32 t = (dist - inner) / FEATHER_TILES;  // 0 at inner edge, 1 at outer edge
                    t = std::clamp(t, 0.0f, 1.0f);
                    // Smoothstep for nicer curve
                    f32 fade = 1.0f - (t * t * (3.0f - 2.0f * t));
                    f32 base = (grid[idx] >= static_cast<u8>(Visibility::Explored))
                        ? BRIGHTNESS_EXPLORED : BRIGHTNESS_UNEXPLORED;
                    f32 feathered = base + (BRIGHTNESS_VISIBLE - base) * fade;
                    target[idx] = std::max(target[idx], feathered);

                    // Feather zone still counts as explored
                    if (grid[idx] < static_cast<u8>(Visibility::Explored)) {
                        grid[idx] = static_cast<u8>(Visibility::Explored);
                    }
                }
            }
        }
    }
}

bool Vision::has_cliff_los(u32 x0, u32 y0, u32 x1, u32 y1, u8 viewer_cliff) const {
    if (!m_terrain) return true;

    // Bresenham line walk from (x0,y0) to (x1,y1)
    i32 dx = static_cast<i32>(x1) - static_cast<i32>(x0);
    i32 dy = static_cast<i32>(y1) - static_cast<i32>(y0);
    i32 abs_dx = std::abs(dx);
    i32 abs_dy = std::abs(dy);
    i32 sx = (dx > 0) ? 1 : -1;
    i32 sy = (dy > 0) ? 1 : -1;

    i32 cx = static_cast<i32>(x0);
    i32 cy = static_cast<i32>(y0);

    if (abs_dx >= abs_dy) {
        i32 err = abs_dx / 2;
        for (i32 i = 0; i < abs_dx; ++i) {
            cx += sx;
            err -= abs_dy;
            if (err < 0) {
                cy += sy;
                err += abs_dx;
            }
            // Skip the destination tile itself — only check intermediate tiles
            if (cx == static_cast<i32>(x1) && cy == static_cast<i32>(y1)) break;

            // Check if this intermediate tile blocks LOS
            u32 tx = static_cast<u32>(cx);
            u32 ty = static_cast<u32>(cy);
            if (tx < m_tiles_x && ty < m_tiles_y) {
                // Max cliff level of the tile's 4 corners
                u8 c00 = m_terrain->cliff_at(tx, ty);
                u8 c10 = m_terrain->cliff_at(tx + 1, ty);
                u8 c01 = m_terrain->cliff_at(tx, ty + 1);
                u8 c11 = m_terrain->cliff_at(tx + 1, ty + 1);
                u8 max_cliff = std::max({c00, c10, c01, c11});
                if (max_cliff > viewer_cliff) return false;
            }
        }
    } else {
        i32 err = abs_dy / 2;
        for (i32 i = 0; i < abs_dy; ++i) {
            cy += sy;
            err -= abs_dx;
            if (err < 0) {
                cx += sx;
                err += abs_dy;
            }
            if (cx == static_cast<i32>(x1) && cy == static_cast<i32>(y1)) break;

            u32 tx = static_cast<u32>(cx);
            u32 ty = static_cast<u32>(cy);
            if (tx < m_tiles_x && ty < m_tiles_y) {
                u8 c00 = m_terrain->cliff_at(tx, ty);
                u8 c10 = m_terrain->cliff_at(tx + 1, ty);
                u8 c01 = m_terrain->cliff_at(tx, ty + 1);
                u8 c11 = m_terrain->cliff_at(tx + 1, ty + 1);
                u8 max_cliff = std::max({c00, c10, c01, c11});
                if (max_cliff > viewer_cliff) return false;
            }
        }
    }

    return true;
}

const f32* Vision::update_visual(Player player, f32 dt) {
    if (m_mode == FogMode::None || player.id >= m_player_count) return nullptr;

    u32 grid_size = m_tiles_x * m_tiles_y;
    f32* visual = &m_visual[player.id * grid_size];
    const f32* target = &m_targets[player.id * grid_size];

    for (u32 i = 0; i < grid_size; ++i) {
        f32 v = visual[i];
        f32 t = target[i];
        if (v < t) {
            // Revealing — fast
            v = std::min(v + REVEAL_SPEED * dt, t);
        } else if (v > t) {
            // Fading — slower
            v = std::max(v - FADE_SPEED * dt, t);
        }
        visual[i] = v;
    }

    return visual;
}

Visibility Vision::get(Player player, u32 tx, u32 ty) const {
    if (m_mode == FogMode::None) return Visibility::Visible;
    if (player.id >= m_player_count || tx >= m_tiles_x || ty >= m_tiles_y) return Visibility::Unexplored;
    return static_cast<Visibility>(m_grids[index(player.id, tx, ty)]);
}

bool Vision::is_visible(Player player, u32 tx, u32 ty) const {
    return get(player, tx, ty) == Visibility::Visible;
}

bool Vision::is_explored(Player player, u32 tx, u32 ty) const {
    return get(player, tx, ty) >= Visibility::Explored;
}

const u8* Vision::grid(Player player) const {
    if (m_mode == FogMode::None || player.id >= m_player_count) return nullptr;
    return &m_grids[player.id * (m_tiles_x * m_tiles_y)];
}

void Vision::set_enabled(bool on) {
    if (on) {
        if (m_mode == FogMode::None) {
            init(m_tiles_x, m_tiles_y, m_tile_size, m_player_count,
                 m_authored_mode, m_terrain);
        }
    } else {
        m_mode = FogMode::None;
    }
}

u32 Vision::create_fog_modifier_rect(Player p, Visibility state,
                                      f32 x0, f32 y0, f32 x1, f32 y1) {
    FogModifier fm;
    fm.id     = ++m_next_modifier_id;
    fm.player = p;
    fm.state  = state;
    fm.shape  = Shape::Rect;
    fm.x0 = std::min(x0, x1); fm.x1 = std::max(x0, x1);
    fm.y0 = std::min(y0, y1); fm.y1 = std::max(y0, y1);
    m_fog_modifiers.push_back(fm);
    return fm.id;
}

u32 Vision::create_fog_modifier_radius(Player p, Visibility state,
                                        f32 cx, f32 cy, f32 radius) {
    FogModifier fm;
    fm.id     = ++m_next_modifier_id;
    fm.player = p;
    fm.state  = state;
    fm.shape  = Shape::Radius;
    fm.cx = cx; fm.cy = cy; fm.radius = radius;
    m_fog_modifiers.push_back(fm);
    return fm.id;
}

void Vision::destroy_fog_modifier(u32 id) {
    std::erase_if(m_fog_modifiers, [id](const FogModifier& fm) { return fm.id == id; });
}

void Vision::set_fog_modifier_active(u32 id, bool active) {
    for (auto& fm : m_fog_modifiers) {
        if (fm.id == id) { fm.active = active; return; }
    }
}

void Vision::apply_fog_modifier(const FogModifier& fm) {
    if (!m_terrain || m_tile_size <= 0) return;
    if (fm.player.id >= m_player_count) return;

    f32 ox = m_terrain->origin_x();
    f32 oy = m_terrain->origin_y();
    u8* grid = &m_grids[fm.player.id * (m_tiles_x * m_tiles_y)];
    f32* target = &m_targets[fm.player.id * (m_tiles_x * m_tiles_y)];

    f32 brightness =
        (fm.state == Visibility::Visible)    ? BRIGHTNESS_VISIBLE :
        (fm.state == Visibility::Explored)   ? BRIGHTNESS_EXPLORED :
                                                BRIGHTNESS_UNEXPLORED;

    auto stamp = [&](u32 tx, u32 ty) {
        u32 idx = ty * m_tiles_x + tx;
        grid[idx]   = static_cast<u8>(fm.state);
        target[idx] = brightness;
    };

    if (fm.shape == Shape::Rect) {
        i32 min_tx = static_cast<i32>(std::floor((fm.x0 - ox) / m_tile_size));
        i32 min_ty = static_cast<i32>(std::floor((fm.y0 - oy) / m_tile_size));
        i32 max_tx = static_cast<i32>(std::ceil ((fm.x1 - ox) / m_tile_size));
        i32 max_ty = static_cast<i32>(std::ceil ((fm.y1 - oy) / m_tile_size));
        min_tx = std::max(0, min_tx);
        min_ty = std::max(0, min_ty);
        max_tx = std::min<i32>(static_cast<i32>(m_tiles_x) - 1, max_tx);
        max_ty = std::min<i32>(static_cast<i32>(m_tiles_y) - 1, max_ty);
        for (i32 ty = min_ty; ty <= max_ty; ++ty) {
            for (i32 tx = min_tx; tx <= max_tx; ++tx) {
                stamp(static_cast<u32>(tx), static_cast<u32>(ty));
            }
        }
    } else {
        f32 cx_tile = (fm.cx - ox) / m_tile_size;
        f32 cy_tile = (fm.cy - oy) / m_tile_size;
        f32 r_tile  = fm.radius / m_tile_size;
        i32 min_tx = std::max(0, static_cast<i32>(std::floor(cx_tile - r_tile)));
        i32 min_ty = std::max(0, static_cast<i32>(std::floor(cy_tile - r_tile)));
        i32 max_tx = std::min<i32>(static_cast<i32>(m_tiles_x) - 1,
                                    static_cast<i32>(std::ceil(cx_tile + r_tile)));
        i32 max_ty = std::min<i32>(static_cast<i32>(m_tiles_y) - 1,
                                    static_cast<i32>(std::ceil(cy_tile + r_tile)));
        f32 r2 = r_tile * r_tile;
        for (i32 ty = min_ty; ty <= max_ty; ++ty) {
            for (i32 tx = min_tx; tx <= max_tx; ++tx) {
                f32 dx = static_cast<f32>(tx) + 0.5f - cx_tile;
                f32 dy = static_cast<f32>(ty) + 0.5f - cy_tile;
                if (dx * dx + dy * dy <= r2) {
                    stamp(static_cast<u32>(tx), static_cast<u32>(ty));
                }
            }
        }
    }
}

} // namespace uldum::simulation
