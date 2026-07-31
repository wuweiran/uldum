#include "simulation/placement.h"

#include "simulation/simulation.h"
#include "simulation/pathfinding.h"
#include "simulation/world.h"
#include "simulation/type_registry.h"
#include "simulation/components.h"
#include "map/terrain_data.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace uldum::simulation {

bool tile_buildable(const Pathfinder& pf, const map::TerrainData& td,
                    i32 tx, i32 ty, MoveType move_type) {
    if (tx < 0 || ty < 0) return false;
    if (static_cast<u32>(tx) >= td.tiles_x || static_cast<u32>(ty) >= td.tiles_y) return false;
    // Structures require FLAT ground: tile_effective_level >= 0 is a flat
    // tile at that cliff level; -1 is a ramp (walkable but not buildable);
    // -2 is a cliff wall. Ramps/cliffs are unbuildable even though a walker
    // could cross a ramp.
    if (td.tile_effective_level(static_cast<u32>(tx), static_cast<u32>(ty)) < 0) return false;
    // Every pathing cell in the tile must be occupiable (passable terrain,
    // not deep water for ground, no runtime blocker). Reuses can_occupy_cell.
    i32 cx0 = tx * static_cast<i32>(PATHING_SUBDIV);
    i32 cy0 = ty * static_cast<i32>(PATHING_SUBDIV);
    for (i32 dy = 0; dy < static_cast<i32>(PATHING_SUBDIV); ++dy) {
        for (i32 dx = 0; dx < static_cast<i32>(PATHING_SUBDIV); ++dx) {
            if (!pf.can_occupy_cell(cx0 + dx, cy0 + dy, move_type)) return false;
        }
    }
    return true;
}

bool footprint_buildable(const Pathfinder& pf, const map::TerrainData& td,
                         f32 wx, f32 wy, u32 fw, u32 fh, MoveType move_type) {
    if (fw == 0 || fh == 0) return true;
    if (!td.is_valid()) return true;
    f32 left_tx_f   = (wx - td.origin_x()) / td.tile_size - 0.5f * static_cast<f32>(fw);
    f32 bottom_ty_f = (wy - td.origin_y()) / td.tile_size - 0.5f * static_cast<f32>(fh);
    i32 tx0 = static_cast<i32>(std::round(left_tx_f));
    i32 ty0 = static_cast<i32>(std::round(bottom_ty_f));
    for (u32 j = 0; j < fh; ++j) {
        for (u32 i = 0; i < fw; ++i) {
            if (!tile_buildable(pf, td, tx0 + static_cast<i32>(i),
                                ty0 + static_cast<i32>(j), move_type)) {
                return false;
            }
        }
    }
    return true;
}

bool footprint_clear(const Pathfinder& pf, const map::TerrainData& td,
                     f32 wx, f32 wy, u32 fw, u32 fh, bool in_cells,
                     MoveType move_type) {
    if (fw == 0 || fh == 0) return true;
    if (!td.is_valid()) return true;
    f32 step = in_cells
        ? td.tile_size / static_cast<f32>(PATHING_SUBDIV)
        : td.tile_size;
    f32 left_f   = (wx - td.origin_x()) / step - 0.5f * static_cast<f32>(fw);
    f32 bottom_f = (wy - td.origin_y()) / step - 0.5f * static_cast<f32>(fh);
    i32 c0x = static_cast<i32>(std::round(left_f));
    i32 c0y = static_cast<i32>(std::round(bottom_f));
    i32 step_cells = in_cells ? 1 : static_cast<i32>(PATHING_SUBDIV);
    i32 cw = static_cast<i32>(fw) * step_cells;
    i32 ch = static_cast<i32>(fh) * step_cells;
    i32 cx0 = c0x * step_cells;
    i32 cy0 = c0y * step_cells;
    for (i32 dy = 0; dy < ch; ++dy) {
        for (i32 dx = 0; dx < cw; ++dx) {
            if (!pf.can_occupy_cell(cx0 + dx, cy0 + dy, move_type)) {
                return false;
            }
        }
    }
    return true;
}

