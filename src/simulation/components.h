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
    u32 hit_count     = 0;   // bumped per normal-attack hit; renderer plays "hit" on change
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
//
// `numeric` is the EFFECTIVE value seen by every reader (combat,
// scripts, HUD). `base` is the value before passive-ability modifiers
// are summed in. recalculate_modifiers rebuilds numeric from base +
// the sum of active_modifiers across the unit's ability set. Lua's
// SetUnitAttribute writes to base; modifiers from passives stack on
// top.
struct AttributeBlock {
    std::map<std::string, f32>         base;      // base values (pre-modifier)
    std::map<std::string, f32>         numeric;   // effective: "armor" → 5.0, "strength" → 22.0
    std::map<std::string, std::string> string_attrs; // "armor_type" → "heavy", "attack_type" → "normal"
};

struct Selectable {
    f32 selection_radius = 0.0f;    // 0 = auto (renderer fills from model AABB × scale)
    f32 selection_height = 0.0f;    // 0 = auto (renderer fills from model AABB × scale)
    i32 priority         = 5;
};

// ── Unit Components ────────────────────────────────────────────────────────

struct Movement {
    f32       speed     = 0;
    f32       turn_rate = 0;  // radians per second
    f32       collision_radius = 32.0f;  // per-unit collision radius (game units)
    MoveType  type      = MoveType::Ground;  // engine preset (pathfinding needs it)
    u8        cliff_level = 0;  // current cliff level the unit is on
    bool      moving    = false;

    // Corridor from A* (cell path) and current straight-line waypoint.
    // Stored in pathing-cell coordinates (terrain tile × PATHING_SUBDIV);
    // find_straight_waypoint consumes these cell centers.
    std::vector<glm::ivec2> corridor;
    glm::vec2 waypoint{0};             // current straight-line target (world XY)
    bool      has_waypoint = false;

    // Transient local-avoidance splice. When the straight line to `waypoint`
    // is blocked by a hard obstacle the A* corridor didn't know about (a
    // foreign unit, or terrain the string-pull grazes), local steering sets
    // ONE detour point to walk toward instead. It's ephemeral: consumed on
    // arrival, dropped the moment the corridor clears again, and always
    // wiped on repath (A* owns the real route; this just survives until the
    // next replan). has_detour=false → steer straight to `waypoint`.
    glm::vec2 detour{0};
    bool      has_detour = false;

    // Re-path timer and destination tracking
    f32       repath_timer = 0;
    glm::vec2 path_dest{0};            // destination used for last pathfind (for repath detection)
    static constexpr f32 REPATH_INTERVAL = 1.5f;

    // Approach mode: set by combat/cast to request movement toward a target.
    // Movement system handles pathfinding + stepping; requester just checks distance.
    Unit      approach_target;          // entity to approach (dynamic position)
    glm::vec2 approach_goal{0};         // fixed position to approach (when no entity target)
    f32       approach_range = 0;       // stop when within this distance (0 = disabled)

    // Stuck-timeout state for the unified Move order. Tracks how long
    // the unit has been failing to make forward progress while a Move
    // order is active. Reset every time real progress happens (>=
    // STUCK_PROGRESS_EPS world units between ticks). When the timer
    // exceeds STUCK_TIMEOUT and the order's `range == 0`, the order
    // self-terminates — that's what unjams pile-ups around a single
    // click point. Follow orders (range > 0 + target_unit) never
    // stuck-terminate; the player explicitly asked us to keep trying.
    f32       stuck_timer       = 0;
    glm::vec2 stuck_anchor{0};          // last position from which we measure progress
    static constexpr f32 STUCK_TIMEOUT     = 3.0f;   // seconds without progress → give up
                                                     // (3s = two repath attempts at 1.5s interval before quitting)
    static constexpr f32 STUCK_PROGRESS_EPS = 4.0f;  // world units of motion that count as progress
};

enum class AttackState : u8 { Idle, MovingToTarget, TurningToFace, WindUp, Backswing, Cooldown };

