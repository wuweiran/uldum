#pragma once

#include "input/command_system.h"
#include "input/selection.h"
#include "input/picking.h"
#include "platform/platform.h"
#include "render/camera.h"
#include "simulation/simulation.h"
#include "core/types.h"

namespace uldum::input {

// Everything an input preset needs to do its job.
struct InputContext {
    const platform::InputState& input;
    SelectionState&             selection;
    CommandSystem&              commands;
    Picker&                     picker;
    render::Camera&             camera;
    const simulation::Simulation& simulation;
    u32 screen_w;
    u32 screen_h;
};

// Base class for input presets. Each preset translates raw input into
// selections, commands, and camera movement.
class InputPreset {
public:
    virtual ~InputPreset() = default;
    virtual void update(const InputContext& ctx, f32 dt) = 0;
};

} // namespace uldum::input
