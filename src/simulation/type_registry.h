#pragma once

#include "simulation/handle_types.h"

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace uldum::asset { class AssetManager; struct JsonDocument; }

namespace uldum::simulation {

struct UnitTypeDef {
    std::string id;
    std::string display_name;
    std::string model_path;
    f32 model_scale = 1.0f;

    // Health (engine built-in state)
    f32 max_health = 100;
    f32 health_regen = 0;

    // Movement
    f32      move_speed = 270;
    f32      turn_rate = 0.6f;
    f32      collision_radius = 32.0f;
    MoveType move_type = MoveType::Ground;

    // Combat
    f32 damage = 10;
    f32 attack_range = 1.0f;
    f32 attack_cooldown = 1.0f;
    f32 cast_point = 0.3f;
    f32 backswing = 0.3f;
    bool is_ranged = false;
    f32 projectile_speed = 20.0f;
    f32 acquire_range = 10.0f;

    // Vision
    f32 sight_range_day = 1400;
    f32 sight_range_night = 800;

    // Selection
    f32 selection_radius = 1.0f;
    i32 selection_priority = 5;

    // Classification — map-defined string flags
    std::vector<std::string> classifications;

    // Abilities
    std::vector<std::string> abilities;

    // Map-defined states beyond HP (e.g. "mana" → {max, regen})
    struct StateDef { f32 max = 0; f32 regen = 0; };
    std::map<std::string, StateDef> states;

    // Map-defined attributes (numeric: "armor" → 5.0, string: "armor_type" → "heavy")
    std::map<std::string, f32>         attributes_numeric;
    std::map<std::string, std::string> attributes_string;

    // Hero (if "hero" classification set)
    std::string                  hero_primary_attr;
    std::map<std::string, f32>   hero_base_attrs;      // "strength" → 22.0
    std::map<std::string, f32>   hero_attr_per_level;   // "strength" → 2.7

    // Sounds (triggered by animation events)
    std::string sound_attack;  // played on attack damage point
    std::string sound_death;   // played on death
    std::string sound_birth;   // played on spawn

    // Building (if "structure" classification set)
    f32 build_time = 0;
};

struct DestructableTypeDef {
    std::string id;
    std::string display_name;
    std::string model_path;
    f32         max_health = 50;
    std::map<std::string, f32>         attributes_numeric;  // "armor" → 0
    std::map<std::string, std::string> attributes_string;   // "armor_type" → "fortified"
    u8          variations = 1;
};

struct ItemTypeDef {
    std::string id;
    std::string display_name;
    std::string icon_path;
    i32         charges = 1;
    f32         cooldown = 0;
    i32         gold_cost = 0;
};

class TypeRegistry {
public:
    // Load types from a path relative to engine root.
    bool load_unit_types(asset::AssetManager& assets, std::string_view path);
    bool load_destructable_types(asset::AssetManager& assets, std::string_view path);
    bool load_item_types(asset::AssetManager& assets, std::string_view path);

    // Load types from an absolute path (for map files).
    bool load_unit_types_absolute(asset::AssetManager& assets, std::string_view abs_path);
    bool load_destructable_types_absolute(asset::AssetManager& assets, std::string_view abs_path);
    bool load_item_types_absolute(asset::AssetManager& assets, std::string_view abs_path);

    const UnitTypeDef*          get_unit_type(std::string_view id) const;
    const DestructableTypeDef*  get_destructable_type(std::string_view id) const;
    const ItemTypeDef*          get_item_type(std::string_view id) const;

    u32 unit_type_count() const { return static_cast<u32>(m_unit_types.size()); }
    u32 destructable_type_count() const { return static_cast<u32>(m_destructable_types.size()); }
    u32 item_type_count() const { return static_cast<u32>(m_item_types.size()); }

private:
    bool load_unit_types_from_doc(const asset::JsonDocument* doc, std::string_view source);
    bool load_destructable_types_from_doc(const asset::JsonDocument* doc, std::string_view source);
    bool load_item_types_from_doc(const asset::JsonDocument* doc, std::string_view source);

    std::unordered_map<std::string, UnitTypeDef>          m_unit_types;
    std::unordered_map<std::string, DestructableTypeDef>  m_destructable_types;
    std::unordered_map<std::string, ItemTypeDef>          m_item_types;
};

} // namespace uldum::simulation
