#pragma once

#include "simulation/entity_types.h"

#include <glm/vec3.hpp>

#include <string_view>
#include <vector>

namespace uldum::map { struct TerrainData; }

namespace uldum::simulation {

class Simulation;
class Pathfinder;
struct World;

// Result of testing a building type at a cursor position: the
// footprint-snapped world position, whether placement is legal there,
// and the footprint extent in tiles (0×0 = the type has no footprint).
// `tile_ok` is a row-major fw×fh mask (index = ty*fw + tx) of per-tile
// buildability, so the placement preview can tint each tile red/green
// individually instead of all-or-nothing. `valid` is the AND of every
// tile plus the collision-overlap test.
struct BuildingPlacement {
    glm::vec3          snapped{0.0f};
    bool               valid = false;
    u32                fw = 0;
    u32                fh = 0;
    std::vector<u8>    tile_ok;   // fw*fh; 1 = buildable, 0 = blocked
};

// Is a single TILE buildable by a structure — flat (not a ramp/cliff
// transition), passable, not deep water, and not occupied by a runtime
// blocker. Stricter than can_occupy_cell (which lets walkers cross ramps):
// buildings require FLAT ground.
bool tile_buildable(const Pathfinder& pf, const map::TerrainData& td,
                    i32 tx, i32 ty, MoveType move_type);

// Whole-footprint buildability for a structure at a snapped center (wx,wy).
// Every TILE the fw×fh footprint covers must satisfy tile_buildable (flat,
// passable, unblocked). The authoritative host-side gate; the UI preview
// uses the per-tile mask in evaluate_building_placement for the same rule.
bool footprint_buildable(const Pathfinder& pf, const map::TerrainData& td,
                         f32 wx, f32 wy, u32 fw, u32 fh, MoveType move_type);

// Footprint clearance against the pathing grid. fw/fh are TILE units when
// in_cells=false, CELL units when in_cells=true; the tile path expands by
// PATHING_SUBDIV. Every cell the footprint covers must be occupiable by
// move_type. Shared by the editor placement preview and in-game build.
bool footprint_clear(const Pathfinder& pf, const map::TerrainData& td,
                     f32 wx, f32 wy, u32 fw, u32 fh, bool in_cells,
                     MoveType move_type);

// True if a collision circle at (wx,wy) with `radius` overlaps any
// same-layer entity in `world` (air vs surface never overlap). `ignore_id`
// skips a preview/self entity; 0 = ignore nothing.
bool collision_overlaps(const World& world, f32 wx, f32 wy, f32 radius,
                        MoveType move_type, u32 ignore_id);

// A footprint occupant that can be pushed aside (vs. a hard blocker): owned by
// `builder_owner_id`, movable (not MoveType::None, not a building), and idle
// (no current order, not in combat, not casting). Shared policy used by both
// the placement preview and the arrival-time displace/abandon logic.
bool is_displaceable(const World& world, u32 unit_id, u32 builder_owner_id);

// Ids of units whose collision disc overlaps the fw×fh tile-footprint centered
// at (wx,wy), same movement layer as `move_type`, excluding `ignore_id`.
std::vector<u32> footprint_occupants(const World& world, const map::TerrainData& td,
                                     f32 wx, f32 wy, u32 fw, u32 fh,
                                     MoveType move_type, u32 ignore_id = 0);

// Evaluate a building type at a cursor world position: snap to the
// footprint grid, test footprint clearance + collision overlap, and
// sample terrain height. `ignore_id` skips a preview entity in the
// overlap sweep. Types with no footprint fall back to a collision-radius
// occupiability test, matching the editor's can_place_at. `owner_id` is the
// placing player: a footprint tile under a *displaceable* own unit stays valid
// (it'll be pushed off on build); UINT32_MAX = treat every occupant as a blocker.
BuildingPlacement evaluate_building_placement(const Simulation& sim,
                                              std::string_view type_id,
                                              f32 cursor_x, f32 cursor_y,
                                              u32 ignore_id = 0,
                                              u32 owner_id = UINT32_MAX);

} // namespace uldum::simulation
