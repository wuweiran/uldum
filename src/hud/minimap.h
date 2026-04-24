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

} // namespace uldum::hud
