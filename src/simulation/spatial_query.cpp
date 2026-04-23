#include "simulation/spatial_query.h"
#include "simulation/simulation.h"
#include "simulation/world.h"
#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace uldum::simulation {

void SpatialGrid::init(f32 world_width, f32 world_height, f32 cell_size, const Simulation* sim) {
    m_world_width  = world_width;
    m_world_height = world_height;
    m_cell_size    = cell_size;
    m_sim          = sim;
    m_cells_x      = static_cast<u32>(std::ceil(world_width / cell_size));
    m_cells_y      = static_cast<u32>(std::ceil(world_height / cell_size));
    // World is centered on (0, 0), so the grid spans
    // [-world_width/2, +world_width/2] — without this offset, all units
    // in the negative quadrants would squash into cell (0, 0).
    m_origin_x     = -0.5f * world_width;
    m_origin_y     = -0.5f * world_height;
    m_cells.resize(m_cells_x * m_cells_y);

    log::info("SpatialGrid", "Initialized: {}x{} cells (cell_size={}, origin=({}, {}))",
              m_cells_x, m_cells_y, cell_size, m_origin_x, m_origin_y);
}

void SpatialGrid::update(const World& world) {
    // Clear all cells
    for (auto& cell : m_cells) {
        cell.units.clear();
    }

    // Insert all units with transforms
    auto& transforms = world.transforms;
    auto& handle_infos = world.handle_infos;

    for (u32 i = 0; i < handle_infos.count(); ++i) {
        u32 id = handle_infos.ids()[i];
        const auto& info = handle_infos.data()[i];
        if (info.category != Category::Unit) continue;

        const auto* t = transforms.get(id);
        if (!t) continue;

        i32 cx = static_cast<i32>((t->position.x - m_origin_x) / m_cell_size);
        i32 cy = static_cast<i32>((t->position.y - m_origin_y) / m_cell_size);
        cx = std::clamp(cx, 0, static_cast<i32>(m_cells_x) - 1);
        cy = std::clamp(cy, 0, static_cast<i32>(m_cells_y) - 1);

        Unit u;
        u.id = id;
        u.generation = info.generation;
        m_cells[cy * m_cells_x + cx].units.push_back(u);
    }
}

void SpatialGrid::get_cell_range(f32 x, f32 y, f32 radius, i32& min_cx, i32& min_cy, i32& max_cx, i32& max_cy) const {
    min_cx = std::max(0, static_cast<i32>((x - radius - m_origin_x) / m_cell_size));
    min_cy = std::max(0, static_cast<i32>((y - radius - m_origin_y) / m_cell_size));
    max_cx = std::min(static_cast<i32>(m_cells_x) - 1, static_cast<i32>((x + radius - m_origin_x) / m_cell_size));
    max_cy = std::min(static_cast<i32>(m_cells_y) - 1, static_cast<i32>((y + radius - m_origin_y) / m_cell_size));
}

bool SpatialGrid::passes_filter(const World& world, Unit unit, const UnitFilter& filter) const {
    if (!world.validate(unit)) return false;

    // Alive check
    if (filter.alive_only) {
        auto* hp = world.healths.get(unit.id);
        if (hp && hp->current <= 0) return false;
    }

    // Owner check
    if (filter.owner.is_valid()) {
        auto* o = world.owners.get(unit.id);
        if (!o || !(o->player == filter.owner)) return false;
    }

    // Enemy check: exclude allies (uses alliance system if available, falls back to same-player check)
    if (filter.enemy_of.is_valid()) {
        auto* o = world.owners.get(unit.id);
        if (o) {
            if (m_sim) {
                if (m_sim->is_allied(filter.enemy_of, o->player)) return false;
            } else {
                if (o->player == filter.enemy_of) return false;
            }
        }
    }

    // Classification check
    if (!filter.classifications.empty()) {
        auto* cls = world.classifications.get(unit.id);
        if (!cls) return false;
        for (auto& required : filter.classifications) {
            if (!has_classification(cls->flags, required)) return false;
        }
    }

    // Exclude buildings
    if (filter.exclude_buildings) {
        auto* cls = world.classifications.get(unit.id);
        if (cls && has_classification(cls->flags, "structure")) return false;
    }

    // Custom predicate
    if (filter.predicate && !filter.predicate(unit)) return false;

    return true;
}

std::vector<Unit> SpatialGrid::units_in_range(const World& world, glm::vec3 center, f32 radius, const UnitFilter& filter) const {
    std::vector<Unit> result;
    f32 radius_sq = radius * radius;

    i32 min_cx, min_cy, max_cx, max_cy;
    get_cell_range(center.x, center.y, radius, min_cx, min_cy, max_cx, max_cy);

    for (i32 cy = min_cy; cy <= max_cy; ++cy) {
        for (i32 cx = min_cx; cx <= max_cx; ++cx) {
            auto& cell = m_cells[cy * m_cells_x + cx];
            for (auto& unit : cell.units) {
                auto* t = world.transforms.get(unit.id);
                if (!t) continue;

                glm::vec3 diff = t->position - center;
                diff.z = 0;  // 2D distance on XY plane
                if (glm::dot(diff, diff) <= radius_sq) {
                    if (passes_filter(world, unit, filter)) {
                        result.push_back(unit);
                    }
                }
            }
        }
    }

    return result;
}

std::vector<Unit> SpatialGrid::units_in_rect(const World& world, f32 x, f32 y, f32 width, f32 height, const UnitFilter& filter) const {
    std::vector<Unit> result;

    i32 min_cx, min_cy, max_cx, max_cy;
    get_cell_range(x + width * 0.5f, y + height * 0.5f, std::max(width, height) * 0.5f,
                   min_cx, min_cy, max_cx, max_cy);

    for (i32 cy = min_cy; cy <= max_cy; ++cy) {
        for (i32 cx = min_cx; cx <= max_cx; ++cx) {
            auto& cell = m_cells[cy * m_cells_x + cx];
            for (auto& unit : cell.units) {
                auto* t = world.transforms.get(unit.id);
                if (!t) continue;

                if (t->position.x >= x && t->position.x <= x + width &&
                    t->position.y >= y && t->position.y <= y + height) {
                    if (passes_filter(world, unit, filter)) {
                        result.push_back(unit);
                    }
                }
            }
        }
    }

    return result;
}

Unit SpatialGrid::nearest_unit(const World& world, glm::vec3 center, f32 max_radius, const UnitFilter& filter) const {
    Unit best;
    f32 best_dist_sq = max_radius * max_radius;

    i32 min_cx, min_cy, max_cx, max_cy;
    get_cell_range(center.x, center.y, max_radius, min_cx, min_cy, max_cx, max_cy);

    for (i32 cy = min_cy; cy <= max_cy; ++cy) {
        for (i32 cx = min_cx; cx <= max_cx; ++cx) {
            auto& cell = m_cells[cy * m_cells_x + cx];
            for (auto& unit : cell.units) {
                auto* t = world.transforms.get(unit.id);
                if (!t) continue;

                glm::vec3 diff = t->position - center;
                diff.z = 0;
                f32 dist_sq = glm::dot(diff, diff);
                if (dist_sq < best_dist_sq) {
                    if (passes_filter(world, unit, filter)) {
                        best = unit;
                        best_dist_sq = dist_sq;
                    }
                }
            }
        }
    }

    return best;
}

} // namespace uldum::simulation
