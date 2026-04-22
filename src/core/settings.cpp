#include "core/settings.h"

namespace uldum::settings {

void Store::set(std::string_view key, Value value) {
    std::string key_str(key);
    m_values[key_str] = value;

    auto it = m_listeners.find(key_str);
    if (it == m_listeners.end()) return;
    // Copy the value into each listener's scope — the stored Value may be
    // mutated by a later set() inside a callback, so we don't pass a ref.
    for (auto& listener : it->second) listener(m_values[key_str]);
}

Value Store::get(std::string_view key) const {
    auto it = m_values.find(std::string(key));
    if (it == m_values.end()) return Value{};
    return it->second;
}

bool Store::get_bool(std::string_view key, bool fallback) const {
    auto v = get(key);
    if (auto* b = std::get_if<bool>(&v)) return *b;
    return fallback;
}

f32 Store::get_f32(std::string_view key, f32 fallback) const {
    auto v = get(key);
    if (auto* f = std::get_if<f32>(&v)) return *f;
    return fallback;
}

i32 Store::get_i32(std::string_view key, i32 fallback) const {
    auto v = get(key);
    if (auto* i = std::get_if<i32>(&v)) return *i;
    return fallback;
}

void Store::subscribe(std::string_view key, Listener listener) {
    m_listeners[std::string(key)].push_back(std::move(listener));
}

} // namespace uldum::settings