// Ranged auto-attack delivery. Presence of this on a Combat means the
// attack fires a missile instead of dealing melee damage instantly.
struct ProjectileSpec {
    std::string model;             // model path ("" = placeholder mesh)
    f32         speed = 20.0f;     // travel speed (game units/sec)
    f32         arc   = 0;         // ballistic arc peak height (game units); 0 = flat shot
    // Spawn offset in the attacker's facing frame, in MODEL-LOCAL units
    // (multiplied by the source's render scale at spawn — author once, it
    // tracks any model_scale). x=forward along facing, y=lateral (right),
    // z=height above feet. Lets the arrow leave the bow, not the ground.
    glm::vec3   launch{0.0f};
};

struct Combat {
    f32         damage          = 0;
    f32         range           = 1.0f;
    f32         attack_cooldown = 1.0f;    // total time between attacks
    f32         dmg_time        = 0.3f;    // seconds: fore-swing before damage
    f32         backsw_time     = 0.3f;    // seconds: backswing after damage
    f32         dmg_pt          = 0.5f;    // fraction of attack animation at damage point
    std::optional<ProjectileSpec> projectile;  // set → ranged; unset → melee
    f32         acquire_range   = 10.0f;   // auto-attack enemy acquisition range
    u8          target_mask     = TARGET_MASK_SURFACE;  // which MoveType layers this attack can hit
    // Runtime state
    AttackState attack_state    = AttackState::Idle;
    f32         attack_timer    = 0;
    Unit        target;
};

// Dead unit state — unit becomes a corpse, then eventually gets cleaned up.
struct DeadState {
    f32 corpse_timer   = 0;     // time since death
    f32 corpse_duration = 8.0f; // seconds corpse remains visible
    f32 cleanup_delay  = 30.0f; // seconds before entity is fully destroyed
    bool corpse_visible = true;  // false after corpse_duration expires
};

// Per-unit sight radius. The Vision subsystem (see vision.h) stamps
// fog tiles from each Sight component's range every tick.
struct Sight {
    f32 sight_range = 1400;
    // Per-unit vision share. UnitShareVision(unit, otherPlayer, true)
    // pushes otherPlayer.id here so the unit's sight contributes to
    // that player's fog map too.
    std::vector<u32> share_to_players;
};

struct OrderQueue {
    std::optional<Order> current;
    std::deque<Order>    queued;

    // Finish the current order and promote the next queued one (if any)
    // into `current`. The "reset current, pop queued front" pair was
    // hand-written at every order-completion site; routing them through
    // one method keeps the two halves from drifting (e.g. resetting
    // without promoting, or promoting without clearing first).
    void advance() {
        if (!queued.empty()) {
            current = std::move(queued.front());
            queued.pop_front();
        } else {
            current.reset();
        }
    }
};

struct AbilityInstance {
    std::string ability_id;
    u32         level              = 1;
    f32         cooldown_remaining = 0;
    bool        auto_cast          = false;
    // Set when this ability was granted to the carrier by an item pickup
    // (give_item_to_unit). The action_bar filters these out so item
    // ability icons don't compete with the unit's intrinsic abilities —
    // items are surfaced through the inventory composite instead.
    bool        from_item          = false;
    // Applied ability fields (for WC3 "buffs")
    Unit        source;                      // unit that applied this (null if self/innate)
    f32         remaining_duration = -1.0f;  // -1 = permanent (innate), >= 0 = timed
    f32         tick_timer         = 0;
    // Active modifiers from this ability's current level (passive_modifier)
    std::map<std::string, f32> active_modifiers;
    // Status flags this instance contributes (passive_flag) — each name is
    // a key into status::; while the instance lives, each flag's refcount
    // on the carrier is incremented.
    std::vector<std::string> active_flags;
};

// Cast state machine. A Cast order steps through these in order:
//   None → MovingToTarget* → TurningToFace → Foreswing → (Channeling*) → Backswing → None
// Starred states are skipped when not applicable (no range, no channel_time).
// The effect fires at Foreswing → next-state transition.
enum class CastState : u8 {
    None,
    MovingToTarget,
    TurningToFace,
    Foreswing,    // cast_time wind-up; effect fires when timer hits 0
    Channeling,   // sustained phase between fire and backswing (channel_time > 0)
    Backswing,
};

struct AbilitySet {
    std::vector<AbilityInstance> abilities;
    // All ability types live here: active, passive, auras, and applied abilities
    // (what WC3 calls "buffs"). No separate component for any of these.

