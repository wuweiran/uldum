#pragma once

// Engine composite: ability-button slot group. One per map, singleton.
// Slot positions + styling come from the map's hud.json `composites.action_bar`
// block. Slot contents are automatic: each frame the bar reads the local
// player's selection, picks the first selected unit, and matches each of
// that unit's non-hidden abilities to a slot by hotkey letter (the slot's
// `hotkey` authored in hud.json == the ability's `hotkey` in its type
// def). No Lua binding is required for the common case.
//
// Lua control (cinematic / tutorial):
//   ActionBarSetVisible(bool)           -- hide the whole bar
//   ActionBarSetSlotVisible(slot, bool) -- hide / re-show a specific slot
//
// Each slot shows, at render time:
//   - the matched ability's icon
//   - cooldown radial overlay (when cooldown_remaining > 0, Phase C)
//   - hotkey badge (small corner label)
//   - disabled tint (unaffordable / invalid target — Phase E)
//
// Input:
//   - click → issue cast on the selected unit
//   - hotkey press → same
//
// Both routes build a GameCommand via the same path as right-click-cast.

#include "core/types.h"
#include "hud/hud.h"
#include "hud/layout.h"

#include <string>
#include <vector>

namespace uldum::hud {

struct ActionBarSlotStyle {
    Color bg            = rgba(48,  52,  68,  240);
    Color hover_bg      = rgba(72,  76,  96,  250);
    Color press_bg      = rgba(102, 106, 132, 255);
    Color disabled_bg   = rgba(30,  32,  40,  240);
    Color border_color  = rgba(0,   0,   0,   128);
    f32   border_width  = 1.0f;
};

struct ActionBarSlot {
    // Absolute screen rect (resolved from slot-relative (x,y,w,h) against
    // the bar's anchor rect). `placement` keeps the raw anchor/offset
    // values so the rect can be recomputed on viewport resize.
    Rect      rect{};
    Placement placement{};
    // Hotkey letter (e.g. "Q"). Always acts as the keyboard trigger for
    // this slot AND the badge drawn in the corner. In Ability hotkey-
    // mode it additionally matches abilities whose own `hotkey` field
    // equals this letter; in Positional mode the matching is by slot
    // index and this field is purely the keybind.
    std::string hotkey;
    ActionBarSlotStyle style;

    // Manual-binding target: ability id set by Lua via
    // `ActionBarSetSlot(slot, id)`. Ignored in Auto binding mode. When
    // Manual + the selected unit actually owns this ability, the slot
    // renders + fires that ability.
    std::string bound_ability;

    // Visibility: Lua (ActionBarSetSlotVisible) can hide an individual
    // slot for cinematics / tutorials.
    bool visible = true;

    // Transient input state (not synced). `hotkey_prev_down` is used for
    // rising-edge detection of the keyboard bind so auto-repeat doesn't
    // re-fire a cast every frame the key is held.
    bool hovered          = false;
    bool pressed          = false;
    bool hotkey_prev_down = false;
};

// Visual variants. The engine ships a small number of render prototypes;
// each map picks one via `composites.action_bar.style = "<id>"`. Each
// variant exposes its own parameter block — adding a variant is a
// localized render path + parser extension, not a generic layer engine.
enum class ActionBarStyleId : u8 {
    ClassicRts = 0,   // WC3-alike: square icon, radial cooldown, hotkey badge.
};

// How the bar routes keyboard input → ability. A player-level setting
// (`input.action_bar_hotkey_mode` in the settings store), not a per-map
// decision — lets users pick between WC3-style mnemonics and MOBA-style
// positional grids once and have it apply everywhere. Only consulted
// when `binding_mode == Auto`; manual binding uses `slot.bound_ability`
// directly and ignores this.
enum class ActionBarHotkeyMode : u8 {
    // Each ability declares its own `hotkey` letter in its type def;
    // slots match abilities by that letter. Pressing "T" triggers
    // whatever ability has hotkey="T" on the selected unit. WC3 style.
    Ability    = 0,
    // Slots bind by position. Slot 1 (hotkey="Q") always triggers the
    // selected unit's 1st non-hidden ability in registration order,
    // slot 2 the 2nd, etc. Ability def's own `hotkey` is ignored.
    // MOBA / SC2 grid style.
    Positional = 1,
};

// Where slot → ability comes from. Per-map, authored in hud.json.
// Different genres want different shapes: RTS auto-populates from the
// selected unit's ability list; action / MOBA-style maps explicitly
// assign specific abilities to specific slots via Lua.
enum class ActionBarBindingMode : u8 {
    // Resolve each frame from selection + hotkey mode above. Default,
    // matches the RTS preset's expectation.
    Auto    = 0,
    // Slot holds an `bound_ability` id set via Lua (ActionBarSetSlot).
    // The bound ability shows + fires only when the selected unit
    // actually owns it. Passive abilities can be bound but shouldn't be
    // (convention, not enforced): nothing fires when triggered.
    Manual  = 1,
};

// Parameters consumed by the classic_rts render path. Unused fields of
// future styles can either be added here (if mostly shared) or split
// into a variant-specific struct when coupling grows.
struct ActionBarStyle {
    // Bar-level background fill (transparent by default — many layouts
    // keep the container invisible and let the slot grid speak for itself).
    Color bg                  = rgba(0,   0,   0,   0);

