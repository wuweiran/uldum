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
    // Current camera view — WC3-style white rectangle marking the ground the
    // main view currently covers. Clipped to the map area (edges past the map
    // don't draw), not clamped.
    Color camera_frame_color = rgba(255, 255, 255, 220);
    f32   camera_frame_width = 1.5f;                     // edge thickness in pixels
    // Map-bound outline — the map's actual extent inside the panel. Only
    // visible when the map isn't the panel's aspect ratio (letterboxed); for
    // a matching map it coincides with the panel border and is skipped.
    Color map_bound_color = rgba(120, 130, 150, 200);
    f32   map_bound_width = 1.0f;
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
//
// The map is fit into the panel PRESERVING ASPECT RATIO (letterboxed), so a
// non-square map keeps its shape instead of stretching to fill a square panel.
// Both transforms route through `minimap_content_rect` — the centered sub-rect
// the map actually occupies. For a map whose aspect matches the panel this
// equals the panel rect (no inset).

// The centered, aspect-preserving sub-rect of `mm` that the map occupies.
// Letterbox (map wider than panel) or pillarbox (map taller). Returns `mm`
// unchanged when the terrain has zero extent.
inline Rect minimap_content_rect(const Rect& mm, const map::TerrainData& td) {
    const f32 mw = td.world_width(), mh = td.world_height();
    if (mw <= 0.0f || mh <= 0.0f) return mm;
    const f32 map_aspect   = mw / mh;
    const f32 panel_aspect = mm.w / mm.h;
    Rect c = mm;
    if (map_aspect > panel_aspect) {
        // Map is wider — fit to width, letterbox top/bottom.
        c.h = mm.w / map_aspect;
        c.y = mm.y + (mm.h - c.h) * 0.5f;
    } else {
        // Map is taller — fit to height, pillarbox left/right.
        c.w = mm.h * map_aspect;
        c.x = mm.x + (mm.w - c.w) * 0.5f;
    }
    return c;
}

// World ground-plane (wx, wy) → minimap screen point. Returns the panel
// origin when the terrain has zero extent (matches the renderer's old
// inv_w/inv_h == 0 guard, which collapsed every dot onto rect.x/y).
inline void minimap_world_to_screen(const Rect& mm, const map::TerrainData& td,
                                    f32 wx, f32 wy, f32& sx, f32& sy) {
    const Rect c = minimap_content_rect(mm, td);
    const f32 inv_w = (td.world_width()  > 0.0f) ? (c.w / td.world_width())  : 0.0f;
    const f32 inv_h = (td.world_height() > 0.0f) ? (c.h / td.world_height()) : 0.0f;
    sx = c.x + (wx - td.origin_x()) * inv_w;
    // Flip Y: north (max world Y) at the top of the panel.
    sy = c.y + c.h - (wy - td.origin_y()) * inv_h;
}

// Minimap screen point → world ground-plane (wx, wy). Inverse of the above.
// A click in the letterbox margin (outside the content rect) clamps to the
// nearest map edge so it still resolves to a valid on-map point.
inline void minimap_screen_to_world(const Rect& mm, const map::TerrainData& td,
                                    f32 sx, f32 sy, f32& wx, f32& wy) {
    const Rect c = minimap_content_rect(mm, td);
    f32 fx = (c.w > 0.0f) ? (sx - c.x) / c.w : 0.0f;   // 0..1 across content
    f32 fy = (c.h > 0.0f) ? (sy - c.y) / c.h : 0.0f;
    fx = (fx < 0.0f) ? 0.0f : (fx > 1.0f ? 1.0f : fx);
    fy = (fy < 0.0f) ? 0.0f : (fy > 1.0f ? 1.0f : fy);
    wx = td.origin_x() + fx * td.world_width();
    // Flip Y to match minimap_world_to_screen (north at top).
    wy = td.origin_y() + (1.0f - fy) * td.world_height();
}

} // namespace uldum::hud
