#include "simulation/type_registry.h"
#include "asset/asset.h"
#include "core/log.h"

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
            def.turn_rate         = m.value("turn_rate", 0.6f);
            def.collision_radius  = m.value("collision_radius", 32.0f);
            def.move_type         = parse_move_type(m.value("type", "ground"));
        }

        if (val.contains("combat")) {
            auto& c = val["combat"];
            def.damage           = c.value("damage", 10.0f);
            def.attack_range     = c.value("range", 1.0f);
            def.attack_cooldown  = c.value("cooldown", 1.0f);
            def.cast_point       = c.value("cast_point", 0.3f);
            def.backswing        = c.value("backswing", 0.3f);
            def.is_ranged        = c.value("ranged", false);
            def.projectile_speed = c.value("projectile_speed", 20.0f);
            def.acquire_range    = c.value("acquire_range", 10.0f);
        }

        if (val.contains("vision")) {
            auto& v = val["vision"];
            def.sight_range_day   = v.value("day", 1400.0f);
            def.sight_range_night = v.value("night", 800.0f);
        }

        if (val.contains("selection")) {
            auto& s = val["selection"];
            def.selection_radius   = s.value("radius", 1.0f);
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

        if (val.contains("hero")) {
            auto& h = val["hero"];
            def.hero_primary_attr = h.value("primary_attr", "");
            if (h.contains("attr_per_level")) {
                for (auto& [attr, growth] : h["attr_per_level"].items()) {
                    def.hero_attr_per_level[attr] = growth.get<f32>();
                }
            }
            for (auto& [attr, growth] : def.hero_attr_per_level) {
                if (def.attributes_numeric.contains(attr)) {
                    def.hero_base_attrs[attr] = def.attributes_numeric[attr];
                }
            }
        }

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
    }

    log::info(TAG, "Loaded {} unit types from '{}'", m_unit_types.size(), source);
    return true;
}

bool TypeRegistry::load_destructable_types_from_doc(const asset::JsonDocument* doc, std::string_view source) {
    for (auto& [key, val] : doc->data.items()) {
        DestructableTypeDef def;
        def.id = key;
        def.display_name = val.value("display_name", key);
        def.model_path   = val.value("model", "");
        def.variations   = static_cast<u8>(val.value("variations", 1));

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

        m_destructable_types[key] = std::move(def);
    }

    log::info(TAG, "Loaded {} destructable types from '{}'", m_destructable_types.size(), source);
    return true;
}

bool TypeRegistry::load_item_types_from_doc(const asset::JsonDocument* doc, std::string_view source) {
    for (auto& [key, val] : doc->data.items()) {
        ItemTypeDef def;
        def.id = key;
        def.display_name = val.value("display_name", key);
        def.icon_path    = val.value("icon", "");
        def.charges      = val.value("charges", 1);
        def.cooldown     = val.value("cooldown", 0.0f);
        def.gold_cost    = val.value("gold_cost", 0);

        m_item_types[key] = std::move(def);
    }

    log::info(TAG, "Loaded {} item types from '{}'", m_item_types.size(), source);
    return true;
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

} // namespace uldum::simulation
