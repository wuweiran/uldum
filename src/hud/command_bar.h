#pragma once

// Engine composite: command-button grid. Complements action_bar — where
// action_bar surfaces a unit's abilities, command_bar surfaces engine-
// built-in commands (Attack, Move, Stop, Hold Position, Patrol). Maps
// opt in and pick which commands to show by declaring a
// `composites.command_bar` block in hud.json; each slot binds to one
// command id and supplies its own icon.
//
// The primary reason this exists at all is that keyboard-hotkey-only
// commands don't work on mobile. Tapping a command_bar slot issues the
// same order the preset's keyboard binding would: Stop / HoldPosition
// go out immediately, Attack / Move enter targeting mode, and the next
// world-click commits.

#include "core/types.h"
#include "hud/hud.h"
#include "hud/layout.h"

#include <string>
#include <vector>

namespace uldum::hud {

struct CommandBarSlotStyle {
    Color bg           = rgba(48,  52,  68,  240);
    Color hover_bg     = rgba(72,  76,  96,  250);
    Color press_bg     = rgba(102, 106, 132, 255);
    Color border_color = rgba(0,   0,   0,   128);
    f32   border_width = 1.0f;
};

// Per-slot binding. `command` is one of the engine command ids handled
// in RtsPreset::dispatch_command: "attack", "move", "stop",
// "hold_position", "attack_move", "patrol". Unknown ids are ignored at
// dispatch with a warning. `icon` is an asset path (KTX2), drawn like
// the action_bar icon; empty → bg-only slot.
struct CommandBarSlot {
    Rect      rect{};
    Placement placement{};
    std::string command;
    std::string icon;
    // Optional keyboard hotkey letter shown in the corner (A, M, S...).
    // The preset's existing keyboard path already handles A / S / H
    // independently of this bar — this field is display-only.
    std::string hotkey;
    CommandBarSlotStyle style;

    bool visible = true;
    bool hovered = false;
    bool pressed = false;
    bool hotkey_prev_down = false;
};

struct CommandBarStyle {
    Color bg              = rgba(0,   0,   0,   0);     // transparent by default
    Color hotkey_color    = rgba(240, 240, 240, 255);
    Color hotkey_badge_bg = rgba(0,   0,   0,   180);
    // Armed slot highlight — same idea as action_bar's armed border.
    // When the preset is in a targeting mode whose command id matches
    // a slot, the slot renders with this accent border at this width,
    // overriding the slot's normal border.
    Color armed_border_color = rgba(255, 214, 0,   255);
    f32   armed_border_width = 3.0f;
};

struct CommandBarConfig {
    bool enabled = false;
    Rect      rect{};
    Placement placement{};
    CommandBarStyle style;
    std::vector<CommandBarSlot> slots;
};

struct CommandBarRuntime {
    bool visible = true;
};

} // namespace uldum::hud
