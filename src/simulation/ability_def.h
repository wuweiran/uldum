#pragma once

#include "core/types.h"

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace uldum::asset { class AssetManager; struct JsonDocument; }

namespace uldum::simulation {

// Ability forms — engine-provided mechanical primitives. Channel is
// not a form: any form can declare a `channel_time` on its level data
// to make the cast sustained.
enum class AbilityForm : u8 {
    Passive,      // always active, modifiers + optional duration
    Aura,         // scan radius, apply/remove passive to nearby units
    Instant,      // cast_time → fire → backsw_time
    TargetUnit,   // + range + target validation + optional projectile
    TargetPoint,  // + range + ground position
    Toggle,       // on/off, periodic cost drain
};

AbilityForm parse_ability_form(const std::string& s);

// Visual shape of the cast indicator drawn during targeting / drag-cast
// — purely a UI concept, simulation does not branch on it. Only meaningful
// for `target_point` abilities; `target_unit` always has the implicit
// "circle around target unit" shape (driven by the level's optional
// `area.radius`). Defaults to Point, which draws no AoE indicator.
enum class IndicatorShape : u8 {
    Point = 0,    // no AoE; the drag-point reticle is the indicator
    Area,         // filled disc at the drag point; uses level.area.radius
    Line,         // rectangle from caster toward drag direction; uses level.area.width, length = level.range
    Cone,         // sector at caster toward drag direction; uses level.area.angle, length = level.range
};

IndicatorShape parse_indicator_shape(const std::string& s);

// Shape-specific size data on a level. Only one of `radius`/`width`/
// `angle` is meaningful per shape (per the table above); fields default
// to zero so a missing JSON key is harmless. `radius` doubles for both
// `target_unit` AoE-around-target and `target_point` shape=area.
struct AbilityArea {
    f32 radius = 0;
    f32 width  = 0;
    f32 angle  = 0;   // degrees
};

struct TargetFilter {
    bool ally   = false;
    bool enemy  = false;
    bool self_  = false;  // self_ to avoid keyword
    bool alive  = true;
    bool dead   = false;
    std::vector<std::string> classifications;
};

struct AbilityLevelDef {
    // Cost: state_name → amount
    std::map<std::string, f32> cost;
    bool cost_can_kill = false;

    f32 cooldown    = 0;
    f32 range       = 0;
    f32 cast_time    = 0;
    f32 backsw_time  = 0;
    f32 damage      = 0;
    f32 heal        = 0;

    // Modifiers (passive/applied): attr_name → value
    std::map<std::string, f32> modifiers;

    // Aura
    f32         aura_radius  = 0;
    std::string aura_ability;  // passive ability id to apply

    // Sustained-cast time (any form). Zero = instant resolution after
    // cast_time. Non-zero = the unit holds the cast for this many
    // seconds, interrupted by stun / move / damage per gameplay rules.
    f32 channel_time = 0;

    // Indicator-shape-specific size data; presence + meaning is driven
    // by the ability's `shape` (see `IndicatorShape`). Absent in JSON →
    // zeroed. Mirrors as `target_unit`'s AoE-around-target radius when
    // present on a unit-targeted ability.
    AbilityArea area;
    bool        has_area = false;

    // Toggle
    std::map<std::string, f32> toggle_cost_per_sec;

    // Projectile
    f32 projectile_speed = 0;
};

struct AbilityDef {
    std::string    id;
    std::string    name;
    std::string    icon;
    std::string    hotkey;              // RTS preset key (e.g., "T"). Empty = no hotkey.
    AbilityForm    form      = AbilityForm::Passive;
    IndicatorShape shape     = IndicatorShape::Point;  // UI only; only meaningful for target_point
    bool           stackable = false;
    bool           hidden    = false;   // hidden abilities don't auto-assign to slots
    u32            max_level  = 1;
    TargetFilter   target_filter;

    std::vector<AbilityLevelDef> levels;

    // Get level data (1-indexed, clamped)
    const AbilityLevelDef& level_data(u32 level) const {
        if (level == 0 || levels.empty()) {
            static AbilityLevelDef empty;
            return empty;
        }
        u32 idx = std::min(level, static_cast<u32>(levels.size())) - 1;
        return levels[idx];
    }
};

class AbilityRegistry {
public:
    bool load(asset::AssetManager& assets, std::string_view abs_path);

    const AbilityDef* get(std::string_view id) const;
    u32 count() const { return static_cast<u32>(m_defs.size()); }

private:
    bool load_from_doc(const asset::JsonDocument* doc, std::string_view source);
    std::unordered_map<std::string, AbilityDef> m_defs;
};

} // namespace uldum::simulation
