#pragma once

// Targeting style — every visual customization that drives the
// "next-click pending" UX (range ring, cast curve, AoE preview, mobile
// snap-target indicator, cursor swap, entity ping). Authored under
// the top-level `targeting` block in hud.json; consumed by the app +
// HUD each frame.
//
// The struct keeps its legacy name (CastIndicatorStyle) for now to
// avoid churn — its scope has grown beyond cast indicators, but every
// site that reads it still does so via the same getter.

#include "core/types.h"
#include "hud/hud.h"   // Color

#include <string>

namespace uldum::hud {

// Relationship-keyed color palette. Single source of truth for "what
// color signifies this kind of relationship to the player": cursor
// tints, entity-ping ring, mobile snap-target indicator, etc. Field
// names match the simulation's terminology (`is_enemy`, alliance
// "ally" flag).
struct IntentPalette {
    Color neutral = rgba(255, 255, 255, 255);
    Color enemy   = rgba(255,  48,  48, 255);
    Color ally    = rgba( 64, 220,  96, 255);
    Color item    = rgba(255, 210,  64, 255);
};

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

    // Snapped target-unit ring color (around the unit when unit-snap
    // is engaged).
    Color target_unit_color     = rgba(255, 255, 255, 180);
    f32   target_unit_thickness = 5.0f;

    // Unified aim-phase palette (hud.json `targeting.colors`). The aim
    // visuals that read "will this gesture succeed?" — the AoE shape
    // decals and the mobile snap-target pillar — are colored from these
    // three by phase, nothing else:
    //   • normal       → resting color while the cast is valid
    //   • out_of_range → drag is beyond range (a global warning)
    //   • cancelling   → drag is in the cancel zone (a global warning)
    // The two warnings are global: every phase-colored visual (incl. the
    // aim arrow) flashes them. `normal` is the shared resting tint for the
    // AoE + pillar only — the arrow keeps its own `cast_curve.color`, and
    // the range ring its own `range_ring.color` (neither is phased).
    // Engine defaults are deliberately pure primaries so an uncustomized
    // map reads as "not yet themed"; maps override with tuned values.
    Color phase_normal       = rgba(255, 255, 255, 255);  // white
    Color phase_out_of_range = rgba(  0,   0, 255, 255);  // blue
    Color phase_cancel       = rgba(255,   0,   0, 255);  // red

    // Optional texture path overrides — one per WorldOverlays slot
    // the cast indicator drives. Empty string means "use the engine's
    // procedural default for this slot". Paths resolve through the
    // AssetManager (relative paths land inside the active map's pak).
    // Map authors override the look by dropping a KTX2 into their map
    // and pointing the slot here.
    std::string range_texture;
    std::string arrow_texture;
    std::string reticle_texture;
    // AoE preview decals — one per shape. The renderer keeps three
    // distinct slots (AoeCircle / AoeLine / AoeCone) so authors can
    // give each shape its own art (e.g. radial fade for circle,
    // direction-aware gradient for cone, ribbon for line). Empty
    // string falls back to the engine's procedural default for that
    // slot. `area_texture` is the legacy circle override; cone /
    // line are independent.
    std::string area_texture;
    std::string area_cone_texture;
    std::string area_line_texture;
    std::string target_unit_texture;
    // Mobile snap-target indicator — vertical light column rooted at
    // the snapped target's feet during drag-cast. Visual is a
    // camera-yaw-aligned billboard pillar; the texture supplies the
    // entire look (alpha gradient, color glow, etc.) and the engine
    // just stretches it over the height/width given here. Color is
    // *not* configured here — it's tinted by PHASE (normal / out-of-
    // range / cancelling), the same palette the AoE indicator uses, so
    // it reads as one thing: whether the gesture will succeed. Sizes are
    // world units; base_offset shifts the column's bottom along Z
    // (default 0 = ground), positive values lift it for taller units,
    // negative sinks it slightly into the terrain for a grounded look.
    std::string snap_target_texture;
    f32 snap_target_height       = 200.0f;
    f32 snap_target_width        =  32.0f;
    f32 snap_target_base_offset  =   0.0f;
    std::string selection_texture;

    // ── Phase 4a additions ─────────────────────────────────────────
    // Intent palette — referenced by name elsewhere in the JSON
    // (e.g. an entity ping's ring color = intents.enemy when right-
    // clicking a hostile unit). Authors rarely override the default;
    // when they do, every intent-tinted visual updates uniformly.
    IntentPalette intents;

    // Cursor textures. Both default to empty — no engine-shipped
    // cursor assets. The OS cursor is the fallback in every state;
    // a HUD-drawn cursor only appears when the map authors one
    // through `targeting.cursors.{default,target}` in hud.json.
    // `cursor_size` is in dp (HUD logical units), scaling 1:1 with
    // OS DPI.
    std::string cursor_default_path;   // "" → keep OS cursor in idle
    std::string cursor_target_path;    // "" → keep OS cursor in targeting
    f32         cursor_size         = 20.0f;

    // Entity ping (post-commit ring on the targeted unit / item).
    // The ring's color is *always* the runtime intent (enemy / ally /
    // item) for the click that fired it — no JSON `color` knob.
    // Authors customize the texture and the animation envelope only.
    std::string entity_ping_texture;             // empty → SelectionRing fallback
    f32         entity_ping_thickness_start = 8.0f;
    f32         entity_ping_thickness_end   = 4.0f;
    f32         entity_ping_lifespan        = 0.45f;
};

struct CastIndicatorConfig {
    bool enabled = true;     // can be turned off via hud.json (rare)
    CastIndicatorStyle style;
};

} // namespace uldum::hud