    // Cast state machine — active while processing a Cast order
    CastState   cast_state    = CastState::None;
    f32         cast_timer    = 0;
    f32         foreswing_secs       = 0;  // cached for renderer: cast_time wind-up phase
    f32         channel_secs         = 0;  // cached for renderer: channel duration (0 = no channel)
    f32         cast_backswing_secs  = 0;  // cached for renderer: backswing recovery phase
    std::string casting_id;       // ability being cast
    Unit        cast_target_unit;
    glm::vec3   cast_target_pos{0};
    // If the active cast came from an inventory item slot, the item
    // handle is captured here so on_ability_effect can surface it to
    // map Lua. Reset to invalid when cast_state returns to None.
    Item        cast_source_item;
};

// Engine-built status flags. Transient action gates set by spells /
// effects / scripts, queried by sim systems each tick. See
// gameplay-model.md "Status Flags" for the full semantics and
// per-flag enforcement points.
//
// The bit values are part of the engine ABI — order matters.
namespace status {
    constexpr u32 Stunned      = 1u << 0;
    constexpr u32 Silenced     = 1u << 1;
    constexpr u32 Muted        = 1u << 2;
    constexpr u32 Disarmed     = 1u << 3;
    constexpr u32 Rooted       = 1u << 4;
    constexpr u32 Invulnerable = 1u << 5;
    constexpr u32 MagicImmune  = 1u << 6;
    constexpr u32 Untargetable = 1u << 7;
    constexpr u32 Unattackable = 1u << 8;
    constexpr u32 Paused       = 1u << 9;
    constexpr u32 Invisible    = 1u << 10;
    constexpr u32 NoAcquire    = 1u << 11;  // Block auto-acquire scan; drop current auto-acquired target.
    constexpr u32 Phased       = 1u << 12;  // Ignore unit-vs-unit collision (move/push through any unit). Terrain + buildings still block. DOTA "phased" (Phase Boots).

    constexpr u32 Count = 13;  // number of distinct flag bits used above
}

// Effective `flags` is the OR of two layers:
//   • manual_bits — set / cleared by direct `set_unit_status` calls
//     (legacy imperative path; deprecated for ability-domain flags).
//   • refcounts[bit_index] > 0 — incremented by each passive_flag
//     AbilityInstance that names this flag, decremented on remove.
// Two passive_flag instances both granting `silenced` keep silence in
// effect until both are removed. The `flags` u32 is recomputed by
// `recompute_effective_flags` whenever either layer changes, so all
// downstream readers can keep using `sf->flags & status::X`.
struct StatusFlags {
    u32 flags        = 0;   // effective view (kept in sync)
    u32 manual_bits  = 0;   // imperative SetUnitStatus layer
    std::array<u8, status::Count> refcounts = {};
};

// Transient per-tick output of system_true_sight. For each unit with
// UNIT_STATUS_INVISIBLE that sits within range of at least one
// enemy-owned detector (numeric attribute `true_sight` > 0), bit
// `detector.player.id` is set. The renderer ORs this with the
// owner/ally check to decide whether to cull. Component is added only
// for units that are revealed; absence == fully invisible to enemies.
struct TrueSightVisibility {
    u32 revealed_to_mask = 0;
};

// Manual per-player visibility override set by UnitReveal(unit, player, true).
// Persists until UnitReveal(unit, player, false). Bypasses invisibility
// and fog for the asking player but does not affect other players.
struct ForcedVisibility {
    u32 revealed_to_mask = 0;
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
    u8          target_bit = 0;  // widget-target bit (STRUCTURE / TREE / DEBRIS) for attack validation
    bool        selectable = true;  // left-clickable? false for trees (WC3: trees aren't selectable)
};

// ── Doodad Components ──────────────────────────────────────────────────────
// Doodads are pure decoration: no health, no collision, no pathing block.
// We keep the variation so save_objects can round-trip the JSON entry
// (type_id lives in HandleInfo).
struct DoodadComp {
    u8 variation = 0;
};

