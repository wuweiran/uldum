#pragma once

// Engine composite: top-down schematic of the world. One per map, singleton.
// Reads terrain bounds, unit positions, fog, and the local selection from
// `WorldContext` at render time — no stored state beyond layout + style.
//
// v1 (this file): bg rect, border, player-colored unit dots (fog-filtered),
// click-to-jump camera. No terrain thumbnail, no fog overlay, no camera-
// viewport outline, no drag-pan. Those land as v2 once the skeleton is
// confirmed.

#include "core/types.h"
#include "hud/hud.h"
#include "hud/layout.h"
#include "map/terrain_data.h"

namespace uldum::hud {

enum class MinimapStyleId : u8 {
    ClassicRts = 0,
};

struct MinimapStyle {
    Color bg              = rgba(12,  14,  20,  230);
    Color border_color    = rgba(0,   0,   0,   200);
    f32   border_width    = 1.0f;
    // Unit-dot colors — used by the owner → color resolver.
    Color own_dot_color   = rgba(80,  220, 90,  255);   // green: local player
    Color ally_dot_color  = rgba(80,  180, 255, 255);   // cyan: allied (non-own)
    Color enemy_dot_color = rgba(220, 70,  70,  255);   // red
    Color neutral_dot_color = rgba(200, 200, 100, 255); // yellow
    f32   dot_size        = 3.0f;                        // square side in pixels
};

struct MinimapConfig {
    bool enabled = false;
    MinimapStyleId style_id = MinimapStyleId::ClassicRts;
    // Absolute screen rect for the minimap panel. Resolved from
    // `placement` against the viewport rect so it can re-anchor on
    // window resize.
    Rect      rect{};
    Placement placement{};
    MinimapStyle style;
};

struct MinimapRuntime {
    bool visible = true;
};

// ── Minimap ↔ world coordinate transforms ────────────────────────────────
// Forward (world → minimap screen) and inverse (screen → world) are exact
// inverses and MUST share the same Y-flip convention: the minimap puts
// north (max world Y) at the top, so screen Y runs opposite world Y. They
// used to live apart — forward inlined in the HUD renderer's dot loop,
// inverse in hud.cpp's click handler — which let the flip drift. Keep the
// pair here so a change to one is made staring at the other.

// World ground-plane (wx, wy) → minimap screen point. Returns the panel
// origin when the terrain has zero extent (matches the renderer's old
// inv_w/inv_h == 0 guard, which collapsed every dot onto rect.x/y).
inline void minimap_world_to_screen(const Rect& mm, const map::TerrainData& td,
                                    f32 wx, f32 wy, f32& sx, f32& sy) {
    const f32 inv_w = (td.world_width()  > 0.0f) ? (mm.w / td.world_width())  : 0.0f;
    const f32 inv_h = (td.world_height() > 0.0f) ? (mm.h / td.world_height()) : 0.0f;
    sx = mm.x + (wx - td.origin_x()) * inv_w;
    // Flip Y: north (max world Y) at the top of the panel.
    sy = mm.y + mm.h - (wy - td.origin_y()) * inv_h;
}

// Minimap screen point → world ground-plane (wx, wy). Inverse of the above.
inline void minimap_screen_to_world(const Rect& mm, const map::TerrainData& td,
                                    f32 sx, f32 sy, f32& wx, f32& wy) {
    const f32 fx = (sx - mm.x) / mm.w;   // 0..1 across minimap
    const f32 fy = (sy - mm.y) / mm.h;
    wx = td.origin_x() + fx * td.world_width();
    // Flip Y to match minimap_world_to_screen (north at top).
    wy = td.origin_y() + (1.0f - fy) * td.world_height();
}

} // namespace uldum::hud
