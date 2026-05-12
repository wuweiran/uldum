#pragma once

#include "core/types.h"
#include "simulation/handle_types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <span>
#include <vector>

namespace uldum::map { struct TerrainData; }

namespace uldum::simulation {

struct World;

enum class MoveType : u8;

// Pathing-cell subdivision: each terrain tile splits into SUBDIV×SUBDIV
// pathing cells (WC3 convention: 4 → 16 cells per tile, 32-unit cells over
// 128-unit tiles). Static obstacles still snap to tile boundaries for now,
// but A* and runtime blocking work at cell granularity so dynamic / sub-tile
// obstacles can land natively.
static constexpr u32 PATHING_SUBDIV = 4;

// A corridor: ordered list of pathing-cell coordinates from A* (source to
// destination). Values are CELL coords (tile_x * SUBDIV + sub_x).
struct Corridor {
    std::vector<glm::ivec2> cells;
    bool valid = false;
};

// Cell-based pathfinding. A* on a SUBDIV-refined grid with occupancy +
// connectivity. Terrain properties (cliff levels, water, passability) are
// stored per terrain tile and inherited by every cell within the tile;
// runtime blocks (buildings, destructables) are per cell so partial-tile
// footprints work natively.
//
// Z coordinates are NEVER used for pathfinding. Z is for visual/height only.
class Pathfinder {
public:
    void set_terrain(const map::TerrainData* terrain) {
        m_terrain = terrain;
        // Old runtime blocks (buildings on the previous map / scene)
        // index into the previous terrain's pathing grid. Drop them so a
        // same-size new terrain doesn't inherit stale blocks.
        m_runtime_blocked_cells.clear();
        build_cache();
    }
    const map::TerrainData* terrain() const { return m_terrain; }

    // ── Tile queries (delegate to TerrainData + MoveType logic) ─────────

    // Can a unit of the given move type occupy this tile?
    // Combines TerrainData::is_tile_passable() with MoveType (air/ground/amphibious).
    // Includes the runtime-block check (returns false if ANY cell in the
    // tile is currently blocked).
    bool can_occupy(u32 tx, u32 ty, MoveType move_type) const;

    // Can a unit on tile (src_tx, src_ty) move to adjacent tile (dst_tx, dst_ty)?
    bool are_connected(u32 src_tx, u32 src_ty, u32 dst_tx, u32 dst_ty,
                       MoveType move_type) const;

    // What cliff level should a unit have after entering this tile?
    u8 cliff_level_on_tile(u32 tx, u32 ty) const;

    // ── Cell queries (the granularity A* actually expands on) ────────────

    // Can a unit occupy this cell? Containing tile must be terrain-passable
    // and the cell itself must not be runtime-blocked.
    bool can_occupy_cell(i32 cx, i32 cy, MoveType move_type) const;

    // Is this cell runtime-blocked (any blocker rect overlaps it)?
    bool is_cell_blocked(i32 cx, i32 cy) const;

    // ── Convenience wrappers (delegate to TerrainData) ───────────────────

    glm::ivec2 world_to_tile(f32 x, f32 y) const;
    glm::ivec2 world_to_cell(f32 x, f32 y) const;
    glm::vec2  tile_center(u32 tx, u32 ty) const;
    glm::vec2  cell_center(i32 cx, i32 cy) const;
    u8         cliff_level_at(f32 x, f32 y) const;
    f32        tile_size() const;
    f32        cell_size() const;   // = tile_size / PATHING_SUBDIV

    // ── Movement validation ─────────────────────────────────────────────

    // Can a unit at (old_x, old_y) move to (new_x, new_y)?
    // Convenience wrapper over are_connected for runtime movement checks.
    bool can_move_to(f32 old_x, f32 old_y, f32 new_x, f32 new_y,
                     MoveType move_type) const;

    // ── Pathfinding ───────────────────────────────────────────────────────

    // Find a corridor (tile path) from start to goal.
    // If world and self_id are provided, tiles occupied by other units incur extra cost.
    Corridor find_corridor(glm::vec2 start, glm::vec2 goal,
                           u8 start_cliff_level, MoveType move_type,
                           const World* world = nullptr, u32 self_id = UINT32_MAX) const;

    // Given a corridor's cells, find the farthest point reachable in a
    // straight line from the given position without leaving the corridor.
    // Returns the waypoint. collision_radius: swept-circle radius for the
    // line test (0 = point sample).
    glm::vec2 find_straight_waypoint(glm::vec2 from, std::span<const glm::ivec2> corridor_cells,
                                      f32 collision_radius = 0,
                                      u8 cliff_level = 0,
                                      MoveType move_type = MoveType::Ground) const;

    // ── Recovery ──────────────────────────────────────────────────────────

    // Find the nearest occupiable tile center for a unit that's on an invalid tile.
    glm::vec2 find_nearest_valid(f32 x, f32 y, MoveType move_type) const;

    // ── Runtime pathing blocks (buildings) ────────────────────────────────

    // Block/unblock a rectangle of pathing cells. Cell-grain is the
    // engine's native unit for runtime obstacles: trees and small
    // destructables block partial-tile footprints, while large objects
    // (buildings) compose by multiplying tile extents by PATHING_SUBDIV.
    // Per-cell refcount so overlapping blockers compose correctly.
    void block_cells  (i32 cx, i32 cy, u32 w, u32 h);
    void unblock_cells(i32 cx, i32 cy, u32 w, u32 h);

    // Tile-rect convenience: take TILE coords and TILE extents, expand
    // each tile into SUBDIV×SUBDIV cells, then call block_cells. Buildings
    // still author footprints in tiles; this keeps that authoring path.
    void block_tiles  (i32 tx, i32 ty, u32 w, u32 h);
    void unblock_tiles(i32 tx, i32 ty, u32 w, u32 h);

    // Is the entire tile flagged blocked? Returns true if ANY cell within
    // the tile is blocked (matches the "tile is unwalkable" semantics
    // callers expect, even though the underlying grid is per-cell).
    bool is_tile_blocked(u32 tx, u32 ty) const;

    // Build connectivity cache from terrain data. Call after set_terrain().
    void build_cache();

private:
    const map::TerrainData* m_terrain = nullptr;
    // Per-cell refcount of runtime blockers (buildings, etc.). Sized to
    // (tiles_x * SUBDIV) * (tiles_y * SUBDIV) on first use; cleared on
    // set_terrain (terrain swap means old blocks are stale).
    std::vector<u8> m_runtime_blocked_cells;

    // Pre-computed terrain cache (built once at terrain load)
    std::vector<bool> m_occupiable;       // can_occupy(tx, ty, Ground) per tile
    std::vector<i8>   m_effective_level;  // tile_effective_level per tile
    std::vector<u8>   m_cliff_level;      // cliff_level_on_tile per tile
    std::vector<u8>   m_connectivity;     // 8 bits: which directions are connected (Ground)
    u32 m_cache_w = 0, m_cache_h = 0;
};

} // namespace uldum::simulation
