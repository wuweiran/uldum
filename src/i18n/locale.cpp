#include "i18n/locale.h"

#include "asset/asset.h"
#include "core/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace uldum::i18n {

namespace {
constexpr const char* TAG = "I18n";

// Entity prefixes that route to a paired entity file in the map pool. The
// LEFT side is the singular key prefix; the RIGHT side is the file basename
// (no `.json`). Mirrors the structure in `types/<entity>.json`.
struct EntityRoute {
    std::string_view prefix;       // "ability"
    std::string_view file_basename; // "abilities"
};
constexpr EntityRoute ENTITY_ROUTES[] = {
    { "ability",      "abilities" },
    { "unit",         "units" },
    { "item",         "items" },
    { "destructable", "destructables" },
    { "doodad",       "doodads" },
};

constexpr const char* TEXT_FILE  = "text";
constexpr const char* SHELL_FILE = "shell";

// Map-pool file basenames the LocalePack tries to load.
const std::vector<std::string> MAP_BASENAMES = {
    "abilities", "units", "items", "destructables", "doodads", "text"
};

const std::vector<std::string> SHELL_BASENAMES = { "shell" };

} // namespace

// ── LocalePack ───────────────────────────────────────────────────────────────

struct LocalePack::Impl {
    std::unordered_map<std::string, nlohmann::json> files;
};

LocalePack::LocalePack() : m_impl(std::make_unique<Impl>()) {}
LocalePack::~LocalePack() = default;
LocalePack::LocalePack(LocalePack&&) noexcept = default;
LocalePack& LocalePack::operator=(LocalePack&&) noexcept = default;

void LocalePack::clear() { m_impl->files.clear(); }
bool LocalePack::empty() const { return m_impl->files.empty(); }

u32 LocalePack::load(asset::AssetManager& assets, std::string_view dir,
                      const std::vector<std::string>& basenames) {
    m_impl->files.clear();
    u32 loaded = 0;
    std::string base(dir);
    if (!base.empty() && base.back() != '/') base += '/';
    for (const auto& bn : basenames) {
        std::string path = base + bn + ".json";
        auto bytes = assets.read_file_bytes(path);
        if (bytes.empty()) continue;  // file optional — skip silently
        try {
            auto j = nlohmann::json::parse(bytes.begin(), bytes.end());
            if (j.is_object()) {
                m_impl->files.emplace(bn, std::move(j));
                ++loaded;
            } else {
                log::warn(TAG, "Locale file '{}' root is not an object — skipped", path);
            }
        } catch (const std::exception& e) {
            log::warn(TAG, "Failed to parse locale file '{}': {}", path, e.what());
        }
    }
    return loaded;
}

std::optional<std::string> LocalePack::lookup(std::string_view file_basename,
                                                std::string_view inner_key) const {
    auto it = m_impl->files.find(std::string(file_basename));
    if (it == m_impl->files.end()) return std::nullopt;
    const auto& root = it->second;
    if (inner_key.empty()) return std::nullopt;

    // Split inner_key on the first dot. Left side is the second-level key
    // (entity id for entity files, category for text/shell). Right side is
    // a single flat property name on that level — dots in property names
    // are literal (e.g. "tooltip.3").
    auto dot = inner_key.find('.');
    if (dot == std::string_view::npos) {
        // No second segment — the inner key is the top-level field itself.
        // Used for e.g. plain shell lookups that fit one level: shell["title"].
        // Most callers will have two segments though.
        auto field = root.find(std::string(inner_key));
        if (field == root.end()) return std::nullopt;
        if (!field->is_string()) return std::nullopt;
        return field->get<std::string>();
    }
    std::string second(inner_key.substr(0, dot));
    std::string rest(inner_key.substr(dot + 1));

    auto outer = root.find(second);
    if (outer == root.end() || !outer->is_object()) return std::nullopt;
    auto inner = outer->find(rest);
    if (inner == outer->end() || !inner->is_string()) return std::nullopt;
    return inner->get<std::string>();
}

// ── LocaleManager ────────────────────────────────────────────────────────────

LocaleManager::LocaleManager() = default;
LocaleManager::~LocaleManager() = default;

void LocaleManager::set_active(std::string_view code) {
    if (code == m_active) return;
    m_active = std::string(code);
    // Re-load active packs if we have a remembered root.
    if (m_assets) {
        if (!m_shell_root.empty()) {
            m_shell_active.load(*m_assets, m_shell_root + "/" + m_active, SHELL_BASENAMES);
        }
        if (!m_map_root.empty()) {
            m_map_active.load(*m_assets, m_map_root + "/" + m_active, MAP_BASENAMES);
        }
    }
    log::info(TAG, "Active locale set to '{}'", m_active);
}

void LocaleManager::load_shell(asset::AssetManager& assets, std::string_view dir) {
    m_assets = &assets;
    m_shell_root = std::string(dir);
    m_shell_active.load(assets, m_shell_root + "/" + m_active, SHELL_BASENAMES);
    if (m_active != m_default) {
        m_shell_default.load(assets, m_shell_root + "/" + m_default, SHELL_BASENAMES);
    } else {
        m_shell_default.clear();
    }
    log::info(TAG, "Loaded shell pack (active='{}', default='{}')", m_active, m_default);
}

