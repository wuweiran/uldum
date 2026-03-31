#include "simulation/type_registry.h"
#include "asset/asset.h"
#include "core/log.h"

namespace uldum::simulation {

static constexpr const char* TAG = "TypeRegistry";

// Helper: parse classification flags from JSON array
static Classification parse_classifications(const nlohmann::json& arr) {
    Classification flags = Classification::None;
    for (const auto& s : arr) {
        std::string v = s.get<std::string>();
        if (v == "ground")     flags = flags | Classification::Ground;
        else if (v == "air")        flags = flags | Classification::Air;
        else if (v == "mechanical") flags = flags | Classification::Mechanical;
        else if (v == "undead")     flags = flags | Classification::Undead;
        else if (v == "worker")     flags = flags | Classification::Worker;
        else if (v == "ancient")    flags = flags | Classification::Ancient;
        else if (v == "hero")       flags = flags | Classification::Hero;
        else if (v == "structure")  flags = flags | Classification::Structure;
        else if (v == "summoned")   flags = flags | Classification::Summoned;
    }
    return flags;
}

static ArmorType parse_armor_type(const std::string& s) {
    if (s == "light")     return ArmorType::Light;
    if (s == "medium")    return ArmorType::Medium;
    if (s == "heavy")     return ArmorType::Heavy;
    if (s == "fortified") return ArmorType::Fortified;
    if (s == "hero")      return ArmorType::Hero;
    return ArmorType::Unarmored;
}

static AttackType parse_attack_type(const std::string& s) {
    if (s == "pierce") return AttackType::Pierce;
    if (s == "siege")  return AttackType::Siege;
    if (s == "magic")  return AttackType::Magic;
    if (s == "chaos")  return AttackType::Chaos;
    if (s == "hero")   return AttackType::Hero;
    return AttackType::Normal;
}

static MoveType parse_move_type(const std::string& s) {
    if (s == "air")        return MoveType::Air;
    if (s == "amphibious") return MoveType::Amphibious;
    return MoveType::Ground;
}

bool TypeRegistry::load_unit_types(asset::AssetManager& assets, std::string_view path) {
    auto handle = assets.load_config(path);
    auto* doc = assets.get(handle);
    if (!doc) {
        log::error(TAG, "Failed to load unit types from '{}'", path);
        return false;
    }

    for (auto& [key, val] : doc->data.items()) {
        UnitTypeDef def;
        def.id = key;
        def.display_name = val.value("display_name", key);
        def.model_path   = val.value("model", "");

        if (val.contains("health")) {
            auto& h = val["health"];
            def.max_health  = h.value("max", 100.0f);
            def.health_regen = h.value("regen", 0.0f);
            def.armor       = h.value("armor", 0.0f);
            def.armor_type  = parse_armor_type(h.value("armor_type", "unarmored"));
        }

        if (val.contains("movement")) {
            auto& m = val["movement"];
            def.move_speed = m.value("speed", 270.0f);
            def.turn_rate  = m.value("turn_rate", 0.6f);
            def.move_type  = parse_move_type(m.value("type", "ground"));
        }

        if (val.contains("combat")) {
            auto& c = val["combat"];
            def.damage         = c.value("damage", 10.0f);
            def.attack_range   = c.value("range", 1.0f);
            def.attack_cooldown = c.value("cooldown", 1.0f);
            def.attack_type    = parse_attack_type(c.value("attack_type", "normal"));
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
            def.classifications = parse_classifications(val["classifications"]);
        }

        if (val.contains("abilities")) {
            for (const auto& a : val["abilities"]) {
                def.abilities.push_back(a.get<std::string>());
            }
        }

        // Hero data
        if (val.contains("hero")) {
            auto& h = val["hero"];
            def.hero_str = h.value("str", 0.0f);
            def.hero_agi = h.value("agi", 0.0f);
            def.hero_int = h.value("int", 0.0f);
            def.hero_str_per_level = h.value("str_per_level", 0.0f);
            def.hero_agi_per_level = h.value("agi_per_level", 0.0f);
            def.hero_int_per_level = h.value("int_per_level", 0.0f);
            std::string attr = h.value("primary_attr", "str");
            if (attr == "agi") def.hero_primary_attr = Attribute::Agi;
            else if (attr == "int") def.hero_primary_attr = Attribute::Int;
            else def.hero_primary_attr = Attribute::Str;
        }

        // Building data
        if (val.contains("building")) {
            auto& b = val["building"];
            def.build_time = b.value("build_time", 60.0f);
        }

        log::trace(TAG, "Registered unit type '{}' (hp={}, speed={}, dmg={})",
                   def.id, def.max_health, def.move_speed, def.damage);
        m_unit_types[key] = std::move(def);
    }

    log::info(TAG, "Loaded {} unit types from '{}'", m_unit_types.size(), path);
    return true;
}

bool TypeRegistry::load_destructable_types(asset::AssetManager& assets, std::string_view path) {
    auto handle = assets.load_config(path);
    auto* doc = assets.get(handle);
    if (!doc) {
        log::error(TAG, "Failed to load destructable types from '{}'", path);
        return false;
    }

    for (auto& [key, val] : doc->data.items()) {
        DestructableTypeDef def;
        def.id = key;
        def.display_name = val.value("display_name", key);
        def.model_path   = val.value("model", "");
        def.variations   = static_cast<u8>(val.value("variations", 1));

        if (val.contains("health")) {
            auto& h = val["health"];
            def.max_health = h.value("max", 50.0f);
            def.armor      = h.value("armor", 0.0f);
            def.armor_type = parse_armor_type(h.value("armor_type", "fortified"));
        }

        m_destructable_types[key] = std::move(def);
    }

    log::info(TAG, "Loaded {} destructable types from '{}'", m_destructable_types.size(), path);
    return true;
}

bool TypeRegistry::load_item_types(asset::AssetManager& assets, std::string_view path) {
    auto handle = assets.load_config(path);
    auto* doc = assets.get(handle);
    if (!doc) {
        log::error(TAG, "Failed to load item types from '{}'", path);
        return false;
    }

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

    log::info(TAG, "Loaded {} item types from '{}'", m_item_types.size(), path);
    return true;
}

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
