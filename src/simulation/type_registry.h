#pragma once

#include "simulation/entity_types.h"
#include "simulation/components.h"

#include <map>
#include <optional>
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
    f32      fly_height = 0.0f;   // Air units: render-Z lift above ground (visual only)

    // Pathing footprint in tiles. Buildings declare this (typically
    // 2×2, 3×3, 4×4); mobile units leave it 0×0. The footprint is the
    // tile region the building physically occupies — both for runtime
    // pathing blocks and for placement snapping (odd extent → tile
    // center, even extent → tile corner). Independent of
    // collision_radius, which still drives unit physics and attack
    // range. Mirrors WC3's split between a Pathing Map and a
    // Collision Size, simplified to a rectangle for v1.
    u32 pathing_footprint_w = 0;
    u32 pathing_footprint_h = 0;

    // Weapon — the design-time attack spec. "weapon" is the def-side
    // vocabulary; at instantiation it becomes the runtime `Combat`
    struct WeaponDef {
        f32 damage = 10;
        f32 attack_range = 1.0f;
        f32 attack_cooldown = 1.0f;
        f32 dmg_time = 0.3f;         // seconds: fore-swing before damage (JSON: weapon.dmg_time)
        f32 backsw_time = 0.3f;      // seconds: backswing after damage (JSON: weapon.backsw_time)
        std::optional<ProjectileSpec> projectile;  // set → ranged auto-attack; unset → melee
        u8  target_mask = TARGET_MASK_SURFACE;      // which MoveType layers this attack can hit (JSON: weapon.targets)
    };
    std::optional<WeaponDef> weapon;

    // Acquisition range is unit-level, not per-weapon (mirrors WC3's
    // Acquisition Range): how far the unit scans for enemies to auto-engage.
    f32 acquire_range = 10.0f;

    // Animation (JSON: animation section)
    f32 dmg_pt = 0.5f;            // fraction of attack animation at damage point
    f32 cast_pt = 0.5f;           // fraction of spell animation at cast point
    f32 walk_speed = 0;            // movement speed at which walk anim plays at 1x (0 = use move_speed)

    // Vision
    f32 sight_range = 1400;

    // Selection. radius/height ≤ 0 = AUTO (derive the click cylinder from the
    // model's AABB at load). A units.json "selection" block overrides both
    // (all-or-nothing). priority is independent and always honored.
    f32 selection_radius = 0.0f;   // 0 = auto from model footprint
    f32 selection_height = 0.0f;   // 0 = auto from model height
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

    // Inventory (0 = cannot hold items)
    u32 inventory_size = 0;

    // Sounds (triggered by animation events)
    std::string sound_attack;  // played on attack damage point
    std::string sound_death;   // played on death
    std::string sound_birth;   // played on spawn

    // Building (if "structure" classification set)
    f32 build_time = 0;
};

struct DestructableTypeDef {
    std::string              id;
    std::vector<std::string> models;        // one entry per visual variation; length = variation count
    f32                      model_scale = 1.0f;            // uniform render scale
    f32                      max_health = 50;
    std::map<std::string, f32>         attributes_numeric;  // "armor" → 0
    std::map<std::string, std::string> attributes_string;   // "armor_type" → "fortified"
    u32                      pathing_footprint_w = 0;       // tiles; 0 = no pathing block
    u32                      pathing_footprint_h = 0;
    std::vector<std::string> targeted_as;                   // WC3 "Targeted As" tags ("structure", "tree", "debris", …)
    u8                       target_bit = TARGET_BIT_DEBRIS;  // derived from targeted_as (attack handshake)
    bool                     selectable = true;             // left-clickable? auto-false for "tree", JSON-overridable
};

// Pure-decoration object: no health, no collision, no pathing block.
// Only the model + variation + scale; loader is intentionally minimal.
struct DoodadTypeDef {
    std::string              id;
    std::vector<std::string> models;
    f32                      model_scale = 1.0f;
};

// WC3-style item class — the single field that drives engine item behavior.
// Permanent: sits in a slot, grants its abilities while carried (default).
// Charged: sits in a slot, grants its use ability; the engine spends 1 charge
//   when that ability's effect fires and destroys the item at 0.
// Powerup: consumed on pickup (no slot needed, works at full inventory); the
//   engine grants nothing and casts nothing — map Lua applies the effect via
//   the pickup event. `abilities` is ignored for powerups.
enum class ItemClass : u8 { Permanent, Charged, Powerup };

inline ItemClass parse_item_class(std::string_view s) {
    if (s == "charged") return ItemClass::Charged;
    if (s == "powerup") return ItemClass::Powerup;
    return ItemClass::Permanent;
}

