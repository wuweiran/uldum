#pragma once

#include "simulation/handle_types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "simulation/order.h"

#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace uldum::simulation {

// ── All Game Objects ───────────────────────────────────────────────────────

struct Transform {
    glm::vec3 position{0.0f};
    f32       facing = 0.0f;   // radians around Y axis
    f32       scale  = 1.0f;
};

struct HandleInfo {
    std::string    type_id;     // references type definition (e.g. "footman")
    Category       category = Category::Unit;
    u32            generation = 0;
};

// ── Widget Components ──────────────────────────────────────────────────────

struct Health {
    f32       current     = 0;
    f32       max         = 0;
    f32       regen_per_sec = 0;
    ArmorType armor_type  = ArmorType::Unarmored;
    f32       armor       = 0;
};

struct Selectable {
    f32 selection_radius = 1.0f;
    i32 priority         = 5;
};

// ── Unit Components ────────────────────────────────────────────────────────

struct Owner {
    Player player;
};

struct Movement {
    f32       speed     = 0;
    f32       turn_rate = 0;
    MoveType  type      = MoveType::Ground;
    glm::vec3 target_pos{0.0f};
    bool      moving    = false;
};

struct Combat {
    f32        damage          = 0;
    f32        range           = 1.0f;
    f32        attack_cooldown = 1.0f;
    f32        cooldown_remaining = 0;
    AttackType attack_type     = AttackType::Normal;
    Unit       target;
};

struct Vision {
    f32 sight_range_day   = 1400;
    f32 sight_range_night = 800;
};

struct OrderQueue {
    std::optional<Order> current;
    std::deque<Order>    queued;
};

struct AbilityInstance {
    std::string ability_id;
    u32         level              = 1;
    f32         cooldown_remaining = 0;
    bool        auto_cast          = false;
    bool        toggle_active      = false;
};

struct AbilitySet {
    std::vector<AbilityInstance> abilities;
};

struct BuffInstance {
    std::string buff_id;
    Unit        source;
    f32         remaining_duration = 0;
    f32         tick_timer         = 0;
};

struct BuffList {
    std::vector<BuffInstance> active;
};

struct UnitClassificationComp {
    Classification flags = Classification::None;
};

// ── Hero Components ────────────────────────────────────────────────────────

struct HeroComp {
    u32       level = 1;
    u32       xp    = 0;
    u32       xp_to_next = 200;
    f32       str = 0, agi = 0, int_ = 0;
    f32       str_per_level = 0, agi_per_level = 0, int_per_level = 0;
    Attribute primary_attr = Attribute::Str;
};

struct Inventory {
    std::array<Item, 6> slots{};
};

// ── Building Components ────────────────────────────────────────────────────

struct TrainOrder {
    std::string unit_type_id;
    f32         progress = 0;
    f32         total_time = 0;
};

struct BuildingComp {
    std::deque<TrainOrder>     train_queue;
    std::vector<std::string>   researched;
};

struct Construction {
    f32  build_progress   = 0;
    bool under_construction = false;
    f32  build_time_total = 0;
};

// ── Destructable Components ────────────────────────────────────────────────

struct DestructableComp {
    std::string type_id;
    u8          variation = 0;
};

struct PathingBlocker {
    std::vector<glm::ivec2> blocked_tiles;
};

// ── Item Components ────────────────────────────────────────────────────────

struct ItemInfo {
    std::string type_id;
    i32         charges = -1;    // -1 = unlimited
    f32         cooldown = 0;
    f32         cooldown_remaining = 0;
};

struct Carriable {
    Unit carried_by;
};

// ── Projectile Component ───────────────────────────────────────────────────

struct ProjectileComp {
    Unit        source;
    Unit        target;
    glm::vec3   target_pos{0.0f};
    f32         speed     = 0;
    f32         damage    = 0;
    std::string source_ability;
    bool        homing    = false;
};

// ── Rendering ──────────────────────────────────────────────────────────────

struct Renderable {
    std::string model_path;
    // AnimationState will be added in render phase
    bool        visible = true;
};

} // namespace uldum::simulation
