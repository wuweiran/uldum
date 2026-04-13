#pragma once

#include "core/types.h"
#include "simulation/handle_types.h"

#include <vector>

namespace uldum::map { struct TerrainData; }

namespace uldum::simulation {

struct World;
class Simulation;

// Fog of war visibility states (per tile, per player).
enum class Visibility : u8 {
    Unexplored = 0,   // never seen — black
    Explored   = 1,   // previously seen — dim, no enemy units shown
    Visible    = 2,   // currently in vision — full brightness
};

// Configurable fog of war mode (set from map manifest).
// Matches WC3 World Editor options.
enum class FogMode : u8 {
    None       = 0,   // no fog — everything visible to all players
    Explored   = 1,   // map starts pre-explored (terrain visible, dimmed) — enemies hidden outside vision
    Unexplored = 2,   // full fog — black areas must be discovered, then dim when out of vision
};

// Tile-based, per-player fog of war.
//
// Each player has a grid of tiles_x * tiles_y visibility values.
// Updated each simulation tick from unit Vision components.
//
// Visual smoothing: a separate float grid is lerped per frame toward
// the target brightness, producing smooth reveal/fade transitions.
class FogOfWar {
public:
    void init(u32 tiles_x, u32 tiles_y, f32 tile_size, u32 player_count, FogMode mode,
              const map::TerrainData* terrain = nullptr);

    // Run one fog update. Call once per simulation tick.
    void update(const World& world, const Simulation& sim);

    // Advance visual interpolation for the given player. Call once per render frame.
    // dt = frame delta time. Returns the visual grid (tiles_x * tiles_y floats, 0..1).
    const f32* update_visual(Player player, f32 dt);

    // ── Queries ────────────────────────────────────────────────────────
    Visibility get(Player player, u32 tx, u32 ty) const;
    bool is_visible(Player player, u32 tx, u32 ty) const;
    bool is_explored(Player player, u32 tx, u32 ty) const;

    // Raw logical grid for the given player (tiles_x * tiles_y u8 values).
    // Returns nullptr if fog is disabled.
    const u8* grid(Player player) const;

    // ── Script API ─────────────────────────────────────────────────────
    void reveal_all(Player player);
    void unexplore_all(Player player);

    // ── Accessors ──────────────────────────────────────────────────────
    FogMode mode() const { return m_mode; }
    bool enabled() const { return m_mode != FogMode::None; }
    u32 tiles_x() const { return m_tiles_x; }
    u32 tiles_y() const { return m_tiles_y; }
    u32 player_count() const { return m_player_count; }

private:
    // Index into per-player grids
    u32 index(u32 player_id, u32 tx, u32 ty) const {
        return player_id * (m_tiles_x * m_tiles_y) + ty * m_tiles_x + tx;
    }

    // Mark tiles in a circle around (cx, cy) with feathered edges.
    void mark_vision_circle(u32 player_id, f32 cx, f32 cy, f32 radius_tiles, u8 viewer_cliff);

    // Check line of sight between two tiles. Returns false if a cliff wall blocks the view.
    bool has_cliff_los(u32 x0, u32 y0, u32 x1, u32 y1, u8 viewer_cliff) const;

    FogMode m_mode = FogMode::None;
    u32 m_tiles_x = 0;
    u32 m_tiles_y = 0;
    f32 m_tile_size = 128.0f;
    u32 m_player_count = 0;
    const map::TerrainData* m_terrain = nullptr;

    // Logical state: player_count * tiles_x * tiles_y, stores Visibility as u8.
    std::vector<u8> m_grids;

    // Target brightness per tile (0.0 = unexplored, 0.4 = explored, 1.0 = visible).
    // Includes feathered edges — tiles at vision boundary get partial values.
    std::vector<f32> m_targets;

    // Visual brightness per tile, lerped toward target each frame.
    std::vector<f32> m_visual;
};

} // namespace uldum::simulation
