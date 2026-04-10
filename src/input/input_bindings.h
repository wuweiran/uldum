#pragma once

#include "platform/platform.h"
#include "core/types.h"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <unordered_map>

namespace uldum::input {

// Maps action ID strings (e.g., "stop") to key name strings (e.g., "S").
// Handles rising-edge detection internally.
class InputBindings {
public:
    // Check if the key bound to an action was just pressed this frame.
    bool action_pressed(const std::string& action, const platform::InputState& input) const;

    // Check if the key bound to an action is currently held.
    bool action_held(const std::string& action, const platform::InputState& input) const;

    // Load bindings from a JSON object: { "stop": "S", "hold": "H", ... }
    void load(const nlohmann::json& j);

    // Fill in missing actions from a defaults map.
    void apply_defaults(const std::unordered_map<std::string, std::string>& defaults);

    // Get the key name bound to an action, or empty string.
    const std::string& get_key(const std::string& action) const;

    // Resolve a key name string to the current value in InputState.
    // Public so ability hotkeys can check keys directly.
    static bool resolve_key(const std::string& key_name, const platform::InputState& input);

private:

    std::unordered_map<std::string, std::string> m_bindings; // action -> key name
    mutable std::unordered_map<std::string, bool> m_prev;    // action -> prev frame state
};

// RTS preset default bindings.
const std::unordered_map<std::string, std::string>& rts_default_bindings();

} // namespace uldum::input
