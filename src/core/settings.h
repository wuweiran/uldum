#pragma once

#include "core/types.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

// Engine-wide settings store — the state side of the Shell-UI ↔ engine
// contract. Typed key/value pairs with observer callbacks so subsystems
// (audio, renderer, input) react automatically to UI changes without
// needing to know about the UI layer.
//
// Keys use dot-namespaced strings: "audio.master_enabled", "graphics.fullscreen".
// Values are a small variant of types we actually need — bool, f32, i32,
// string. Expand when a real use case demands it.
//
// Not in scope (yet): persistence, schema validation, default reset. Those
// land as Tier 3 when we have more settings to justify the machinery.

namespace uldum::settings {

using Value = std::variant<bool, f32, i32, std::string>;

class Store {
public:
    using Listener = std::function<void(const Value&)>;

    // Set a value. Fires all registered listeners on this key. Accepts any
    // Value type; observers decide whether the shape matches their
    // expectation (they typically use std::get_if<T>).
    void set(std::string_view key, Value value);

    // Read the current value. Returns a default-constructed variant (bool=false)
    // if the key has never been set.
    Value get(std::string_view key) const;

    // Typed convenience accessors — return a fallback if the key is unset
    // or the stored type doesn't match.
    bool    get_bool(std::string_view key, bool fallback = false) const;
    f32     get_f32 (std::string_view key, f32  fallback = 0.0f) const;
    i32     get_i32 (std::string_view key, i32  fallback = 0)    const;

    // Subscribe to changes on one key. Every `set()` on that key invokes
    // the listener with the new value. Listeners aren't called on `get()`.
    // No unsubscribe for now — lifetime is engine-scoped.
    void subscribe(std::string_view key, Listener listener);

private:
    std::unordered_map<std::string, Value> m_values;
    std::unordered_map<std::string, std::vector<Listener>> m_listeners;
};

} // namespace uldum::settings
