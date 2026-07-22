#pragma once

#include "core/types.h"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uldum::asset { class AssetManager; }

namespace uldum::i18n {

// Pool selector. Shell strings live in engine/strings/<locale>/shell.json,
// owned by the App layer. Map strings live in <map>/strings/<locale>/<entity>.json,
// owned per-map. The two pools never cross-look-up.
enum class Pool : u8 { Shell, Map };

// Args dictionary for placeholder substitution. Values are stringified at
// the call site — the resolver just does literal `{name}` substitution.
using ArgsMap = std::unordered_map<std::string, std::string>;

// Localized text payload — the C++ counterpart of Lua's L(key, args)
// handle. Carries a lookup key + the substitution args (numbers /
// strings already stringified). Lives intact across the network: the
// host serializes the payload, each client resolves with its own
// LocaleManager. Player-facing APIs (CreateTextTag, SetLabelText,
// DisplayMessage, ...) accept this anywhere they take user-visible
// text, alongside a plain `std::string` for literal text. An empty
// `key` means "no localization payload" — caller should use a literal
// string instead.
struct LocalizedString {
    std::string key;
    std::vector<std::pair<std::string, std::string>> args;

    bool empty() const { return key.empty(); }
    void clear() { key.clear(); args.clear(); }
};

// Optional callback the LocaleManager invokes when a map-pool entity-key
// lookup misses every locale pack. Returns the raw default-language string
// for that engine-bound key, or nullopt if the type registry has no value.
// Wired by the type registry: for `ability.holy_light.tooltip`, the callback
// reads `types/abilities.json["holy_light"]["tooltip"]`.
using RawFallbackFn = std::function<std::optional<std::string>(std::string_view key)>;

// A loaded set of locale files for one (pool, locale) pair. Each file is a
// two-level nested JSON dict — see docs/i18n.md.
class LocalePack {
public:
    LocalePack();
    ~LocalePack();
    LocalePack(LocalePack&&) noexcept;
    LocalePack& operator=(LocalePack&&) noexcept;
    LocalePack(const LocalePack&) = delete;
    LocalePack& operator=(const LocalePack&) = delete;

    // Try to load each file in `basenames` (without .json extension) from
    // `dir`. Missing files are silently skipped — they're optional overrides.
    // Returns the number of files actually loaded.
    u32 load(asset::AssetManager& assets, std::string_view dir,
             const std::vector<std::string>& basenames);

    // Look up a key inside a specific file. inner_key is the portion of the
    // dotted lookup key AFTER the file-routing segment. Walks one level into
    // the JSON via the first remaining dot; the rest is a flat property name
    // on that level (including dotted suffixes like "tooltip.3").
    std::optional<std::string> lookup(std::string_view file_basename,
                                       std::string_view inner_key) const;

    void clear();
    bool empty() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Top-level locale resolver. Owns active + default packs for each pool,
// dispatches the fallback chain, applies placeholder substitution.
class LocaleManager {
public:
    LocaleManager();
    ~LocaleManager();

    // Active vs default locale. Default is "en"; active starts equal and is
    // overridden by config / CLI / settings.
    void set_active(std::string_view code);
    std::string_view active() const { return m_active; }
    std::string_view default_locale() const { return m_default; }

    // Load packs. Re-callable; replaces the previously loaded pack on the
    // same pool. `dir` is the directory containing locale subdirs:
    //
    //   engine/strings/         <locale>/shell.json
    //   <map>/strings/          <locale>/{abilities,units,items,...}.json
    void load_shell(asset::AssetManager& assets, std::string_view engine_strings_dir);
    void load_map(asset::AssetManager& assets, std::string_view map_strings_dir);

    // Drop the map pool. Shell pool persists across sessions.
    void unload_map();

    // Callback for raw fallback to `types/<entity>.json`. Map pool only.
    void set_raw_fallback_fn(RawFallbackFn fn) { m_raw_fallback = std::move(fn); }

    // Resolve a key against the given pool. Always returns a string —
    // never throws. Fallback chain:
    //   1. Active locale pack.
    //   2. Default locale pack (when different from active).
    //   3. Raw fallback to the entity registry (entity keys only). The
    //      registry owns schema defaults — a schema-known property that
    //      wasn't authored returns "" rather than nullopt. The chain
    //      treats that as a valid hit and stops.
    //   4. Literal key. Reached only when none of the above produced a
    //      value (non-entity key without translation, or unknown entity
    //      field). The key surfaces in the UI so authors spot it.
    // After lookup, `{name}` placeholders in the template are replaced by
    // matching entries from `args`. Unknown placeholders pass through
    // verbatim.
    std::string resolve(Pool pool, std::string_view key, const ArgsMap& args = {}) const;

    // Like `resolve`, but returns nullopt on a total miss (where `resolve`
    // would fall back to the literal key). Lets a caller walk a chain of
    // candidate keys and take the first that actually resolves — used by
    // the HUD's cast-reject `ui.error.<base>.<specifier>` → `.<base>` walk.
    std::optional<std::string> try_resolve(Pool pool, std::string_view key,
                                           const ArgsMap& args = {}) const;

    // Convenience overload — resolves a LocalizedString payload (as
    // delivered by the network). Flattens args to the ArgsMap shape.
    std::string resolve(Pool pool, const LocalizedString& loc) const;

    // Available locales from engine/strings/locales.json. Map: code → display name.
    const std::unordered_map<std::string, std::string>& available_locales() const {
        return m_available;
    }
    void load_locale_registry(asset::AssetManager& assets, std::string_view path);

private:
    static std::string apply_args(std::string_view tmpl, const ArgsMap& args);

    // Resolve (pool, locale) → (file basename, inner key). Returns true if
    // the prefix matched an entity convention; false routes to text.json /
    // shell.json with the whole key as inner_key.
    static bool route(Pool pool, std::string_view key,
                      std::string& out_file, std::string& out_inner);

    std::string m_active  = "en";
    std::string m_default = "en";
    LocalePack m_shell_active;
    LocalePack m_shell_default;
    LocalePack m_map_active;
    LocalePack m_map_default;
    RawFallbackFn m_raw_fallback;
    std::unordered_map<std::string, std::string> m_available;
    std::string m_shell_root;
    std::string m_map_root;
    asset::AssetManager* m_assets = nullptr;
};

} // namespace uldum::i18n
