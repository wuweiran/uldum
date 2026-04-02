#pragma once

#include "core/types.h"

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace uldum::asset { class AssetManager; struct JsonDocument; }

namespace uldum::simulation {

// Ability forms — engine-provided mechanical primitives.
enum class AbilityForm : u8 {
    Passive,      // always active, modifiers + optional duration
    Aura,         // scan radius, apply/remove passive to nearby units
    Instant,      // cast point → fire → backswing
    TargetUnit,   // + range + target validation + optional projectile
    TargetPoint,  // + range + ground position
    Toggle,       // on/off, periodic cost drain
    Channel,      // sustained cast for duration, interrupted by stun/move
};

AbilityForm parse_ability_form(const std::string& s);

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
    f32 cast_point  = 0;
    f32 backswing   = 0;
    f32 damage      = 0;
    f32 heal        = 0;

    // Modifiers (passive/applied): attr_name → value
    std::map<std::string, f32> modifiers;

    // Aura
    f32         aura_radius  = 0;
    std::string aura_ability;  // passive ability id to apply

    // Channel
    f32 channel_duration = 0;

    // Toggle
    std::map<std::string, f32> toggle_cost_per_sec;

    // Projectile
    f32 projectile_speed = 0;
};

struct AbilityDef {
    std::string  id;
    std::string  name;
    std::string  icon;
    AbilityForm  form      = AbilityForm::Passive;
    bool         stackable = false;
    u32          max_level  = 1;
    TargetFilter target_filter;

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
