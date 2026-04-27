#include "simulation/ability_def.h"
#include "asset/asset.h"
#include "core/log.h"

namespace uldum::simulation {

static constexpr const char* TAG = "AbilityRegistry";

AbilityForm parse_ability_form(const std::string& s) {
    if (s == "passive")      return AbilityForm::Passive;
    if (s == "aura")         return AbilityForm::Aura;
    if (s == "instant")      return AbilityForm::Instant;
    if (s == "target_unit")  return AbilityForm::TargetUnit;
    if (s == "target_point") return AbilityForm::TargetPoint;
    if (s == "toggle")       return AbilityForm::Toggle;
    return AbilityForm::Passive;
}

IndicatorShape parse_indicator_shape(const std::string& s) {
    if (s == "point") return IndicatorShape::Point;
    if (s == "area")  return IndicatorShape::Area;
    if (s == "line")  return IndicatorShape::Line;
    if (s == "cone")  return IndicatorShape::Cone;
    return IndicatorShape::Point;
}

static TargetFilter parse_target_filter(const nlohmann::json& j) {
    TargetFilter f;
    f.ally   = j.value("ally", false);
    f.enemy  = j.value("enemy", false);
    f.self_  = j.value("self", false);
    f.alive  = j.value("alive", true);
    f.dead   = j.value("dead", false);
    if (j.contains("classifications")) {
        for (auto& c : j["classifications"]) {
            f.classifications.push_back(c.get<std::string>());
        }
    }
    return f;
}

static AbilityLevelDef parse_level(const nlohmann::json& j) {
    AbilityLevelDef lvl;

    if (j.contains("cost")) {
        for (auto& [k, v] : j["cost"].items()) {
            lvl.cost[k] = v.get<f32>();
        }
    }
    lvl.cost_can_kill = j.value("cost_can_kill", false);
    lvl.cooldown      = j.value("cooldown", 0.0f);
    lvl.range         = j.value("range", 0.0f);
    lvl.cast_time     = j.value("cast_time", 0.0f);
    lvl.backsw_time   = j.value("backsw_time", 0.0f);
    lvl.damage        = j.value("damage", 0.0f);
    lvl.heal          = j.value("heal", 0.0f);

    if (j.contains("modifiers")) {
        for (auto& [k, v] : j["modifiers"].items()) {
            lvl.modifiers[k] = v.get<f32>();
        }
    }

    lvl.aura_radius   = j.value("aura_radius", 0.0f);
    lvl.aura_ability  = j.value("aura_ability", "");
    lvl.channel_time  = j.value("channel_time", 0.0f);

    if (j.contains("area")) {
        const auto& a = j["area"];
        lvl.area.radius = a.value("radius", 0.0f);
        lvl.area.width  = a.value("width",  0.0f);
        lvl.area.angle  = a.value("angle",  0.0f);
        lvl.has_area    = true;
    }

    if (j.contains("toggle_cost_per_sec")) {
        for (auto& [k, v] : j["toggle_cost_per_sec"].items()) {
            lvl.toggle_cost_per_sec[k] = v.get<f32>();
        }
    }

    lvl.projectile_speed = j.value("projectile_speed", 0.0f);

    return lvl;
}

bool AbilityRegistry::load(asset::AssetManager& assets, std::string_view abs_path) {
    auto handle = assets.load_config_absolute(abs_path);
    auto* doc = assets.get(handle);
    if (!doc) return false;
    return load_from_doc(doc, abs_path);
}

bool AbilityRegistry::load_from_doc(const asset::JsonDocument* doc, std::string_view source) {
    for (auto& [key, val] : doc->data.items()) {
        AbilityDef def;
        def.id        = key;
        def.name      = val.value("name", key);
        def.icon      = val.value("icon", "");
        def.form      = parse_ability_form(val.value("form", "passive"));
        def.shape     = parse_indicator_shape(val.value("shape", "point"));
        def.hotkey    = val.value("hotkey", "");
        def.stackable = val.value("stackable", false);
        def.hidden    = val.value("hidden", false);
        def.max_level = val.value("max_level", 1u);

        if (val.contains("target_filter")) {
            def.target_filter = parse_target_filter(val["target_filter"]);
        }

        if (val.contains("levels")) {
            for (auto& lvl_json : val["levels"]) {
                def.levels.push_back(parse_level(lvl_json));
            }
        }

        // If no levels defined, create one default level
        if (def.levels.empty()) {
            def.levels.push_back({});
        }

        log::trace(TAG, "Registered ability '{}' (form={}, levels={})",
                   def.id, val.value("form", "passive"), def.levels.size());
        m_defs[key] = std::move(def);
    }

    log::info(TAG, "Loaded {} ability defs from '{}'", m_defs.size(), source);
    return true;
}

const AbilityDef* AbilityRegistry::get(std::string_view id) const {
    auto it = m_defs.find(std::string(id));
    return it != m_defs.end() ? &it->second : nullptr;
}

} // namespace uldum::simulation
