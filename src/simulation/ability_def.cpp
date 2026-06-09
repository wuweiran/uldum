#include "simulation/ability_def.h"
#include "asset/asset.h"
#include "core/log.h"

namespace uldum::simulation {

static constexpr const char* TAG = "AbilityRegistry";

AbilityForm parse_ability_form(const std::string& s) {
    if (s == "passive_modifier") return AbilityForm::PassiveModifier;
    if (s == "passive_flag")     return AbilityForm::PassiveFlag;
    // Legacy alias — older defs used "passive" for what is now passive_modifier.
    if (s == "passive")          return AbilityForm::PassiveModifier;
    if (s == "aura")             return AbilityForm::Aura;
    if (s == "instant")          return AbilityForm::Instant;
    if (s == "target")           return AbilityForm::Target;
    return AbilityForm::PassiveModifier;
}

static u32 parse_widget_kinds(const nlohmann::json& j) {
    u32 mask = 0;
    if (!j.is_array()) return mask;
    for (const auto& v : j) {
        if (!v.is_string()) continue;
        const std::string s = v.get<std::string>();
        if      (s == "unit")          mask |= widget_kind::Unit;
        else if (s == "destructable")  mask |= widget_kind::Destructable;
        else if (s == "item")          mask |= widget_kind::Item;
    }
    return mask;
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

    if (j.contains("flags") && j["flags"].is_array()) {
        for (auto& f : j["flags"]) {
            if (f.is_string()) lvl.flags.push_back(f.get<std::string>());
        }
    }

    lvl.duration      = j.value("duration", -1.0f);
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
        def.tooltip   = val.value("tooltip", "");
        def.icon      = val.value("icon", "");
        const std::string form_str = val.value("form", "passive");
        def.form      = parse_ability_form(form_str);
        def.shape     = parse_indicator_shape(val.value("shape", "point"));
        def.hotkey    = val.value("hotkey", "");
        def.stackable      = val.value("stackable", false);
        def.hidden         = val.value("hidden", false);
        def.pierces_immune = val.value("pierces_immune", false);
        def.max_level = val.value("max_level", 1u);

        if (val.contains("target_filter")) {
            def.target_filter = parse_target_filter(val["target_filter"]);
        }

        // Target-form metadata. At least one of `widget_kinds` (a
        // non-empty array of "unit" / "destructable" / "item") or
        // `accept_point` (true) must be set for the form to do anything
        // useful; combining them yields the WC3-style hybrid cast.
        if (def.form == AbilityForm::Target) {
            if (val.contains("widget_kinds")) {
                def.widget_kinds = parse_widget_kinds(val["widget_kinds"]);
            }
            if (val.contains("accept_point")) {
                def.accept_point = val["accept_point"].get<bool>();
            }
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

        // Cache string-typed top-level fields for i18n raw fallback.
        RawFields raw;
        for (auto& [field, fv] : val.items()) {
            if (fv.is_string()) raw.emplace(field, fv.get<std::string>());
        }
        m_raw[key] = std::move(raw);
    }

    log::info(TAG, "Loaded {} ability defs from '{}'", m_defs.size(), source);
    return true;
}

const AbilityDef* AbilityRegistry::get(std::string_view id) const {
    auto it = m_defs.find(std::string(id));
    return it != m_defs.end() ? &it->second : nullptr;
}

std::optional<std::string> AbilityRegistry::raw_string_field(std::string_view ability_id,
                                                               std::string_view field) const {
    auto dit = m_defs.find(std::string(ability_id));
    if (dit == m_defs.end()) return std::nullopt;
    const AbilityDef& def = dit->second;

    // Schema-defined string fields. Returning the struct value gives
    // unauthored fields their natural default ("" for std::string) —
    // a missing JSON property reads back as an empty value, not as
    // "absent key". Numeric optional fields already get this for free
    // via `val.value(field, default)` in the loader; this brings
    // strings into line.
    if (field == "name")    return def.name;
    if (field == "tooltip") return def.tooltip;
    if (field == "icon")    return def.icon;
    if (field == "hotkey")  return def.hotkey;

    // Map-authored extension fields not in the schema fall through to
    // the raw JSON cache. Returns nullopt when the field isn't present —
    // caller (i18n raw fallback) takes that as "no value at all".
    auto rit = m_raw.find(std::string(ability_id));
    if (rit == m_raw.end()) return std::nullopt;
    auto fit = rit->second.find(std::string(field));
    if (fit == rit->second.end()) return std::nullopt;
    return fit->second;
}

} // namespace uldum::simulation
