#pragma once

#include "simulation/handle_types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "simulation/order.h"

#include <array>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace uldum::simulation {

// ── All Game Objects ───────────────────────────────────────────────────────

struct Transform {
    glm::vec3 position{0.0f};
    f32       facing = 0.0f;   // radians around Z axis (up). 0 = facing +Y
    f32       scale  = 1.0f;
};

struct HandleInfo {
    std::string    type_id;     // references type definition (e.g. "footman")
    Category       category = Category::Unit;
    u32            generation = 0;
};

// ── Widget Components ──────────────────────────────────────────────────────

// HP is engine-built-in (engine fires death event when current reaches 0).
struct Health {
    f32 current     = 0;
    f32 max         = 0;
    f32 regen_per_sec = 0;
};

// Map-defined states beyond HP (mana, energy, rage, etc.).
struct StateValue {
    f32 current     = 0;
    f32 max         = 0;
    f32 regen_per_sec = 0;
};

struct StateBlock {
    std::map<std::string, StateValue> states;
};

// Map-defined attributes (strength, agility, armor, armor_type, attack_type, etc.).
// Single values that don't deplete or regenerate.
struct AttributeBlock {
    std::map<std::string, f32>         numeric;   // "armor" → 5.0, "strength" → 22.0
    std::map<std::string, std::string> string_attrs; // "armor_type" → "heavy", "attack_type" → "normal"
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
    f32       turn_rate = 0;  // radians per second
    MoveType  type      = MoveType::Ground;  // engine preset (pathfinding needs it)
    glm::vec3 target_pos{0.0f};
    bool      moving    = false;

    // Current path (set by MovementSystem when processing Move orders)
    std::vector<glm::vec3> path;
    u32 path_index = 0;  // current waypoint index
};

enum class AttackState : u8 { Idle, MovingToTarget, TurningToFace, WindUp, Backswing, Cooldown };

struct Combat {
    f32         damage          = 0;
    f32         range           = 1.0f;
    f32         attack_cooldown = 1.0f;    // total time between attacks
    f32         cast_point      = 0.3f;    // time into attack when damage fires
    f32         backswing       = 0.3f;    // recovery time after cast_point
    bool        is_ranged       = false;
    f32         projectile_speed = 20.0f;  // for ranged attacks
    // Runtime state
    AttackState attack_state    = AttackState::Idle;
    f32         attack_timer    = 0;
    Unit        target;
};

// Visual feedback: scale pulse on attack hit, decays back to 1.0
struct ScalePulse {
    f32 current_scale = 1.0f;
    f32 timer         = 0;
};

// Dead unit state — unit becomes a corpse, then eventually gets cleaned up.
struct DeadState {
    f32 corpse_timer   = 0;     // time since death
    f32 corpse_duration = 8.0f; // seconds corpse remains visible
    f32 cleanup_delay  = 30.0f; // seconds before entity is fully destroyed
    bool corpse_visible = true;  // false after corpse_duration expires
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
    // Applied ability fields (for WC3 "buffs")
    Unit        source;                      // unit that applied this (null if self/innate)
    f32         remaining_duration = -1.0f;  // -1 = permanent (innate), >= 0 = timed
    f32         tick_timer         = 0;
    // Active modifiers from this ability's current level
    std::map<std::string, f32> active_modifiers;
};

struct AbilitySet {
    std::vector<AbilityInstance> abilities;
    // All ability types live here: active, passive, auras, and applied abilities
    // (what WC3 calls "buffs"). No separate component for any of these.
};

// Map-defined classification flags (e.g., "ground", "air", "hero", "structure").
struct UnitClassificationComp {
    std::vector<std::string> flags;
};

// ── Hero Components ────────────────────────────────────────────────────────

struct HeroComp {
    u32         level = 1;
    u32         xp    = 0;
    u32         xp_to_next = 200;
    std::string primary_attr;  // map-defined attribute name (e.g. "strength")
    // Per-level attribute growth — attribute name → growth per level
    std::map<std::string, f32> attr_per_level;
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
    bool        visible = true;
};

} // namespace uldum::simulation
