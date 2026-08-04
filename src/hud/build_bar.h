#pragma once

// Engine composite: build sub-panel. A worker's Build-form ability opens
// this locally (command-card build mode) — but as its OWN composite,
// NOT a takeover of action_bar. The map authors a `composites.build_bar`
// block in hud.json declaring the slot GRID (positions + style); the slot
// CONTENTS are filled by the engine from the opened ability's `builds`
// list (structure type ids), and each slot's icon comes from that unit
// type's `icon`. Clicking a slot arms build placement via build_panel_fn.
//
// Shown by open_build_panel, hidden by close_build_panel — the engine
// toggles visibility; the author never positions it relative to action_bar.

#include "core/types.h"
#include "hud/hud.h"
#include "hud/layout.h"

#include <chrono>
#include <string>
#include <vector>

namespace uldum::hud {

struct BuildBarSlotStyle {
    Color bg           = rgba(48,  52,  68,  240);
    Color hover_bg     = rgba(72,  76,  96,  250);
    Color press_bg     = rgba(102, 106, 132, 255);
    Color border_color = rgba(0,   0,   0,   128);
    f32   border_width = 1.0f;
};

// One slot in the build grid. Layout + style are authored; the structure
// it shows this open (type id + icon) is engine-filled from the ability's
// builds list, so no `command`/`icon` authoring like command_bar.
struct BuildBarSlot {
    Rect      rect{};
    Placement placement{};
    BuildBarSlotStyle style;

    bool visible = true;
    bool hovered = false;
    bool pressed = false;

    // Desktop click can flip pressed on/off in one frame; hold the visual
    // briefly so the click reads (same trick command_bar uses).
    std::chrono::steady_clock::time_point press_pulse_until{};
};

struct BuildBarStyle {
    Color bg              = rgba(0, 0, 0, 0);   // transparent container by default
    Color hotkey_color    = rgba(240, 240, 240, 255);
    Color hotkey_badge_bg = rgba(0,   0,   0,   180);
};

struct BuildBarConfig {
    bool enabled = false;
    Rect      rect{};
    Placement placement{};
    BuildBarStyle style;
    std::vector<BuildBarSlot> slots;

    // Authored dismiss button (command-card Cancel). Its position/style
    // are declared by the map in the `cancel` block, so it can sit anywhere
    // (not auto-appended after the structures — a map with scattered slots or
    // no background controls exactly where the X goes). Absent → no Cancel
    // button (desktop still has right-click / Esc / selection-change; a
    // mobile-targeted map should author one). Draws an "X" glyph; tap/click
    // closes the panel.
    bool         has_cancel = false;
    BuildBarSlot cancel;
};

// Runtime visibility. Opened/closed by open_build_panel / close_build_panel;
// starts hidden (a build ability has to open it).
struct BuildBarRuntime {
    bool visible = false;
};

} // namespace uldum::hud