bool collision_overlaps(const World& world, f32 wx, f32 wy, f32 radius,
                        MoveType move_type, u32 ignore_id) {
    if (radius <= 0) return false;
    bool my_air = (move_type == MoveType::Fly);
    for (u32 i = 0; i < world.movements.count(); ++i) {
        u32 id = world.movements.ids()[i];
        if (id == ignore_id) continue;
        const auto& other = world.movements.data()[i];
        if ((other.type == MoveType::Fly) != my_air) continue;
        const auto* t = world.transforms.get(id);
        if (!t) continue;
        f32 dx = t->position.x - wx;
        f32 dy = t->position.y - wy;
        f32 min_dist = radius + other.collision_radius;
        if (dx * dx + dy * dy < min_dist * min_dist) return true;
    }
    return false;
}

bool is_displaceable(const World& world, u32 unit_id, u32 builder_owner_id) {
    // Own unit.
    const auto* owner = world.owners.get(unit_id);
    if (!owner || owner->id != builder_owner_id) return false;
    // Movable — not a fixed structure.
    const auto* mv = world.movements.get(unit_id);
    if (!mv || mv->type == MoveType::None) return false;
    if (world.buildings.has(unit_id)) return false;
    // Idle — not carrying out an order, fighting, or casting.
    if (const auto* oq = world.order_queues.get(unit_id); oq && oq->current) return false;
    if (const auto* cb = world.combats.get(unit_id);
        cb && cb->attack_state != AttackState::Idle) return false;
    if (const auto* as = world.ability_sets.get(unit_id);
        as && as->cast_state != CastState::None) return false;
    return true;
}

std::vector<u32> footprint_occupants(const World& world, const map::TerrainData& td,
                                     f32 wx, f32 wy, u32 fw, u32 fh,
                                     MoveType move_type, u32 ignore_id) {
    std::vector<u32> out;
    if (fw == 0 || fh == 0 || !td.is_valid()) return out;
    // Footprint tile-rect in world space (SW corner + extent), same snap math
    // as evaluate_building_placement's tile mask.
    f32 left_tx_f   = (wx - td.origin_x()) / td.tile_size - 0.5f * static_cast<f32>(fw);
    f32 bottom_ty_f = (wy - td.origin_y()) / td.tile_size - 0.5f * static_cast<f32>(fh);
    i32 tx0 = static_cast<i32>(std::round(left_tx_f));
    i32 ty0 = static_cast<i32>(std::round(bottom_ty_f));
    f32 xl = td.origin_x() + static_cast<f32>(tx0) * td.tile_size;
    f32 yb = td.origin_y() + static_cast<f32>(ty0) * td.tile_size;
    f32 xr = xl + static_cast<f32>(fw) * td.tile_size;
    f32 yt = yb + static_cast<f32>(fh) * td.tile_size;

    const bool my_air = (move_type == MoveType::Fly);
    for (u32 i = 0; i < world.movements.count(); ++i) {
        u32 id = world.movements.ids()[i];
        if (id == ignore_id) continue;
        const auto& mv = world.movements.data()[i];
        if ((mv.type == MoveType::Fly) != my_air) continue;
        if (mv.collision_radius <= 0.0f) continue;
        const auto* t = world.transforms.get(id);
        if (!t) continue;
        // Circle (unit) vs. AABB (whole footprint): clamp the center into the
        // rect, compare squared distance to radius².
        f32 nx = std::clamp(t->position.x, xl, xr);
        f32 ny = std::clamp(t->position.y, yb, yt);
        f32 ddx = t->position.x - nx, ddy = t->position.y - ny;
        if (ddx * ddx + ddy * ddy < mv.collision_radius * mv.collision_radius) {
            out.push_back(id);
        }
    }
    return out;
}

