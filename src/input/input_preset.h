#pragma once

#include "input/command_system.h"
#include "input/selection.h"
#include "input/picking.h"
#include "input/input_bindings.h"
#include "platform/platform.h"
#include "render/camera.h"
#include "simulation/simulation.h"
#include "core/types.h"

#include <memory>
#include <string_view>

namespace uldum::input {

// Everything an input preset needs to do its job.
struct InputContext {
    const platform::InputState& input;
    SelectionState&             selection;
    CommandSystem&              commands;
    Picker&                     picker;
    render::Camera&             camera;
    const InputBindings&        bindings;
    const simulation::Simulation& simulation;
    u32 screen_w;
    u32 screen_h;
    // True when the HUD has captured pointer input for this frame (hover
    // over a slot or tree widget, or a press ongoing over one). Presets
    // use this to skip pointer-initiated gameplay actions — selection
    // box drag, right-click orders — that would otherwise fight the UI.
    // Keyboard-only actions (hotkeys, camera pan) are unaffected.
    bool hud_captured = false;
    // Sub-tick interpolation factor (0..1) for the current frame — same
    // value the renderer uses. Presets that follow entity positions
    // (e.g. the Action preset's hero-tracking camera) read this so
    // they stay visually in sync with the interpolated unit render.
    f32 alpha = 1.0f;
};

// Base class for input presets. Each preset translates raw input into
// selections, commands, and camera movement.
class InputPreset {
public:
    virtual ~InputPreset() = default;
    virtual void update(const InputContext& ctx, f32 dt) = 0;

    // Ask the preset to trigger an ability as if the user had pressed
    // its hotkey. Fired from outside the input loop (HUD slot click).
    // The app orders HUD pointer dispatch before the preset update each
    // frame, so a click queued here is consumed by the same update()
    // call's trailing flush — zero extra latency. Default impl no-ops
    // for presets that don't support ability casts.
    virtual void queue_ability(std::string_view /*ability_id*/) {}

    // Id of the ability currently in "pick a target" mode, or empty
    // when the preset isn't waiting for a cast target. Used by the HUD
    // to highlight the armed slot so the player knows which ability
    // is about to fire. Default: never armed.
    virtual std::string_view targeting_ability_id() const { return {}; }
};

// Create an input preset by name ("rts", future: "action_rpg").
std::unique_ptr<InputPreset> create_preset(std::string_view name);

} // namespace uldum::input
