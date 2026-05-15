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
//
// "Target" subsumes WC3's TargetUnit / TargetPoint / TargetDestructable /
// TargetItem split. The single form covers all "cast on a thing in the
// world" shapes; `widget_kinds` + `accept_point` on AbilityDef pick
// which kinds the cursor accepts.
enum class AbilityForm : u8 {
    PassiveModifier,  // always active, contributes numeric modifiers
    PassiveFlag,      // always active, applies refcounted status flags
    Aura,             // scan radius, apply/remove a passive_* buff to nearby units
    Instant,          // cast_time → fire → backsw_time, no target
    Target,           // cast on a widget and/or a point — see widget_kinds + accept_point
};

AbilityForm parse_ability_form(const std::string& s);

// Bitmask values for `AbilityDef::widget_kinds`. A Target-form ability
// accepts a widget pick when the widget's category bit is set; otherwise
// the cursor falls through to the ground point (if `accept_point` is on).
namespace widget_kind {
    constexpr u32 Unit         = 1u << 0;
    constexpr u32 Destructable = 1u << 1;
    constexpr u32 Item         = 1u << 2;
}

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

    // Modifiers (passive_modifier): bare attr_name → additive value
    // (e.g. "armor": 5). Suffix `_mult` switches to multiplicative
    // unit-fraction (e.g. "move_speed_mult": -0.5 = -50% speed).
    std::map<std::string, f32> modifiers;

    // Refcounted status flags (passive_flag). Each name maps to a bit in
    // status:: (see components.h). While the instance is alive, each
    // flag's refcount is incremented; on remove, decremented.
    std::vector<std::string> flags;

    // -1 = permanent (default for innate passives). >= 0 = timed; the
    // instance is auto-removed when remaining_duration hits 0.
    f32 duration = -1.0f;

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

    // Projectile
    f32 projectile_speed = 0;
};

struct AbilityDef {
    std::string    id;
    std::string    name;
    std::string    icon;
    std::string    hotkey;              // RTS preset key (e.g., "T"). Empty = no hotkey.
    AbilityForm    form      = AbilityForm::PassiveModifier;
    IndicatorShape shape     = IndicatorShape::Point;  // UI only; only meaningful for target_point
    bool           stackable = false;
    bool           hidden    = false;   // hidden abilities don't auto-assign to slots
    // Bypass UNIT_STATUS_MAGIC_IMMUNE on the target. Used by dispels
    // / purges / hexes that are specifically designed to land on
    // magic-immune units. Default false. Has no effect on
    // UNIT_STATUS_UNTARGETABLE — nothing pierces that.
    bool           pierces_immune = false;
    u32            max_level  = 1;
    TargetFilter   target_filter;

    // Target-form metadata. `widget_kinds` is a bitmask of widget_kind::*
    // values (Unit / Destructable / Item) the cursor will snap to.
    // `accept_point` lets the cursor fall through to the ground when no
    // accepted widget is under it. Three resulting shapes:
    //   • widget-only:        widget_kinds != 0, accept_point = false
    //   • point-only:         widget_kinds == 0, accept_point = true
    //   • hybrid (widget-first, point fallback): widget_kinds != 0,
    //                                            accept_point = true
    // Ignored when `form` isn't Target.
    u32  widget_kinds = 0;
    bool accept_point = false;

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

    // Drop every registered ability. Mirrors TypeRegistry::clear so
    // session shutdown leaves a fresh registry for the next map.
    void clear() { m_defs.clear(); }

private:
    bool load_from_doc(const asset::JsonDocument* doc, std::string_view source);
    std::unordered_map<std::string, AbilityDef> m_defs;
};

} // namespace uldum::simulation