BuildingPlacement evaluate_building_placement(const Simulation& sim,
                                              std::string_view type_id,
                                              f32 cursor_x, f32 cursor_y,
                                              u32 ignore_id, u32 owner_id) {
    BuildingPlacement out;
    const map::TerrainData* td = sim.terrain();
    if (!td || !td->is_valid()) return out;

    const auto* def = sim.types().get_unit_type(std::string(type_id));
    if (!def) return out;

    out.fw = def->pathing_footprint_w;
    out.fh = def->pathing_footprint_h;

    f32 wx = cursor_x, wy = cursor_y;
    if (out.fw > 0 && out.fh > 0) {
        wx = map::snap_building_x(*td, wx, out.fw);
        wy = map::snap_building_y(*td, wy, out.fh);
    }
    out.snapped = { wx, wy, map::sample_height(*td, wx, wy) };

    bool ok = true;
    if (out.fw > 0 && out.fh > 0) {
        // Per-tile buildability mask (row-major fw×fh). SW tile from the
        // snapped center: same math as footprint_clear / the block reservation.
        f32 left_tx_f   = (wx - td->origin_x()) / td->tile_size - 0.5f * static_cast<f32>(out.fw);
        f32 bottom_ty_f = (wy - td->origin_y()) / td->tile_size - 0.5f * static_cast<f32>(out.fh);
        i32 tx0 = static_cast<i32>(std::round(left_tx_f));
        i32 ty0 = static_cast<i32>(std::round(bottom_ty_f));
        out.tile_ok.assign(static_cast<usize>(out.fw) * out.fh, 0);
        // Pass 1: terrain / static-blocker buildability (flat, passable, no
        // reserved blocker). Dynamic units don't reserve grid cells, so this
        // pass alone leaves unit-occupied tiles green.
        for (u32 j = 0; j < out.fh; ++j) {
            for (u32 i = 0; i < out.fw; ++i) {
                bool b = tile_buildable(sim.pathfinder(), *td,
                                        tx0 + static_cast<i32>(i),
                                        ty0 + static_cast<i32>(j), def->move_type);
                out.tile_ok[static_cast<usize>(j) * out.fw + i] = b ? 1u : 0u;
            }
        }
        // Pass 2: dynamic unit occupancy (WC3). Units don't reserve grid cells,
        // so pass 1 misses them; a unit whose collision disc intrudes a tile
        // turns THAT tile red. Same-layer only (air vs surface never block).
        const World& w = sim.world();
        const bool my_air = (def->move_type == MoveType::Fly);
        for (u32 mi = 0; mi < w.movements.count(); ++mi) {
            u32 uid = w.movements.ids()[mi];
            if (uid == ignore_id) continue;
            const auto& mv = w.movements.data()[mi];
            if ((mv.type == MoveType::Fly) != my_air) continue;
            if (mv.collision_radius <= 0.0f) continue;
            // Displaceable own units don't block placement — they'll be pushed
            // off when the build lands, so their tiles stay green.
            if (is_displaceable(w, uid, owner_id)) continue;
            const auto* t = w.transforms.get(uid);
            if (!t) continue;
            const f32 ux = t->position.x, uy = t->position.y;
            const f32 r2 = mv.collision_radius * mv.collision_radius;
            for (u32 j = 0; j < out.fh; ++j) {
                f32 yb = td->origin_y() + static_cast<f32>(ty0 + static_cast<i32>(j)) * td->tile_size;
                f32 yt = yb + td->tile_size;
                f32 ny = std::clamp(uy, yb, yt);
                f32 ddy = uy - ny;
                for (u32 i = 0; i < out.fw; ++i) {
                    usize idx = static_cast<usize>(j) * out.fw + i;
                    if (!out.tile_ok[idx]) continue;   // already red
                    f32 xl = td->origin_x() + static_cast<f32>(tx0 + static_cast<i32>(i)) * td->tile_size;
                    f32 xr = xl + td->tile_size;
                    f32 nx = std::clamp(ux, xl, xr);
                    f32 ddx = ux - nx;
                    if (ddx * ddx + ddy * ddy < r2) out.tile_ok[idx] = 0u;
                }
            }
        }
        for (u8 v : out.tile_ok) if (!v) { ok = false; break; }
    } else if (def->collision_radius > 0) {
        i32 tx = static_cast<i32>(std::floor((wx - td->origin_x()) / td->tile_size));
        i32 ty = static_cast<i32>(std::floor((wy - td->origin_y()) / td->tile_size));
        ok = tile_buildable(sim.pathfinder(), *td, tx, ty, def->move_type);
        if (ok) {
            ok = !collision_overlaps(sim.world(), wx, wy, def->collision_radius,
                                     def->move_type, ignore_id);
        }
    }
    out.valid = ok;
    return out;
}

} // namespace uldum::simulation
