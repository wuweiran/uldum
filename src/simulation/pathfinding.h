#pragma once

#include "core/types.h"
#include "simulation/entity_types.h"

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
        // Scratch sized to the previous terrain's cell count — drop so
        // build_cache resizes to the new dimensions on the next search.
        m_visit_stamp.clear();
        m_visit_gen = 0;
        m_astar_nodes.clear();
        m_static_occupiable_ground.clear();
        m_components_ground.clear();
        m_components_amphibious.clear();
        m_components_water.clear();
        m_components_ground_dirty     = false;
        m_components_amphibious_dirty = false;
        m_components_water_dirty       = false;
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

    // Radius-aware position test: is a unit of `radius` (world units) centred
    // at (x,y) fully on occupiable terrain? Checks the centre cell plus a ring
    // of samples at `radius` — so the unit's whole footprint fits, not just its
    // centre. Used to keep a wide unit (ship) from parking with its hull over
    // the shore. radius ≤ 0 falls back to the plain centre-cell test.
    bool pos_clear_for_radius(f32 x, f32 y, f32 radius, MoveType move_type) const;

    // Pull `goal` back along the line toward `from` until a unit of `radius`
    // fits there (pos_clear_for_radius passes), in small world-space steps so
    // the stop point is sub-cell precise. If the goal already fits, returns it
    // unchanged. If nothing along the segment fits, returns `from`. Cheap:
    // runs once per path query, not per frame.
    glm::vec2 clamp_goal_for_radius(glm::vec2 goal, glm::vec2 from,
                                    f32 radius, MoveType move_type) const;

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

    // Find a corridor (tile path) from start to goal. Returns an empty
    // (invalid) corridor when no path exists; the caller treats that as
    // a "no path" signal.
    Corridor find_corridor(glm::vec2 start, glm::vec2 goal,
                           u8 start_cliff_level, MoveType move_type) const;

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

    // Recompute the per-cell connected-component id map for the given
    // MoveType if its dirty flag is set (e.g. after a building placement
    // or destruction). Otherwise a no-op. Called lazily from
    // find_corridor — only the MoveType we're actually pathing for ever
    // gets flooded, so maps with no Amphibious units never pay that
    // cost. Air is a no-op (returns immediately).
    void refresh_components_if_dirty(MoveType mt) const;

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

    // Per-cell static-occupiable cache for Ground (terrain passable +
    // not deep water). Doesn't include the runtime-block check — that
    // changes when buildings come and go and is checked separately.
    // Sized cells_x * cells_y, built once in build_cache.
    std::vector<u8>  m_static_occupiable_ground;
    u32 m_cells_w = 0, m_cells_h = 0;

    // A* scratch — persistent across calls to avoid per-search heap
    // churn. The "closed" set is a generation-counter array: at the
    // start of a search we bump m_visit_gen; a cell is closed when
    // m_visit_stamp[idx] == m_visit_gen. This makes "clear the closed
    // set" O(1) instead of O(cells_x * cells_y) zero-fill per call.
    // Size matches cells_x * cells_y on first use.
    mutable std::vector<u32> m_visit_stamp;
    mutable u32              m_visit_gen = 0;
    // A* node pool — reused via .clear() each search; the underlying
    // capacity sticks around.
    struct AStarNode {
        u32 tx, ty;
        f32 g_cost;
        f32 f_cost;
        u32 parent_idx;
        u8  cliff_level;
    };
    struct NodeCompare {
        bool operator()(const AStarNode& a, const AStarNode& b) const {
            return a.f_cost > b.f_cost;
        }
    };
    mutable std::vector<AStarNode> m_astar_nodes;

    // Per-cell connected-component id, one map per MoveType that needs
    // distinct connectivity (Ground vs Amphibious — Air can fly to any
    // cell so doesn't need one). 0 = unreachable cell; ids start at 1.
    // Built lazily on first find_corridor that needs the relevant
    // MoveType, refreshed when the corresponding dirty flag is set
    // (block_cells / unblock_cells flip both flags). u32 ids so we
    // never wrap on dense maze maps with many tiny components.
    //
    // The per-MoveType dirty pair (instead of a single bool) lets the
    // typical "only Ground units exist" map skip the Amphibious flood
    // forever: as long as no Amphibious request ever asks for it, the
    // flag is never read and the flood never runs.
    mutable std::vector<u32> m_components_ground;
    mutable std::vector<u32> m_components_amphibious;
    mutable std::vector<u32> m_components_water;
    mutable bool             m_components_ground_dirty     = false;
    mutable bool             m_components_amphibious_dirty = false;
    mutable bool             m_components_water_dirty       = false;
};

} // namespace uldum::simulation
