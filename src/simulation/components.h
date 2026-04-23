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

    // Sub-tick render interpolation. `alpha` in [0, 1] — callers should
    // use the same value the renderer passes into its per-frame draw so
    // everything tracking an entity (mesh, selection circle, HP bar,
    // world labels) projects through the exact same position.
    glm::vec3 interp_position(f32 alpha) const {
        return prev_position + (position - prev_position) * alpha;
    }
    // Shortest-path angle interpolation (handles 2π wrap).
    f32 interp_facing(f32 alpha) const {
        f32 diff = facing - prev_facing;
        constexpr f32 PI = 3.14159265358979f;
        while (diff >  PI) diff -= 2.0f * PI;
        while (diff < -PI) diff += 2.0f * PI;
        return prev_facing + diff * alpha;
    }
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

    // Re-path timer and destination tracking
    f32       repath_timer = 0;
    glm::vec2 path_dest{0};            // destination used for last pathfind (for repath detection)
    static constexpr f32 REPATH_INTERVAL = 1.5f;

    // Approach mode: set by combat/cast to request movement toward a target.
    // Movement system handles pathfinding + stepping; requester just checks distance.
    Unit      approach_target;          // entity to approach (dynamic position)
    glm::vec2 approach_goal{0};         // fixed position to approach (when no entity target)
    f32       approach_range = 0;       // stop when within this distance (0 = disabled)
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
    f32 sight_range = 1400;
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

// ── Inventory ─────────────────────────────────────────────────────────────
// Any unit can hold items if inventory_size > 0 in its type definition.

struct Inventory {
    std::vector<Item> slots;  // sized by inventory_size from unit type def
    u32 max_slots = 0;
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
    std::vector<glm::ivec2> blocked_vertices;  // vertex coords blocked at runtime
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
    bool        skip_birth = false;  // skip birth animation (entity revealed, not newly created)
};

} // namespace uldum::simulation
