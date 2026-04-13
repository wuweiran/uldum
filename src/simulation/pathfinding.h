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

// A corridor: ordered list of tile coordinates from A* (source to destination).
struct Corridor {
    std::vector<glm::ivec2> tiles;  // (tx, ty) in tile coordinates
    bool valid = false;
};

// Tile-based pathfinding. A* on tile graph with occupancy + connectivity.
//
// Key concepts:
// - Occupancy: can a unit exist on a tile? (depends on tile type and unit move type)
// - Connectivity: can a unit move from tile A to tile B? (depends on cliff levels, ramps, shared edge)
// - Corridor: the A* result — an ordered sequence of connected tiles
// - Straight-line: longest line through corridor that doesn't exit it (the actual waypoint)
//
// Z coordinates are NEVER used for pathfinding. Z is for visual/height only.
class Pathfinder {
public:
    void set_terrain(const map::TerrainData* terrain) { m_terrain = terrain; }
    const map::TerrainData* terrain() const { return m_terrain; }

    // ── Tile queries (delegate to TerrainData + MoveType logic) ─────────

    // Can a unit of the given move type occupy this tile?
    // Combines TerrainData::is_tile_passable() with MoveType (air/ground/amphibious).
    bool can_occupy(u32 tx, u32 ty, MoveType move_type) const;

    // Can a unit on tile (src_tx, src_ty) move to adjacent tile (dst_tx, dst_ty)?
    bool are_connected(u32 src_tx, u32 src_ty, u32 dst_tx, u32 dst_ty,
                       MoveType move_type) const;

    // What cliff level should a unit have after entering this tile?
    u8 cliff_level_on_tile(u32 tx, u32 ty) const;

    // ── Convenience wrappers (delegate to TerrainData) ───────────────────

    glm::ivec2 world_to_tile(f32 x, f32 y) const;
    glm::vec2 tile_center(u32 tx, u32 ty) const;
    u8 cliff_level_at(f32 x, f32 y) const;
    f32 tile_size() const;

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

    // Given corridor tiles, find the farthest point reachable in a straight line from
    // the given position without leaving the corridor. Returns the waypoint.
    // collision_radius: swept circle radius for the line test (0 = point).
    glm::vec2 find_straight_waypoint(glm::vec2 from, std::span<const glm::ivec2> corridor_tiles,
                                      f32 collision_radius = 0,
                                      u8 cliff_level = 0,
                                      MoveType move_type = MoveType::Ground) const;

    // ── Recovery ──────────────────────────────────────────────────────────

    // Find the nearest occupiable tile center for a unit that's on an invalid tile.
    glm::vec2 find_nearest_valid(f32 x, f32 y, MoveType move_type) const;

    // ── Runtime pathing blocks (buildings) ────────────────────────────────

    // Block/unblock vertices at runtime (buildings, destructables).
    // These are NOT saved to terrain — they exist only while the blocker lives.
    void block_vertices(const std::vector<glm::ivec2>& verts);
    void unblock_vertices(const std::vector<glm::ivec2>& verts);
    bool is_vertex_blocked(u32 vx, u32 vy) const;

private:
    const map::TerrainData* m_terrain = nullptr;
    std::vector<u8> m_runtime_blocked;  // per-vertex refcount (multiple buildings can overlap)
};

} // namespace uldum::simulation
