#pragma once

// Shared anchor / placement helpers used by both the hud.json loader
// and the runtime resize path. Composites (action_bar, minimap, etc.)
// store a Placement next to their resolved absolute rect so the rect
// can be recomputed when the viewport changes size without re-parsing
// JSON.

#include "core/types.h"
#include "hud/hud.h"

#include <string_view>

namespace uldum::hud {

struct AnchorFrac { f32 h; f32 v; };

// 9-point anchor string → (horizontal_fraction, vertical_fraction).
// Format: two chars where the first is vertical (t/m/b) and second is
// horizontal (l/c/r). Anything else defaults to top-left.
inline AnchorFrac parse_anchor(std::string_view s) {
    if (s.size() != 2) return { 0.0f, 0.0f };
    f32 v = (s[0] == 'm') ? 0.5f : (s[0] == 'b') ? 1.0f : 0.0f;
    f32 h = (s[1] == 'c') ? 0.5f : (s[1] == 'r') ? 1.0f : 0.0f;
    return { h, v };
}

// Resolve (parent, anchor, offset, size) → absolute rect. The anchor
// picks a point on the parent AND a matching corner on the child; the
// child is placed so its anchor corner sits at the parent's anchor
// point plus (x, y).
inline Rect resolve_rect(const Rect& parent, AnchorFrac a,
                         f32 x, f32 y, f32 w, f32 h) {
    Rect r{};
    r.x = parent.x + parent.w * a.h - w * a.h + x;
    r.y = parent.y + parent.h * a.v - h * a.v + y;
    r.w = w;
    r.h = h;
    return r;
}

// Stored raw layout for re-resolving on resize. `anchor` + (x, y, w, h)
// reproduce the absolute rect against whichever parent rect is in use
// at resize time (viewport for bar-level, bar rect for slot-level).
struct Placement {
    AnchorFrac anchor{ 0.0f, 0.0f };
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 w = 0.0f;
    f32 h = 0.0f;
};

inline Rect resolve(const Rect& parent, const Placement& p) {
    return resolve_rect(parent, p.anchor, p.x, p.y, p.w, p.h);
}

} // namespace uldum::hud
