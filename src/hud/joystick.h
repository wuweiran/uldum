#pragma once

// Engine composite: virtual analog stick for touchscreen camera pan.
// One per map, singleton. Maps opt in by declaring a
// `composites.joystick` block in hud.json. v1 is an anchored base
// circle with a knob that tracks the capturing finger, clamped to a
// maximum radius; release snaps the knob back to center. The stick
// outputs a normalized `(dx, dy)` in [-1, 1]² which the app multiplies
// by a pan speed and feeds to `camera.pan()` each frame. Desktop maps
// don't declare it — the keyboard pan covers them.
//
// v2 (not yet): re-anchor on press (base moves to the touch-down point),
// idle/active alpha states, dead-zone tuning.

#include "core/types.h"
#include "hud/hud.h"
#include "hud/layout.h"

namespace uldum::hud {

struct JoystickStyle {
    Color base_color   = rgba(20, 22, 32,  160);
    Color base_border  = rgba(0,  0,  0,   200);
    f32   base_border_width = 2.0f;
    Color knob_color   = rgba(200, 206, 224, 220);
    Color knob_border  = rgba(0,   0,   0,   200);
    f32   knob_border_width = 2.0f;
    // Knob diameter as a fraction of the base's shorter side (0..1).
    // Default 0.5 gives the knob half the base's radius, leaving the
    // other half as travel range.
    f32   knob_size_frac = 0.5f;
    // Input below this fraction of the max travel reports zero. Keeps
    // a resting thumb from drifting the camera due to sensor noise.
    f32   deadzone_frac = 0.12f;
    // Alpha multiplier applied to base + knob colors while idle (no
    // finger captured). At 1.0 idle and active look the same; at 0.0
    // the stick disappears until touched. Default 0.45 matches the
    // "faint reminder" look common in mobile games.
    f32   idle_alpha_frac = 0.45f;
};

struct JoystickConfig {
    bool enabled = false;
    // Absolute screen rect for the joystick's base HOME position. The
    // base circle is drawn inscribed in this rect when idle. On press
    // inside `activation_rect`, the base re-anchors to the press point
    // for the duration of the gesture, then returns here on release.
    Rect      rect{};
    Placement placement{};
    // Activation region — a press inside here captures the joystick
    // and re-anchors the base to the press point. Lets one large
    // region on the screen feel like "the stick" even though the
    // stick itself is a small circle. Defaults to the base rect if
    // the hud.json block omits `activation`.
    Rect      activation_rect{};
    Placement activation_placement{};
    bool      has_activation = false;
    JoystickStyle style;
};

struct JoystickRuntime {
    bool visible = true;
    // -1 when idle, or the touch slot index currently controlling the
    // knob. On desktop / mouse, slot 0 aliases the primary pointer.
    i32  captured_slot = -1;
    // Knob offset from base center, in screen pixels. Clamped to the
    // base's travel radius. Reset to (0, 0) on release.
    f32  knob_dx = 0.0f;
    f32  knob_dy = 0.0f;
    // Current base center in screen pixels. Equals the configured
    // rect's center while idle; jumps to the press point on capture;
    // returns to the home center on release.
    f32  base_cx = 0.0f;
    f32  base_cy = 0.0f;
    // Cached normalized output in [-1, 1]². Updated each frame by
    // `joystick_update`; read by `Hud::joystick_vector`.
    f32  out_x = 0.0f;
    f32  out_y = 0.0f;
};

} // namespace uldum::hud