// Cell rectangle an object occupies on the runtime pathing grid.
// (cx, cy) is the south-west pathing CELL of the footprint; the block
// covers cells [cx, cx+w) × [cy, cy+h). Cell units (32 game units each
// = 1/4 terrain tile) so a tree can block a 2×2 sub-tile patch instead
// of a whole tile. Buildings author footprints in tiles and convert
// via Pathfinder::block_tiles, which expands to cell units internally;
// destructables author footprints directly in cells. See
// Pathfinder::block_cells / unblock_cells.
struct PathingBlocker {
    i32 cx = 0;
    i32 cy = 0;
    u32 w  = 0;
    u32 h  = 0;
};

// ── Item Components ────────────────────────────────────────────────────────

struct ItemInfo {
    std::string type_id;
    // Two free integer fields the engine stores + renders but never
    // interprets. Map Lua handles consumption / level-up logic. Both
    // default to 0; the initial values come from item type def.
    i32         charges = 0;
    i32         level   = 0;
};

struct Carriable {
    Unit carried_by;
};

// ── Projectile Component ───────────────────────────────────────────────────
//
// Projectile is an "agent" — it has a handle (lives in world.projectiles
// + world.handle_infos) but is NOT a widget. No HP, no selection, no
// orders, no inventory. Carries a payload from a source point to either
// a homing target (Path::Homing) or a target point (Path::Linear), and
// fires PROJECTILE_HIT events for hits + PROJECTILE_DESTROYED on every
// destroy path.
//
// `damage` is a first-class field because the engine consumes it for
// `is_attack` projectiles (auto-attack arrows). Ability projectiles can
// set / get it too — engine just ignores. Lua-side state beyond damage
// goes in a side table keyed by the projectile handle.

struct ProjectileComp {
    Unit        source;
    Unit        target;                  // homing target (invalid for Linear)
    glm::vec3   target_pos{0.0f};        // linear terminus or fallback for Homing on target loss
    f32         speed         = 0;
    f32         arc_height    = 0;       // peak height of a ballistic arc (game units). 0 = straight/flat flight.
    f32         damage        = 0;       // engine consumes for is_attack; Lua can set/get for any
    bool        is_attack     = false;   // engine routes hits through deal_attack_damage
    f32         hit_radius    = 32.0f;
    f32         max_distance  = 0;       // Linear only (0 = use lifetime)
    f32         max_lifetime  = 10.0f;   // safety cap
    f32         elapsed       = 0;
    f32         traveled      = 0;
    bool        emitted       = false;   // false between CreateProjectile and EmitProjectile*
    enum class Path : u8 { Homing, Linear } path = Path::Homing;
    glm::vec3   spawn_pos{0.0f};
    // Linear with pierce: every unit within hit_radius along the path
    // fires PROJECTILE_HIT. Tracked so we don't hit the same unit twice
    // on a single flight. Stored as full Unit handles (id + generation)
    // so a recycled id can't alias a different unit — a raw-id list
    // would wrongly mark a freshly-spawned unit (same id, new gen) as
    // already-hit and grant it false immunity.
    std::vector<Unit> already_hit;
    // Dying state — gameplay has ended (PROJECTILE_DESTROYED fired,
    // triggers cleaned up, no further movement / collision) but the
    // entity persists briefly so the renderer can play the model's
    // `death` clip. `death_timer` counts down to 0, then the entity
    // is silently torn down.
    bool        dying         = false;
    f32         death_timer   = 0;
};

// ── Rendering ──────────────────────────────────────────────────────────────

struct Renderable {
    std::string model_path;
    bool        visible = true;
    bool        skip_birth = false;  // skip birth animation (entity revealed/spawned out of viewer sight, not born in view)
    f32         visual_alpha = 1.0f; // 0..1 multiplier on fragment alpha (SetUnitAlpha)
};

// Script-driven animation queue. Filled by SetUnitAnimation /
// QueueUnitAnimation in Lua; consumed by the renderer's draw loop
// (which advances the front clip each time it finishes, removes the
// entry when the queue empties, and lets derive_anim_state take over
// again). While the entry is present, the engine's per-frame
// re-derivation of AnimState from sim components (combat / movement /
// cast state) is bypassed — script_controlled is the renderer's
// internal mirror of "this unit has an AnimQueue right now."
//
// Death is special: when the unit dies the entry is force-removed
// so the death animation plays instead of whatever the map queued.
struct AnimQueue {
    std::deque<std::string> clips;        // FIFO of clip names
    bool                    looping = false;  // last clip in the queue loops
};

} // namespace uldum::simulation