void LocaleManager::load_map(asset::AssetManager& assets, std::string_view dir) {
    m_assets = &assets;
    m_map_root = std::string(dir);
    u32 a = m_map_active.load(assets, m_map_root + "/" + m_active, MAP_BASENAMES);
    u32 d = 0;
    if (m_active != m_default) {
        d = m_map_default.load(assets, m_map_root + "/" + m_default, MAP_BASENAMES);
    } else {
        m_map_default.clear();
    }
    log::info(TAG, "Loaded map pack from '{}' (active={} files, default={} files)",
              m_map_root, a, d);
}

void LocaleManager::unload_map() {
    m_map_active.clear();
    m_map_default.clear();
    m_map_root.clear();
}

bool LocaleManager::route(Pool pool, std::string_view key,
                           std::string& out_file, std::string& out_inner) {
    if (pool == Pool::Shell) {
        out_file = SHELL_FILE;
        out_inner = std::string(key);
        return false;  // not an "entity match"; raw fallback doesn't apply
    }
    // Map pool: check entity prefix routing.
    auto dot = key.find('.');
    if (dot != std::string_view::npos) {
        std::string_view prefix = key.substr(0, dot);
        std::string_view rest   = key.substr(dot + 1);
        for (const auto& r : ENTITY_ROUTES) {
            if (prefix == r.prefix) {
                out_file = std::string(r.file_basename);
                out_inner = std::string(rest);
                return true;
            }
        }
    }
    // Fall through to text.json.
    out_file = TEXT_FILE;
    out_inner = std::string(key);
    return false;
}

std::optional<std::string> LocaleManager::try_resolve(Pool pool, std::string_view key,
                                                      const ArgsMap& args) const {
    std::string file, inner;
    bool is_entity = route(pool, key, file, inner);

    auto try_pool = [&](const LocalePack& pack) -> std::optional<std::string> {
        return pack.lookup(file, inner);
    };

    // Step 1: active locale pack.
    if (auto v = try_pool(pool == Pool::Shell ? m_shell_active : m_map_active)) {
        return apply_args(*v, args);
    }
    // Step 2: default locale pack (only if different from active).
    if (m_active != m_default) {
        if (auto v = try_pool(pool == Pool::Shell ? m_shell_default : m_map_default)) {
            return apply_args(*v, args);
        }
    }
    // Step 3: raw fallback to the entity registry. Entity keys carry
    // their own schema defaults — the callback may return Some("") for a
    // schema-known property that wasn't authored. We treat that as a hit.
    if (pool == Pool::Map && is_entity && m_raw_fallback) {
        if (auto v = m_raw_fallback(key)) {
            return apply_args(*v, args);
        }
    }
    // Total miss — no literal-key substitute here (that's resolve()'s job).
    return std::nullopt;
}

std::string LocaleManager::resolve(Pool pool, std::string_view key,
                                     const ArgsMap& args) const {
    if (auto v = try_resolve(pool, key, args)) return *v;
    // Literal key. Reached only for non-entity keys or unknown entity
    // fields — surfaces the key in the UI so authors spot it.
    return std::string(key);
}

std::string LocaleManager::resolve(Pool pool, const LocalizedString& loc) const {
    if (loc.empty()) return std::string{};
    ArgsMap args;
    args.reserve(loc.args.size());
    for (const auto& [k, v] : loc.args) args.emplace(k, v);
    return resolve(pool, loc.key, args);
}

std::string LocaleManager::apply_args(std::string_view tmpl, const ArgsMap& args) {
    // Literal `{name}` substitution. Unknown placeholders pass through. v1
    // implements named substitution only — ICU plural/select land later.
    std::string out;
    out.reserve(tmpl.size());
    usize i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '{') {
            // Find closing brace.
            auto end = tmpl.find('}', i + 1);
            if (end == std::string_view::npos) {
                out.append(tmpl.substr(i));
                break;
            }
            std::string name(tmpl.substr(i + 1, end - i - 1));
            auto it = args.find(name);
            if (it != args.end()) {
                out.append(it->second);
            } else {
                // Unknown placeholder — emit verbatim so authors can spot it.
                out.append(tmpl.substr(i, end - i + 1));
            }
            i = end + 1;
        } else {
            out.push_back(tmpl[i]);
            ++i;
        }
    }
    return out;
}

void LocaleManager::load_locale_registry(asset::AssetManager& assets, std::string_view path) {
    m_available.clear();
    auto bytes = assets.read_file_bytes(path);
    if (bytes.empty()) {
        log::warn(TAG, "Locale registry '{}' missing", path);
        return;
    }
    try {
        auto j = nlohmann::json::parse(bytes.begin(), bytes.end());
        if (!j.is_object()) {
            log::warn(TAG, "Locale registry '{}' root is not an object", path);
            return;
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) {
                m_available.emplace(it.key(), it.value().get<std::string>());
            }
        }
        log::info(TAG, "Loaded locale registry: {} locale(s) available", m_available.size());
    } catch (const std::exception& e) {
        log::warn(TAG, "Failed to parse locale registry '{}': {}", path, e.what());
    }
}

} // namespace uldum::i18n
