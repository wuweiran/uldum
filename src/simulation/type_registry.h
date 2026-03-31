#pragma once

#include "simulation/handle_types.h"

#include <string>
#include <string_view>
#include <unordered_map>

namespace uldum::asset { class AssetManager; }

namespace uldum::simulation {

struct UnitTypeDef {
    std::string id;
    std::string display_name;
    std::string model_path;

    // Health
    f32       max_health = 100;
    f32       health_regen = 0;
    f32       armor = 0;
    ArmorType armor_type = ArmorType::Unarmored;

    // Movement
    f32      move_speed = 270;
    f32      turn_rate = 0.6f;
    MoveType move_type = MoveType::Ground;

    // Combat
    f32        damage = 10;
    f32        attack_range = 1.0f;
    f32        attack_cooldown = 1.0f;
    AttackType attack_type = AttackType::Normal;

    // Vision
    f32 sight_range_day = 1400;
    f32 sight_range_night = 800;

    // Selection
    f32 selection_radius = 1.0f;
    i32 selection_priority = 5;

    // Classification
    Classification classifications = Classification::Ground;

    // Abilities
    std::vector<std::string> abilities;

    // Hero (if hero flag set)
    f32       hero_str = 0, hero_agi = 0, hero_int = 0;
    f32       hero_str_per_level = 0, hero_agi_per_level = 0, hero_int_per_level = 0;
    Attribute hero_primary_attr = Attribute::Str;

    // Building (if structure flag set)
    f32 build_time = 0;
};

struct DestructableTypeDef {
    std::string id;
    std::string display_name;
    std::string model_path;
    f32         max_health = 50;
    f32         armor = 0;
    ArmorType   armor_type = ArmorType::Fortified;
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
    bool load_unit_types(asset::AssetManager& assets, std::string_view path);
    bool load_destructable_types(asset::AssetManager& assets, std::string_view path);
    bool load_item_types(asset::AssetManager& assets, std::string_view path);

    const UnitTypeDef*          get_unit_type(std::string_view id) const;
    const DestructableTypeDef*  get_destructable_type(std::string_view id) const;
    const ItemTypeDef*          get_item_type(std::string_view id) const;

    u32 unit_type_count() const { return static_cast<u32>(m_unit_types.size()); }
    u32 destructable_type_count() const { return static_cast<u32>(m_destructable_types.size()); }
    u32 item_type_count() const { return static_cast<u32>(m_item_types.size()); }

private:
    std::unordered_map<std::string, UnitTypeDef>          m_unit_types;
    std::unordered_map<std::string, DestructableTypeDef>  m_destructable_types;
    std::unordered_map<std::string, ItemTypeDef>          m_item_types;
};

} // namespace uldum::simulation
