#include "input/input_bindings.h"

#include <nlohmann/json.hpp>

namespace uldum::input {

// ── Key name resolution ──────────────────────────────────────────────────
// Maps key name strings to InputState member pointers.

using KeyPtr = bool platform::InputState::*;

static const std::unordered_map<std::string, KeyPtr>& key_table() {
    static const std::unordered_map<std::string, KeyPtr> table = {
        {"Escape", &platform::InputState::key_escape},
        {"W",      &platform::InputState::key_w},
        {"A",      &platform::InputState::key_a},
        {"S",      &platform::InputState::key_s},
        {"D",      &platform::InputState::key_d},
        {"Q",      &platform::InputState::key_q},
        {"E",      &platform::InputState::key_e},
        {"H",      &platform::InputState::key_h},
        {"P",      &platform::InputState::key_p},
        {"F1",     &platform::InputState::key_f1},
        {"F2",     &platform::InputState::key_f2},
        {"F3",     &platform::InputState::key_f3},
        {"Up",     &platform::InputState::key_up},
        {"Down",   &platform::InputState::key_down},
        {"Left",   &platform::InputState::key_left},
        {"Right",  &platform::InputState::key_right},
        {"Shift",  &platform::InputState::key_shift},
        {"Ctrl",   &platform::InputState::key_ctrl},
        {"Alt",    &platform::InputState::key_alt},
    };
    return table;
}

bool InputBindings::resolve_key(const std::string& key_name, const platform::InputState& input) {
    // Check named keys (multi-char: Escape, F1, Shift, etc.)
    auto& table = key_table();
    auto it = table.find(key_name);
    if (it != table.end()) {
        return input.*(it->second);
    }

    if (key_name.size() == 1) {
        char c = key_name[0];
        // Number keys "0"-"9"
        if (c >= '0' && c <= '9') return input.key_num[c - '0'];
        // Letter keys "A"-"Z" (uppercase)
        if (c >= 'A' && c <= 'Z') return input.key_letter[c - 'A'];
        // Letter keys "a"-"z" (lowercase, treat same)
        if (c >= 'a' && c <= 'z') return input.key_letter[c - 'a'];
    }

    return false;
}

// ── InputBindings ────────────────────────────────────────────────────────

bool InputBindings::action_pressed(const std::string& action, const platform::InputState& input) const {
    auto it = m_bindings.find(action);
    if (it == m_bindings.end()) return false;

    bool current = resolve_key(it->second, input);
    bool& prev = m_prev[action];
    bool pressed = current && !prev;
    prev = current;
    return pressed;
}

bool InputBindings::action_held(const std::string& action, const platform::InputState& input) const {
    auto it = m_bindings.find(action);
    if (it == m_bindings.end()) return false;
    return resolve_key(it->second, input);
}

void InputBindings::load(const nlohmann::json& j) {
    if (!j.is_object()) return;
    for (auto& [action, key] : j.items()) {
        if (key.is_string()) {
            m_bindings[action] = key.get<std::string>();
        }
    }
}

void InputBindings::apply_defaults(const std::unordered_map<std::string, std::string>& defaults) {
    for (auto& [action, key] : defaults) {
        m_bindings.emplace(action, key);  // only inserts if not already present
    }
}

const std::string& InputBindings::get_key(const std::string& action) const {
    static const std::string empty;
    auto it = m_bindings.find(action);
    return (it != m_bindings.end()) ? it->second : empty;
}

// ── RTS defaults ─────────────────────────────────────────────────────────

const std::unordered_map<std::string, std::string>& rts_default_bindings() {
    static const std::unordered_map<std::string, std::string> defaults = {
        {"stop",          "S"},
        {"hold",          "H"},
        {"attack_move",   "A"},
        {"patrol",        "P"},
        {"select_hero_1", "F1"},
        {"select_hero_2", "F2"},
        {"select_hero_3", "F3"},
    };
    return defaults;
}

} // namespace uldum::input
