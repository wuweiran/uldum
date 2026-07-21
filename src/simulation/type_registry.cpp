#include "simulation/type_registry.h"
#include "asset/asset.h"
#include "core/log.h"

#include <glm/gtc/constants.hpp>

namespace uldum::simulation {

static constexpr const char* TAG = "TypeRegistry";

// ── Public loaders (relative path) ────────────────────────────────────────

bool TypeRegistry::load_unit_types(asset::AssetManager& assets, std::string_view path) {
    auto handle = assets.load_config(path);
    auto* doc = assets.get(handle);
    if (!doc) { log::error(TAG, "Failed to load unit types from '{}'", path); return false; }
    return load_unit_types_from_doc(doc, path);
}

bool TypeRegistry::load_destructable_types(asset::AssetManager& assets, std::string_view path) {
    auto handle = assets.load_config(path);
    auto* doc = assets.get(handle);
    if (!doc) { log::error(TAG, "Failed to load destructable types from '{}'", path); return false; }
    return load_destructable_types_from_doc(doc, path);
}

bool TypeRegistry::load_item_types(asset::AssetManager& assets, std::string_view path) {
    auto handle = assets.load_config(path);
    auto* doc = assets.get(handle);
    if (!doc) { log::error(TAG, "Failed to load item types from '{}'", path); return false; }
    return load_item_types_from_doc(doc, path);
}

bool TypeRegistry::load_doodad_types(asset::AssetManager& assets, std::string_view path) {
    auto handle = assets.load_config(path);
    auto* doc = assets.get(handle);
    if (!doc) { log::error(TAG, "Failed to load doodad types from '{}'", path); return false; }
    return load_doodad_types_from_doc(doc, path);
}

// ── Public loaders (absolute path) ────────────────────────────────────────

bool TypeRegistry::load_unit_types_absolute(asset::AssetManager& assets, std::string_view abs_path) {
    auto handle = assets.load_config_absolute(abs_path);
    auto* doc = assets.get(handle);
    if (!doc) return false;
    return load_unit_types_from_doc(doc, abs_path);
}

bool TypeRegistry::load_destructable_types_absolute(asset::AssetManager& assets, std::string_view abs_path) {
    auto handle = assets.load_config_absolute(abs_path);
    auto* doc = assets.get(handle);
    if (!doc) return false;
    return load_destructable_types_from_doc(doc, abs_path);
}

bool TypeRegistry::load_item_types_absolute(asset::AssetManager& assets, std::string_view abs_path) {
    auto handle = assets.load_config_absolute(abs_path);
    auto* doc = assets.get(handle);
    if (!doc) return false;
    return load_item_types_from_doc(doc, abs_path);
}

bool TypeRegistry::load_doodad_types_absolute(asset::AssetManager& assets, std::string_view abs_path) {
    auto handle = assets.load_config_absolute(abs_path);
    auto* doc = assets.get(handle);
    if (!doc) return false;
    return load_doodad_types_from_doc(doc, abs_path);
}

// ── Internal parsers ──────────────────────────────────────────────────────

bool TypeRegistry::load_unit_types_from_doc(const asset::JsonDocument* doc, std::string_view source) {
    for (auto& [key, val] : doc->data.items()) {
        UnitTypeDef def;
        def.id = key;
        def.display_name = val.value("display_name", key);
        def.model_path   = val.value("model", "");
        def.model_scale  = val.value("model_scale", 1.0f);

        if (val.contains("health")) {
            auto& h = val["health"];
            def.max_health   = h.value("max", 100.0f);
            def.health_regen = h.value("regen", 0.0f);
        }

        if (val.contains("movement")) {
            auto& m = val["movement"];
            def.move_speed        = m.value("speed", 270.0f);
            // WC3 turn_rate convention: 0.5 ≈ 360°/s. Convert to rad/s: rate * 4π.
            def.turn_rate         = m.value("turn_rate", 0.6f) * 4.0f * glm::pi<f32>();
            def.collision_radius  = m.value("collision_radius", 32.0f);
            def.move_type         = parse_move_type(m.value("type", "ground"));
            def.fly_height        = m.value("fly_height", 0.0f);
        }

        // Pathing footprint — either a 2-element array `[w, h]` or
        // separate `pathing_footprint_w` / `pathing_footprint_h` fields
        // on the unit type. Buildings should set this; mobile units
        // leave it absent.
        if (auto fp = val.find("pathing_footprint"); fp != val.end()) {
            if (fp->is_array() && fp->size() == 2) {
                def.pathing_footprint_w = fp->at(0).get<u32>();
                def.pathing_footprint_h = fp->at(1).get<u32>();
            }
        } else {
            def.pathing_footprint_w = val.value("pathing_footprint_w", 0u);
            def.pathing_footprint_h = val.value("pathing_footprint_h", 0u);
        }

        if (val.contains("combat")) {
            auto& c = val["combat"];
            def.damage           = c.value("damage", 10.0f);
            def.attack_range     = c.value("range", 1.0f);
            def.attack_cooldown  = c.value("cooldown", 1.0f);
            def.dmg_time         = c.value("dmg_time", 0.3f);
            def.backsw_time      = c.value("backsw_time", 0.3f);
            // Ranged attack: a `projectile` block makes this attack fire a
            // missile. Its presence replaces the old `ranged` flag.
            if (c.contains("projectile")) {
                auto& pj = c["projectile"];
                ProjectileSpec spec;
                spec.model = pj.value("model", "");
                spec.speed = pj.value("speed", 20.0f);
                spec.arc   = pj.value("arc", 0.0f);
                if (pj.contains("launch")) {
                    auto& l = pj["launch"];
                    spec.launch = {l.value("forward", 0.0f),
                                   l.value("side", 0.0f),
                                   l.value("height", 0.0f)};
                }
                def.projectile = std::move(spec);
            }
            def.acquire_range    = c.value("acquire_range", 10.0f);
            // Attack target layers. JSON "targets": ["ground","air",...]. Omitted
            // → surface (ground/water/amphibious) + structure, NOT air — units
            // opt into anti-air. parse_target_mask({}) supplies the implicit
            // STRUCTURE bit so an ordinary unit can still smash crates/barrels.
            std::vector<std::string> tl;
            if (c.contains("targets") && c["targets"].is_array()) {
                for (auto& t : c["targets"]) if (t.is_string()) tl.push_back(t.get<std::string>());
            }
            def.target_mask = parse_target_mask(tl);
        }

        if (val.contains("animation")) {
            auto& a = val["animation"];
            def.dmg_pt           = a.value("dmg_pt", 0.5f);
            def.cast_pt          = a.value("cast_pt", 0.5f);
            def.walk_speed       = a.value("walk_speed", 0.0f);
        }

        if (val.contains("vision")) {
            auto& v = val["vision"];
            def.sight_range = v.value("range", 1400.0f);
        }

        if (val.contains("selection")) {
            auto& s = val["selection"];
            // All-or-nothing override: an authored "radius" switches OFF auto
            // sizing and uses the JSON cylinder. height defaults to 2× radius
            // if omitted. No radius → leave 0 (auto from model). priority always.
            if (s.contains("radius")) {
                def.selection_radius = s.value("radius", 1.0f);
                def.selection_height = s.value("height", def.selection_radius * 2.0f);
            }
            def.selection_priority = s.value("priority", 5);
        }

        if (val.contains("classifications")) {
            for (const auto& c : val["classifications"]) {
                def.classifications.push_back(c.get<std::string>());
            }
        }

        if (val.contains("abilities")) {
            for (const auto& a : val["abilities"]) {
                def.abilities.push_back(a.get<std::string>());
            }
        }

        if (val.contains("states")) {
            for (auto& [sid, sval] : val["states"].items()) {
                UnitTypeDef::StateDef sd;
                sd.max   = sval.value("max", 0.0f);
                sd.regen = sval.value("regen", 0.0f);
                def.states[sid] = sd;
            }
        }

        if (val.contains("attributes")) {
            for (auto& [aid, aval] : val["attributes"].items()) {
                if (aval.is_number()) {
                    def.attributes_numeric[aid] = aval.get<f32>();
                } else if (aval.is_string()) {
                    def.attributes_string[aid] = aval.get<std::string>();
                }
            }
        }

        def.inventory_size = val.value("inventory_size", 0u);

        if (val.contains("sounds")) {
            auto& s = val["sounds"];
            def.sound_attack = s.value("attack", "");
            def.sound_death  = s.value("death", "");
            def.sound_birth  = s.value("birth", "");
        }

        if (val.contains("building")) {
            auto& b = val["building"];
            def.build_time = b.value("build_time", 60.0f);
        }

        log::trace(TAG, "Registered unit type '{}' (hp={}, speed={}, dmg={})",
                   def.id, def.max_health, def.move_speed, def.damage);
        m_unit_types[key] = std::move(def);

        // Cache string-typed top-level fields for i18n raw fallback.
        // Localized packs override; this is the default-language source.
        RawFields raw;
        for (auto& [field, fv] : val.items()) {
            if (fv.is_string()) raw.emplace(field, fv.get<std::string>());
        }
        m_raw_units[key] = std::move(raw);
    }

    log::info(TAG, "Loaded {} unit types from '{}'", m_unit_types.size(), source);
    return true;
}

bool TypeRegistry::load_destructable_types_from_doc(const asset::JsonDocument* doc, std::string_view source) {
    for (auto& [key, val] : doc->data.items()) {
        DestructableTypeDef def;
        def.id = key;
        if (val.contains("models") && val["models"].is_array()) {
            for (auto& m : val["models"]) {
                if (m.is_string()) def.models.push_back(m.get<std::string>());
            }
        }
        if (def.models.empty()) {
            log::warn(TAG, "Destructable type '{}' has no models", key);
        }
        def.model_scale = val.value("model_scale", 1.0f);

        if (val.contains("health")) {
            auto& h = val["health"];
            def.max_health = h.value("max", 50.0f);
        }

        if (val.contains("attributes")) {
            for (auto& [aid, aval] : val["attributes"].items()) {
                if (aval.is_number()) {
                    def.attributes_numeric[aid] = aval.get<f32>();
                } else if (aval.is_string()) {
                    def.attributes_string[aid] = aval.get<std::string>();
                }
            }
        }

        if (auto fp = val.find("pathing_footprint"); fp != val.end() && fp->is_array() && fp->size() == 2) {
            def.pathing_footprint_w = fp->at(0).get<u32>();
            def.pathing_footprint_h = fp->at(1).get<u32>();
        }

        // "Targeted As" — WC3's attack-handshake axis (how the destructable is
        // hit). "tree" makes it un-choppable by ordinary units (only
        // tree-targeting attacks hit it); "structure" reads as building-like;
        // omitted / anything else → debris (crate/barrel: ordinary units can
        // smash). structure + debris are both default-hittable.
        if (auto c = val.find("targeted_as"); c != val.end() && c->is_array()) {
            for (auto& cid : *c) {
                if (cid.is_string()) def.targeted_as.push_back(cid.get<std::string>());
            }
        }
        def.target_bit = widget_target_from_targeted_as(def.targeted_as);

        // Selectable by left-click? WC3: trees aren't selectable; crates/other
        // destructables are. Default follows the "tree" tag; an explicit
        // "selectable" field overrides either way.
        def.selectable = !has_classification(def.targeted_as, "tree");
        if (auto s = val.find("selectable"); s != val.end() && s->is_boolean()) {
            def.selectable = s->get<bool>();
        }

        m_destructable_types[key] = std::move(def);

        RawFields raw;
        for (auto& [field, fv] : val.items()) {
            if (fv.is_string()) raw.emplace(field, fv.get<std::string>());
        }
        m_raw_destructables[key] = std::move(raw);
    }

    log::info(TAG, "Loaded {} destructable types from '{}'", m_destructable_types.size(), source);
    return true;
}

bool TypeRegistry::load_item_types_from_doc(const asset::JsonDocument* doc, std::string_view source) {
    for (auto& [key, val] : doc->data.items()) {
        ItemTypeDef def;
        def.id              = key;
        def.display_name    = val.value("name", val.value("display_name", key));
        def.tooltip         = val.value("tooltip", "");
        def.icon_path       = val.value("icon", "");
        def.model_path      = val.value("model", "");
        def.model_scale     = val.value("model_scale", 1.0f);
        def.pickup_radius   = val.value("pickup_radius", 48.0f);
        def.item_class      = parse_item_class(val.value("type", std::string("permanent")));
        def.initial_charges = val.value("initial_charges", 0);
        def.initial_level   = val.value("initial_level",   0);
        if (def.item_class != ItemClass::Charged && def.initial_charges != 0) {
            log::warn(TAG, "Item '{}': 'initial_charges' is only meaningful on type 'charged' — ignoring", key);
            def.initial_charges = 0;
        }
        if (auto a = val.find("abilities"); a != val.end() && a->is_array()) {
            for (auto& aid : *a) {
                if (aid.is_string()) def.abilities.push_back(aid.get<std::string>());
            }
        }

        m_item_types[key] = std::move(def);

        RawFields raw;
        for (auto& [field, fv] : val.items()) {
            if (fv.is_string()) raw.emplace(field, fv.get<std::string>());
        }
        m_raw_items[key] = std::move(raw);
    }

    log::info(TAG, "Loaded {} item types from '{}'", m_item_types.size(), source);
    return true;
}

bool TypeRegistry::load_doodad_types_from_doc(const asset::JsonDocument* doc, std::string_view source) {
    for (auto& [key, val] : doc->data.items()) {
        DoodadTypeDef def;
        def.id           = key;
        if (val.contains("models") && val["models"].is_array()) {
            for (auto& m : val["models"]) {
                if (m.is_string()) def.models.push_back(m.get<std::string>());
            }
        }
        if (def.models.empty()) {
            log::warn(TAG, "Doodad type '{}' has no models", key);
        }
        def.model_scale = val.value("model_scale", 1.0f);

        m_doodad_types[key] = std::move(def);

        RawFields raw;
        for (auto& [field, fv] : val.items()) {
            if (fv.is_string()) raw.emplace(field, fv.get<std::string>());
        }
        m_raw_doodads[key] = std::move(raw);
    }
    log::info(TAG, "Loaded {} doodad types from '{}'", m_doodad_types.size(), source);
    return true;
}

std::optional<std::string> TypeRegistry::raw_string_field(Category cat,
                                                            std::string_view entity_id,
                                                            std::string_view field) const {
    // Items have a `tooltip` schema field that defaults to "" when
    // unauthored. Returning the struct value gives the property the
    // same "missing JSON property = empty value, not absent key"
    // semantic as optional numeric fields elsewhere in the loader.
    if (cat == Category::Item && field == "tooltip") {
        auto it = m_item_types.find(std::string(entity_id));
        if (it == m_item_types.end()) return std::nullopt;
        return it->second.tooltip;
    }
    const RawCache* cache = nullptr;
    switch (cat) {
        case Category::Unit:         cache = &m_raw_units; break;
        case Category::Destructable: cache = &m_raw_destructables; break;
        case Category::Doodad:       cache = &m_raw_doodads; break;
        case Category::Item:         cache = &m_raw_items; break;
    }
    auto eit = cache->find(std::string(entity_id));
    if (eit == cache->end()) return std::nullopt;
    auto fit = eit->second.find(std::string(field));
    if (fit == eit->second.end()) return std::nullopt;
    return fit->second;
}

// ── Lookups ───────────────────────────────────────────────────────────────

const UnitTypeDef* TypeRegistry::get_unit_type(std::string_view id) const {
    auto it = m_unit_types.find(std::string(id));
    return it != m_unit_types.end() ? &it->second : nullptr;
}

const DestructableTypeDef* TypeRegistry::get_destructable_type(std::string_view id) const {
    auto it = m_destructable_types.find(std::string(id));
    return it != m_destructable_types.end() ? &it->second : nullptr;
}

const ItemTypeDef* TypeRegistry::get_item_type(std::string_view id) const {
    auto it = m_item_types.find(std::string(id));
    return it != m_item_types.end() ? &it->second : nullptr;
}

const DoodadTypeDef* TypeRegistry::get_doodad_type(std::string_view id) const {
    auto it = m_doodad_types.find(std::string(id));
    return it != m_doodad_types.end() ? &it->second : nullptr;
}

} // namespace uldum::simulation
