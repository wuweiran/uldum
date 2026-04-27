#pragma once

// Cast / drag-cast indicator style. Authored once per map under
// `composites.cast_indicator` in hud.json; consumed by the app each
// frame when populating AbilityIndicators from Hud::aim_state().
//
// One config covers both platforms — desktop targeting mode and mobile
// drag-cast feed the same indicator state, and the user-facing visual
// shouldn't differ between them.

#include "core/types.h"
#include "hud/hud.h"   // Color

#include <string>

namespace uldum::hud {

struct CastIndicatorStyle {
    // Range ring at the caster — radius = ability.range. Color is
    // applied as-is (no per-phase tint), since the ring's purpose is
    // "this is where you can reach", independent of the live aim
    // state.
    Color range_color     = rgba(255, 255, 255, 80);
    f32   range_thickness = 4.0f;

    // 3D arc from caster ground to drag/target point. Curve sags
    // upward in the middle by `arc_height`; the near-caster end
    // fades to transparent so the line doesn't visually cross the
    // unit body. `head_height` is reserved for callers that want
    // to start the arc above the unit (currently unused; keep for
    // forward-compat with future per-unit-type head offsets).
    Color arrow_color     = rgba(255, 255, 255, 200);
    f32   arrow_thickness = 6.0f;
    f32   head_height     = 0.0f;
    f32   arc_height      = 100.0f;

    // Reticle (filled circle with radial-alpha falloff). Drawn at the
    // drag point when the ability is unsnapped point-target / unit-
    // target, replaced by the snapped-unit ring otherwise.
    Color reticle_color  = rgba(180, 220, 255, 220);
    f32   reticle_radius = 18.0f;

    // AoE indicator (ring + tick pattern decal for shape=area). For
    // target_unit + has_area, anchored at the snapped unit. Default
    // is a soft white — neutral for any spell. Authors override per
    // ability or per map via `area_color` in hud.json.
    Color area_color = rgba(230, 235, 255, 220);

    // Snapped target-unit ring color (around the unit when unit-snap
    // is engaged).
    Color target_unit_color     = rgba(255, 255, 255, 180);
    f32   target_unit_thickness = 5.0f;

    // Per-phase tint applied to "active" indicators (everything except
    // the range ring). Multiplies the base color's RGB component-wise
    // and replaces the alpha. Phase Normal is the identity (treated as
    // white-1.0); OutOfRange recolors to a cool blue, Cancelling to a
    // warm red.
    Color out_of_range_tint = rgba(108, 168, 255, 200);
    Color cancel_tint       = rgba(255, 80,  80, 230);

    // Optional texture path overrides — one per WorldOverlays slot
    // the cast indicator drives. Empty string means "use the engine's
    // procedural default for this slot". Paths resolve through the
    // AssetManager (relative paths land inside the active map's pak).
    // Map authors override the look by dropping a KTX2 into their map
    // and pointing the slot here.
    std::string range_texture;
    std::string arrow_texture;
    std::string reticle_texture;
    std::string area_texture;
    std::string target_unit_texture;
    std::string selection_texture;
};

struct CastIndicatorConfig {
    bool enabled = true;     // can be turned off via hud.json (rare)
    CastIndicatorStyle style;
};

} // namespace uldum::hud
