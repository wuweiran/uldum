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
    // Mobile snap-target indicator — vertical light column over the
    // snapped target during drag-cast. Visual is a camera-yaw-aligned
    // billboard pillar; the texture supplies the entire look (alpha
    // gradient, color glow, etc.) and the engine just stretches it
    // over the height/width given here. Color is *not* configured —
    // the intent palette tints the column ally / enemy / neutral so
    // it reads at a glance which relationship the snap holds. Sizes
    // are world units; base_offset lifts the column above the unit's
    // feet so the solid end sits well above head height.
    std::string snap_target_texture;
    f32 snap_target_height       = 200.0f;
    f32 snap_target_width        =  32.0f;
    f32 snap_target_base_offset  =  96.0f;
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
