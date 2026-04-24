#include "input/input_preset.h"
#include "input/rts_preset.h"
#include "input/action_preset.h"
#include "core/log.h"

namespace uldum::input {

std::unique_ptr<InputPreset> create_preset(std::string_view name) {
    if (name == "rts" || name.empty()) {
        return std::make_unique<RtsPreset>();
    }
    if (name == "action") {
        return std::make_unique<ActionPreset>();
    }
    log::warn("Input", "Unknown input preset '{}', falling back to RTS", name);
    return std::make_unique<RtsPreset>();
}

} // namespace uldum::input