    // Dark pie overlay that covers the still-on-cooldown fraction of the
    // slot, sweeping clockwise from 12 o'clock. Clears to zero as the
    // ability comes off cooldown.
    Color cooldown_overlay    = rgba(0,   0,   0,   192);

    // Remaining-time number centered over the cooldown overlay.
    Color cooldown_text_color = rgba(255, 255, 255, 255);
    f32   cooldown_text_size  = 20.0f;

    // Hotkey badge letter ("Q", "W"...) in the slot's top-right corner.
    Color hotkey_color        = rgba(240, 240, 240, 255);
    // Small dark pill drawn behind the letter so it stays readable on
    // bright / busy icons. Set alpha=0 in style_params to disable.
    Color hotkey_badge_bg     = rgba(0,   0,   0,   180);

    // Dim overlay for unaffordable slots AND for every non-armed slot
    // while the player is in targeting mode — the contrast is what
    // makes the armed slot legible.
    Color disabled_tint       = rgba(0,   0,   0,   128);

    // Armed slot highlight (targeting mode): overrides the slot's
    // normal border with this accent color at this width so the player
    // always sees which ability is about to commit. Thicker/brighter
    // than the normal border by default; tunable per map.
    Color armed_border_color  = rgba(255, 214, 0,   255);   // gold
    f32   armed_border_width  = 3.0f;

    // Drag-cast cancel zone (drawn only while a drag-cast is active).
    // `idle_*` is the dim-red appearance when the gesture is in
    // Pressed/Aiming; `active_*` is the bright-red appearance when the
    // finger is actually over the zone (Cancelling). The "✕" glyph in
    // the middle uses `glyph_color`.
    Color cancel_zone_idle_bg     = rgba(180, 40, 40, 140);
    Color cancel_zone_idle_border = rgba(255, 90, 90, 200);
    Color cancel_zone_active_bg   = rgba(220, 40, 40, 220);
    Color cancel_zone_active_border = rgba(255, 200, 200, 255);
    Color cancel_zone_glyph_color = rgba(255, 255, 255, 235);
};

struct ActionBarConfig {
    bool enabled = false;
    ActionBarStyleId     style_id     = ActionBarStyleId::ClassicRts;
    ActionBarBindingMode binding_mode = ActionBarBindingMode::Auto;
    // Absolute screen rect for the whole bar (for optional bg draw).
    // Resolved from `placement` against the viewport rect.
    Rect      rect{};
    Placement placement{};
    ActionBarStyle style;
    std::vector<ActionBarSlot> slots;

    // AoV-style mobile drag-cast cancel zone — a separate rect on the
    // screen that, when the player drags into it, transitions the
    // gesture to Cancelling (release = no cast). Visible only while a
    // drag-cast gesture is active. If `cancel_zone_authored` is false,
    // the loader fills in a sensible default at right-center so maps
    // get the behavior without needing to author it. The rect is
    // resolved against the viewport, not the bar rect.
    Rect      cancel_zone_rect{};
    Placement cancel_zone_placement{};
    bool      cancel_zone_authored = false;
};

// Runtime state — separate from config because it changes at runtime
// (bar visibility, hotkey mode) while config is frozen. The selected
// unit that actually drives slot contents is read live from
// WorldContext each frame, so it doesn't need storage here.
struct ActionBarRuntime {
    bool                visible      = true;
    // Sourced from the global settings store (`input.action_bar_hotkey_mode`).
    // Drives both the render-time ability resolution and the keyboard
    // dispatch. Default Ability so a map that ships ability-authored
    // hotkeys keeps working without any settings wiring.
    ActionBarHotkeyMode hotkey_mode  = ActionBarHotkeyMode::Ability;
};

} // namespace uldum::hud
