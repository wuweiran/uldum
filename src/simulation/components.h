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
    f32       facing = 0.0f;   // radians around Z axis (up). 0 = facing +X (WC3 convention)
    f32       scale  = 1.0f;
    // Previous tick state for render interpolation
    glm::vec3 prev_position{0.0f};
    f32       prev_facing = 0.0f;
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
    f32 selection_height = 100.0f;  // height of selection cylinder (world units)
    i32 priority         = 5;
};

// ── Unit Components ────────────────────────────────────────────────────────

struct Owner {
    Player player;
};

struct Movement {
    f32       speed     = 0;
    f32       turn_rate = 0;  // radians per second
    f32       collision_radius = 32.0f;  // per-unit collision radius (game units)
    MoveType  type      = MoveType::Ground;  // engine preset (pathfinding needs it)
    u8        cliff_level = 0;  // current cliff level the unit is on
    bool      moving    = false;

    // Corridor from A* (tile path) and current straight-line waypoint
    std::vector<glm::ivec2> corridor;  // tile coordinates
    glm::vec2 waypoint{0};             // current straight-line target (world XY)
    bool      has_waypoint = false;

    // Re-path timer: re-compute corridor periodically
    f32 repath_timer = 0;
    static constexpr f32 REPATH_INTERVAL = 1.5f;
};

enum class AttackState : u8 { Idle, MovingToTarget, TurningToFace, WindUp, Backswing, Cooldown };

struct Combat {
    f32         damage          = 0;
    f32         range           = 1.0f;
    f32         attack_cooldown = 1.0f;    // total time between attacks
    f32         dmg_time        = 0.3f;    // seconds: fore-swing before damage
    f32         backsw_time     = 0.3f;    // seconds: backswing after damage
    f32         dmg_pt          = 0.5f;    // fraction of attack animation at damage point
    bool        is_ranged       = false;
    f32         projectile_speed = 20.0f;  // for ranged attacks
    f32         acquire_range   = 10.0f;   // auto-attack enemy acquisition range
    // Runtime state
    AttackState attack_state    = AttackState::Idle;
    f32         attack_timer    = 0;
    Unit        target;
    glm::vec3   chase_path_dest{0};  // last pathfind destination for re-path detection
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

enum class CastState : u8 { None, MovingToTarget, TurningToFace, CastPoint, Backswing };

struct AbilitySet {
    std::vector<AbilityInstance> abilities;
    // All ability types live here: active, passive, auras, and applied abilities
    // (what WC3 calls "buffs"). No separate component for any of these.

    // Cast state machine — active while processing a Cast order
    CastState   cast_state    = CastState::None;
    f32         cast_timer    = 0;
    f32         cast_point_secs = 0;  // cached for renderer: seconds for wind-up phase
    f32         cast_backswing_secs = 0; // cached for renderer: seconds for backswing phase
    std::string casting_id;       // ability being cast
    Unit        cast_target_unit;
    glm::vec3   cast_target_pos{0};
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