struct ItemTypeDef {
    std::string              id;
    std::string              display_name;
    // Player-facing description shown in the inventory-slot tooltip.
    // Defaults to "" when unauthored — same shape as any optional
    // numeric field that defaults via `val.value(field, default)` in
    // the loader.
    std::string              tooltip;
    std::string              icon_path;       // HUD slot icon (KTX2)
    std::string              model_path;      // ground render model (glTF)
    f32                      model_scale  = 1.0f;
    ItemClass                item_class   = ItemClass::Permanent;
    // Abilities granted to the carrier while the item is in inventory.
    // Activeness is implicit — if abilities[0].form is non-passive, the
    // inventory slot fires that ability on click/hotkey; otherwise the
    // slot is inert (passive item). Subsequent abilities still apply
    // their effects (modifiers / auras) but never fire.
    std::vector<std::string> abilities;
    // Charged-class starting charge count. Meaningless (and warned) on
    // other classes. `initial_level` is a free integer the engine renders
    // (badge) but never interprets.
    i32                      initial_charges = 0;
    i32                      initial_level   = 0;
};

class TypeRegistry {
public:
    // Load types from a path relative to engine root.
    bool load_unit_types(asset::AssetManager& assets, std::string_view path);
    bool load_destructable_types(asset::AssetManager& assets, std::string_view path);
    bool load_doodad_types(asset::AssetManager& assets, std::string_view path);
    bool load_item_types(asset::AssetManager& assets, std::string_view path);

    // Load types from an absolute path (for map files).
    bool load_unit_types_absolute(asset::AssetManager& assets, std::string_view abs_path);
    bool load_destructable_types_absolute(asset::AssetManager& assets, std::string_view abs_path);
    bool load_doodad_types_absolute(asset::AssetManager& assets, std::string_view abs_path);
    bool load_item_types_absolute(asset::AssetManager& assets, std::string_view abs_path);

    const UnitTypeDef*          get_unit_type(std::string_view id) const;
    const DestructableTypeDef*  get_destructable_type(std::string_view id) const;
    const DoodadTypeDef*        get_doodad_type(std::string_view id) const;
    const ItemTypeDef*          get_item_type(std::string_view id) const;

    u32 unit_type_count() const { return static_cast<u32>(m_unit_types.size()); }
    u32 destructable_type_count() const { return static_cast<u32>(m_destructable_types.size()); }
    u32 doodad_type_count() const { return static_cast<u32>(m_doodad_types.size()); }
    u32 item_type_count() const { return static_cast<u32>(m_item_types.size()); }

    // Read-only iteration over loaded types — used by the editor's
    // Place-mode type pickers to build dropdowns.
    const std::unordered_map<std::string, UnitTypeDef>&         unit_types()         const { return m_unit_types; }
    const std::unordered_map<std::string, DestructableTypeDef>& destructable_types() const { return m_destructable_types; }
    const std::unordered_map<std::string, DoodadTypeDef>&       doodad_types()       const { return m_doodad_types; }
    const std::unordered_map<std::string, ItemTypeDef>&         item_types()         const { return m_item_types; }

    // Drop every loaded type so the next map starts with an empty
    // registry. Without this, types declared by a previous map's
    // JSON linger and shadow new maps that omit them.
    void clear() {
        m_unit_types.clear();
        m_destructable_types.clear();
        m_doodad_types.clear();
        m_item_types.clear();
        m_raw_units.clear();
        m_raw_destructables.clear();
        m_raw_doodads.clear();
        m_raw_items.clear();
    }

    // Raw string-field cache populated alongside the structured load.
    // Backs the i18n raw-fallback chain — when a locale pack misses a
    // key like `unit.footman.description`, the resolver reads the field
    // directly from this cache (which mirrors the JSON's string-typed
    // top-level fields for each entity). Returns nullopt if the entity
    // id or field isn't present.
    enum class Category : u8 { Unit, Destructable, Doodad, Item };
    std::optional<std::string> raw_string_field(Category cat,
                                                  std::string_view entity_id,
                                                  std::string_view field) const;

private:
    bool load_unit_types_from_doc(const asset::JsonDocument* doc, std::string_view source);
    bool load_destructable_types_from_doc(const asset::JsonDocument* doc, std::string_view source);
    bool load_doodad_types_from_doc(const asset::JsonDocument* doc, std::string_view source);
    bool load_item_types_from_doc(const asset::JsonDocument* doc, std::string_view source);

    using RawFields = std::unordered_map<std::string, std::string>;
    using RawCache  = std::unordered_map<std::string, RawFields>;

    std::unordered_map<std::string, UnitTypeDef>          m_unit_types;
    std::unordered_map<std::string, DestructableTypeDef>  m_destructable_types;
    std::unordered_map<std::string, DoodadTypeDef>        m_doodad_types;
    std::unordered_map<std::string, ItemTypeDef>          m_item_types;

    RawCache m_raw_units;
    RawCache m_raw_destructables;
    RawCache m_raw_doodads;
    RawCache m_raw_items;
};

} // namespace uldum::simulation
