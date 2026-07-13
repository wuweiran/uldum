#include "core/settings.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace uldum::settings {

static constexpr const char* TAG = "Settings";

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
    // A missing key must return the fallback. We can't rely on
    // get()+get_if<bool> here: get() returns a default-constructed
    // Value{}, and the variant's first alternative is `bool`, so an
    // unset key yields a variant that genuinely holds bool(false) —
    // get_if<bool> would succeed and return false, swallowing the
    // fallback. (f32/i32 don't have this problem: the default variant
    // holds bool, not their type, so their get_if fails into the
    // fallback correctly.) Check presence explicitly first.
    auto it = m_values.find(std::string(key));
    if (it == m_values.end()) return fallback;
    if (auto* b = std::get_if<bool>(&it->second)) return *b;
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

bool Store::has(std::string_view key) const {
    return m_values.find(std::string(key)) != m_values.end();
}

void Store::save(const std::string& path) const {
    nlohmann::json root = nlohmann::json::object();
    for (const auto& [key, value] : m_values) {
        nlohmann::json entry;
        std::visit([&entry](auto&& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)             { entry["type"] = "bool";   entry["v"] = v; }
            else if constexpr (std::is_same_v<T, f32>)         { entry["type"] = "f32";    entry["v"] = v; }
            else if constexpr (std::is_same_v<T, i32>)         { entry["type"] = "i32";    entry["v"] = v; }
            else if constexpr (std::is_same_v<T, std::string>) { entry["type"] = "string"; entry["v"] = v; }
        }, value);
        root[key] = std::move(entry);
    }

    std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path() && !std::filesystem::exists(p.parent_path(), ec)) {
        std::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            log::warn(TAG, "Settings dir '{}' not writable ({}); not saved",
                      p.parent_path().string(), ec.message());
            return;
        }
    }

    std::ofstream file(path);
    if (!file.is_open()) {
        log::error(TAG, "Failed to write settings to '{}'", path);
        return;
    }
    file << root.dump(2);
    log::info(TAG, "Settings saved to '{}'", path);
}

bool Store::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;  // no file yet — defaults apply

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(file);
    } catch (const nlohmann::json::exception& e) {
        log::warn(TAG, "Failed to parse settings '{}': {}", path, e.what());
        return false;
    }
    if (!root.is_object()) return false;

    for (auto& [key, entry] : root.items()) {
        if (!entry.is_object() || !entry.contains("type") || !entry.contains("v")) continue;
        // Guard the type read: entry["type"] existing doesn't mean it's a
        // string. A numeric/array "type" would make .get<std::string>()
        // throw, and the try above only wraps parse() — the throw would
        // escape and terminate. is_string() keeps a malformed entry a skip.
        if (!entry["type"].is_string()) continue;
        const std::string type = entry["type"].get<std::string>();
        const auto& v = entry["v"];
        // Route through set() so subscribers react to the loaded value.
        if      (type == "bool"   && v.is_boolean())        set(key, v.get<bool>());
        else if (type == "f32"    && v.is_number())         set(key, v.get<f32>());
        else if (type == "i32"    && v.is_number_integer()) set(key, v.get<i32>());
        else if (type == "string" && v.is_string())         set(key, v.get<std::string>());
    }
    log::info(TAG, "Settings loaded from '{}'", path);
    return true;
}

} // namespace uldum::settings
