# Uldum Engine — Gameplay Object Model

This document defines the game object hierarchy, component design, and gameplay systems.
Inspired by Warcraft III's object model, implemented via ECS internally.

See `docs/map-system.md` for the full engine vs map boundary definition.

### Key Terminology

- **Ability**: anything attached to a unit that has effects — active (cast by player), passive (always on), or applied (given to a unit by another ability, with optional duration and auto-remove). In WC3 terms, "buffs" and "auras" are both just abilities.
- **State**: a depletable/regenerating resource (HP, mana, energy). Has `current`, `max`, `regen`. HP is engine-built-in; others are map-defined.
- **Attribute**: a single-value modifier (strength, agility, intelligence). Does not deplete. All map-defined.
- **Classification**: a string-based targeting flag ("ground", "air", "hero"). All map-defined.

## 1. Object Hierarchy

### WC3 Reference

In WC3's JASS type system, the hierarchy is:

```
handle
└── widget              (has position + health, can be targeted/damaged)
    ├── unit            (ALL game units — regular, heroes, AND buildings)
    │   ├── [regular]       differentiated by flags, not separate types
    │   ├── [hero]          IsUnitType(u, UNIT_TYPE_HERO)
    │   └── [building]      IsUnitType(u, UNIT_TYPE_STRUCTURE)
    ├── destructable    (environment objects — trees, rocks, gates)
    └── item            (ground items — potions, weapons)
```

Key points from WC3:
- **Building IS a Unit** — same type, just has the STRUCTURE flag
- **Hero IS a Unit** — same type, just has the HERO flag plus level/XP/inventory
- **Item is NOT a Destructable** — they are siblings under Widget
- **Doodad** is not in the type system — purely visual, outside the gameplay model
- **Projectile** is also not in the type system — a visual effect, not a gameplay object

### Uldum Hierarchy

Same model, expressed as **component sets** rather than class inheritance.
Each "type" in the hierarchy is defined by which components a handle receives at creation.

```
Handle (u32 id + u32 generation)
│
├── Widget (Transform + Health + Selectable)
│   │
│   │   The three Widget subtypes — the core gameplay objects:
│   │
│   ├── Unit (Widget + Owner + Movement + Combat + Vision + OrderQueue + AbilitySet)
│   │   │
│   │   │   Unit subtypes are NOT separate categories. They are regular
│   │   │   units with extra components + classification flags:
│   │   │
│   │   ├── [Regular Unit]    — base unit, no extra components
│   │   │                       e.g. Footman, Archer, Grunt
│   │   │
│   │   ├── [Hero]            — + Hero + Inventory components
│   │   │                       + HERO classification flag
│   │   │                       e.g. Paladin, Archmage
│   │   │
│   │   └── [Building]        — + Building component
│   │                           + STRUCTURE classification flag
│   │                           Movement.speed = 0 (immobile)
│   │                           e.g. Barracks, Town Hall
│   │
│   ├── Destructable (Widget + Destructable + PathingBlocker)
│   │                   e.g. Tree, Rock, Barrel, Gate
│   │
│   └── Item (Widget + ItemInfo + Carriable)
│              e.g. Potion, Sword, Scroll
│
├── Doodad (Transform + Renderable only — NOT a Widget, no gameplay)
│            e.g. Flower, Signpost, Fence
│
└── Projectile (Transform + Projectile + Renderable — transient, NOT a Widget)
                 e.g. Arrow, Fireball, Frost Bolt
```

### Three Levels

1. **Handle** — base type for everything in the world (id + generation). Internally indexes into component storage.
2. **Widget** — handles that participate in gameplay (have health, can be selected/targeted). The three Widget subtypes are Unit, Destructable, and Item.
3. **Unit subtypes** — Hero and Building are units with extra components bolted on, identified by `UnitClassification` flags. They are NOT separate categories.

Doodads and Projectiles are **not Widgets** — they have no health, cannot be targeted,
and do not participate in the gameplay model. Doodads are purely visual. Projectiles
are transient visual objects with simple movement logic.

### Typed Handles (public API)

The public API (C++ game code and Lua scripts) works with **typed handles**. This follows
WC3's JASS model where `unit`, `destructable`, `item` are distinct types that internally
hold a handle reference.

```
JASS                          Uldum C++                    Uldum Lua
────                          ──────────                   ─────────
handle (base)                 Handle { id, generation }    (not exposed)
├── unit                      Unit   : Handle              Unit
├── destructable              Destructable : Handle        Destructable
├── item                      Item   : Handle              Item
└── player                    Player { id }                Player
```

Each typed handle holds an `id` (index into component storage) and a `generation`
counter (incremented when the slot is reused). This is the same `Handle` concept
from `core/handle.h` — there is no separate "entity ID."

- **Type safety**: you cannot pass an Item where a Unit is expected — enforced at
  compile time in C++ and at runtime in Lua.
- **Stale detection**: if a unit dies and its handle slot is reused, old Unit handles
  detect the generation mismatch and safely fail.
- The raw `id` is an internal index. Scripts and game code work with typed handles,
  never raw integers (unless explicitly requesting it, like WC3's `GetHandleId()`).

```cpp
// Base handle — shared by all game object types
struct Handle { u32 id; u32 generation; };

// Typed handles — distinct types, same layout
struct Unit          : Handle {};
struct Destructable  : Handle {};
struct Item          : Handle {};
struct Player        { PlayerId id; };
```

## 3. Component Design

### All Game Objects

Every game object in the world has these components (stored internally, indexed by handle ID):

```
Transform {
    vec3  position
    f32   facing        // rotation around Z axis (up), in radians. 0 = facing +Y
    f32   scale         // uniform scale, default 1.0
}

HandleInfo {
    TypeId     type_id   // references a type definition (e.g. "footman")
    Category   category  // unit, destructable, item, doodad, projectile
                         // (hero/building are units — identified by classification flags)
    u32        generation // incremented on reuse, used by typed handles for stale detection
}
```

### Widget Components (targetable, has health)

Added to Units, Buildings, Heroes, Destructables, Items — anything that can be selected or damaged.

```
Health {
    f32       current
    f32       max
    f32       regen_per_sec
}

// Additional states (mana, energy, etc.) are map-defined.
// The engine provides a generic state storage system.
StateBlock {
    map<string, StateValue>  states   // "mana" → {current, max, regen}
}

StateValue {
    f32  current
    f32  max
    f32  regen_per_sec
}

Selectable {
    f32  selection_radius    // circle size when selected
    i32  priority            // selection priority (heroes > units > buildings)
}
```

Note: `armor`, `armor_type`, and `attack_type` are no longer engine concepts.
They are map-defined strings stored in unit type definitions. The map's Lua
damage handler reads them from the attacker/target and applies its own formula.

### Unit Components

```
Owner {
    PlayerId  player
}

Movement {
    f32       speed          // units per second
    f32       turn_rate      // radians per second
    MoveType  type           // ground, air, amphibious — engine preset (needed for pathfinding)
    vec3      target_pos     // current movement target (used by MovementSystem)
    bool      moving         // currently in motion
}

Combat {
    f32        damage
    f32        range          // attack range (melee ~1.0, ranged ~6.0+)
    f32        attack_cooldown // seconds between attacks (full cycle)
    f32        dmg_time       // seconds: fore-swing before damage fires
    f32        backsw_time    // seconds: recovery after damage fires
    f32        dmg_pt         // fraction (0-1) of attack animation at damage point
    Unit       target         // current attack target (typed handle, not raw ID)
}

Vision {
    f32  sight_range_day
    f32  sight_range_night
}

OrderQueue {
    Order              current
    std::deque<Order>  queued
}

AbilitySet {
    std::vector<AbilityInstance>  abilities
    // All ability types live here: active, passive, auras, and applied abilities
    // (what WC3 calls "buffs"). No separate component for any of these.
}

UnitClassification {
    set<string> flags   // map-defined strings: "ground", "air", "hero", "structure", etc.
}

// Map-defined attributes (strength, agility, intelligence, etc.)
AttributeBlock {
    map<string, f32>  values   // "strength" → 22.0, "agility" → 13.0
}
```

Note: `MoveType` is one of the few engine-defined enums because pathfinding needs
to query terrain differently for ground vs air vs amphibious movement.

### Hero Components (added to hero-type units)

```
Hero {
    u32     level
    u32     xp
    u32     xp_to_next
    string  primary_attr          // map-defined attribute name (e.g. "strength")

    // Per-level attribute growth rates — map-defined attribute names
    map<string, f32>  attr_per_level   // "strength" → 2.7, "agility" → 1.5
}

Inventory {
    std::array<Item, 6>  slots   // typed Item handles, null if empty
}
```

Hero attributes (strength, agility, intelligence, etc.) are stored in the unit's
`AttributeBlock` component, not in the Hero component. The Hero component only
tracks leveling metadata. Attribute values and growth rates reference map-defined
attribute names via strings.

### Building Components (added to building-type units)

```
Building {
    std::deque<TrainOrder>     train_queue      // units being trained
    std::vector<ResearchId>    researched        // completed researches
    std::vector<ResearchId>    available_research
}

Construction {
    f32   build_progress    // 0.0 → 1.0
    bool  under_construction
    f32   build_time_total  // seconds
}
```

### Destructable Components

```
Destructable {
    DestructableTypeId  type
    u8                  variation    // visual variation index
}

PathingBlocker {
    std::vector<TileCoord>  blocked_tiles
}
```

### Item Components

```
ItemInfo {
    ItemTypeId  type
    i32         charges      // -1 = unlimited, 0 = consumed
    f32         cooldown     // if item has an active ability
    f32         cooldown_remaining
}

Carriable {
    Unit  carried_by    // unit carrying this item, null if on ground
}
```

### Doodad

Doodads have only `Transform`, `HandleInfo`, and `Renderable`. No Widget components —
they cannot be selected, damaged, or interacted with.

### Projectile Components

```
Projectile {
    Unit       source        // unit that fired
    Unit       target        // homing target (if unit-targeted)
    vec3       target_pos    // destination (if point-targeted)
    f32        speed
    f32        damage
    AbilityId  source_ability // ability that created this projectile
    bool       homing        // tracks target vs travels to point
}
```

### Rendering Component (all visible entities)

```
Renderable {
    ModelHandle     model
    AnimationState  anim
    bool            visible
}
```

## 4. Order System

### Order Types

```
Move            { vec3 target_pos }
AttackMove      { vec3 target_pos }
Attack          { Unit target }
Stop            { }
HoldPosition    { }
Patrol          { std::vector<vec3> waypoints, u32 current_waypoint }
Cast            { AbilityId ability, Unit target_unit, vec3 target_pos }
Train           { TypeId unit_type }              // buildings only
Research        { ResearchId research }            // buildings only
Build           { TypeId building_type, vec3 pos } // workers only
PickupItem      { Item item }
DropItem        { Item item, vec3 pos }
```

### Order Structure

```
Order {
    OrderType  type
    // union/variant of the above payloads
}
```

### Order Flow

1. **Player input** (click, hotkey) → create an `Order`
2. **Validate** — can the unit execute this order? (has ability? in range? correct unit type?)
3. **Push to OrderQueue** — if shift-queued, append; otherwise replace current
4. **Each simulation tick**: the relevant system executes `OrderQueue.current`
   - `MovementSystem` handles Move, AttackMove (move phase), Patrol, Build (walk to site)
   - `CombatSystem` handles Attack, AttackMove (attack phase)
   - `AbilitySystem` handles Cast
   - `TrainSystem` handles Train, Research
5. **On completion**: pop `current`, promote next from `queued`
6. **On interrupt** (stunned, new order): clear `current`, optionally clear `queued`

### Default Orders

All units automatically get: `attack`, `move`, `stop`, `hold_position`.
Buildings get: `set_rally_point`.
Heroes additionally get: item-related orders.

## 5. Ability System

See [ability-system.md](ability-system.md) for the full ability system design, including:
- Ability forms (passive, aura, instant, target_unit, target_point, toggle, channel)
- AbilityDef (template) vs Ability (runtime instance) terminology
- Cast flow, state costs, modifier system
- Aura scanning, applied passives (buffs), duration/stacking
- Lua script callbacks and engine events
- API: `AddAbility`, `RemoveAbility`, `ApplyPassiveAbility`

### Design Principle

**Ability = Form + Effect**

- **Form** is one of ~10 engine-provided primitives (how it's activated, targeted, cast).
  The engine handles all the mechanical plumbing: cooldowns, mana, targeting, range
  checking, animation timing, projectiles, buff tracking, stat stacking.
- **Effect** is what actually happens — defined in JSON for simple cases, Lua for complex ones.

Map makers never deal with opaque ability IDs. They pick a clear form and define the effect
in human-readable JSON or Lua. No need to inherit from a hardcoded base ability.

When an ability is added to a unit, its effects are applied. When removed, they are reverted.
The engine tracks this automatically — map scripts don't need to remember what was modified.

### Ability Forms — Passive

Passive forms are always active while the ability is on the unit. No casting required.

| Form | Engine handles | Example |
|------|---------------|---------|
| **stat_modifier** | Apply/revert stat changes when ability is added/removed/leveled | +5 armor, +10% attack speed |
| **aura** | Find nearby units in radius, apply/remove a buff automatically | Devotion Aura, Brilliance Aura |
| **attack_modifier** | Hook into each attack hit, apply extra effect (slow, lifesteal, DoT) | Frost attack, Lifesteal, Poison |

### Ability Forms — Active

Active forms require the player (or AI/script) to activate them.

| Form | Engine handles | Example |
|------|---------------|---------|
| **instant** | Cooldown + mana + cast animation → fire effect (no target selection) | Divine Shield, Avatar, Blink |
| **target_unit** | + range check + target validation + optional projectile | Holy Light, Hex, Storm Bolt |
| **target_point** | + range check + ground position | Blizzard, Flame Strike |
| **target_unit_or_point** | Either targeting mode | Chain Lightning, Shockwave |
| **toggle** | On/off state, may drain mana per second while active | Immolation, Defend |
| **channel** | Sustained cast, ticks over duration, interrupted by stun/move | Tranquility, Starfall |

### Trigger Bindings

Any ability — regardless of form — can optionally bind to triggers. Triggers fire
Lua callbacks on game events. This is how you build reactive abilities like Critical
Strike, Bash, or Evasion without needing a special ability form.

Available trigger events:

```
on_attack_hit       // this unit's attack lands
on_attacked         // this unit is hit by an attack
on_damaged          // this unit takes damage (any source)
on_deal_damage      // this unit deals damage (any source)
on_kill             // this unit kills another unit
on_death            // this unit dies
on_ability_cast     // this unit finishes casting an ability
on_order_received   // this unit receives an order
on_ability_applied  // an ability is applied to this unit (by another unit/ability)
on_ability_removed  // an applied ability is removed from this unit
```

Example: Critical Strike is a `stat_modifier` (no active component) with an
`on_attack_hit` trigger that rolls a chance and multiplies damage via Lua.

### What the Engine Handles vs What Scripts Handle

**Engine (C++):**
- Cooldown countdown and reset
- Mana cost validation and deduction
- Range checking and target validation (friend/foe/self filters)
- Cast animation timing (effect fires at the cast point frame)
- Projectile spawning, movement, and hit detection
- Applied ability tracking: application, duration, and removal
- Modifier stacking and recalculation (base + all active ability modifiers = effective)
- Aura radius scanning (apply/remove aura abilities to nearby units)
- Toggle state management and per-second mana drain
- Channel tick timing and interruption on stun/move
- Automatic revert of all effects when ability is removed from unit

**Declarative JSON (simple effects):**
```json
{
    "devotion_aura": {
        "form": "aura",
        "aura_radius": 900,
        "target_filter": { "ally": true },
        "levels": [
            { "buff": { "modifiers": { "armor_flat": 1.5 } } },
            { "buff": { "modifiers": { "armor_flat": 3.0 } } },
            { "buff": { "modifiers": { "armor_flat": 4.5 } } }
        ]
    },
    "holy_light": {
        "form": "target_unit",
        "target_filter": { "ally": true, "self": true },
        "cooldown": 9,
        "mana_cost": 65,
        "range": 800,
        "levels": [
            { "heal": 200 },
            { "heal": 400 },
            { "heal": 600 }
        ]
    },
    "frost_attack": {
        "form": "attack_modifier",
        "on_hit_buff": "frost_slow",
        "levels": [
            { "bonus_damage": 5 }
        ]
    },
    "critical_strike": {
        "form": "stat_modifier",
        "triggers": {
            "on_attack_hit": "critical_strike_on_hit"
        },
        "levels": [
            { "chance": 0.15, "multiplier": 2.0 },
            { "chance": 0.15, "multiplier": 3.0 },
            { "chance": 0.15, "multiplier": 4.0 }
        ]
    }
}
```

**Lua scripts (complex/custom effects):**
```lua
-- Critical Strike trigger handler
function critical_strike_on_hit(caster, target, ability_level, context)
    local def = GetAbilityLevelData(caster, "critical_strike", ability_level)
    if math.random() < def.chance then
        context.damage = context.damage * def.multiplier
        -- play crit visual effect
        CreateEffect("effects/critical_hit.gltf", GetUnitPosition(target))
    end
end

-- Chain Lightning: too complex for pure JSON
ability_handlers["chain_lightning"] = {
    on_hit = function(caster, target, level)
        local damage = 85 * level
        local bounces = 4 + level
        local current = target
        local hit = {}
        for i = 1, bounces do
            DamageUnit(caster, current, damage)
            hit[current] = true
            damage = damage * 0.85
            current = FindNearestEnemy(current, 500, hit)
            if not current then break end
        end
    end
}
```

### Ability Definition (internal structure)

```
AbilityDef {
    AbilityId       id
    std::string     name
    AbilityForm     form        // stat_modifier, aura, attack_modifier,
                                // instant, target_unit, target_point,
                                // target_unit_or_point, toggle, channel
    f32             cooldown
    f32             mana_cost
    f32             range
    TargetFilter    target_filter   // ally, enemy, self, ground, air, etc.
    u32             max_level

    // Per-level data (indexed by level)
    std::vector<AbilityLevelData>  levels

    // Optional trigger bindings (event name → Lua function name)
    std::unordered_map<std::string, std::string>  triggers

    // Form-specific config
    f32             aura_radius         // aura form
    f32             channel_duration    // channel form
    f32             channel_interval    // channel form
    f32             toggle_mana_per_sec // toggle form
    BuffId          on_hit_buff         // attack_modifier form
    ProjectileTypeId projectile         // target_unit/point forms
}

AbilityLevelData {
    // Declarative effects (engine applies directly)
    f32     damage
    f32     heal
    f32     area_of_effect
    BuffId  applied_buff        // buff to apply on cast/hit

    // Form-specific per-level values
    StatModifiers   modifiers   // stat_modifier form
    f32     bonus_damage        // attack_modifier form

    // Trigger data (passed to Lua handler)
    f32     chance              // for on_hit triggers (e.g. critical strike)
    f32     multiplier          // for on_hit triggers
    // ... extensible via JSON — Lua reads arbitrary fields
}
```

### Ability Instance (per-unit runtime state)

```
AbilityInstance {
    AbilityId   def_id
    u32         level               // current level (1-indexed, 0 = not learned)
    f32         cooldown_remaining
    bool        auto_cast           // auto-cast enabled (for applicable forms)
    bool        toggle_active       // toggle form: currently on?
    bool        channeling          // channel form: currently casting?
    f32         channel_remaining   // channel form: time left
}
```

### Ability Execution Flow (active forms)

1. Player issues `Cast` order with ability ID + target
2. `AbilitySystem` validates: cooldown ready? mana? target passes filter? in range?
3. Unit turns to face target, plays cast animation
4. At cast point (animation event):
   - Deduct mana, start cooldown
   - If ability has a projectile → spawn projectile, effect fires on impact
   - Otherwise → apply effect immediately:
     - Declarative: engine applies damage/heal/buff from AbilityLevelData
     - Scripted: engine calls the Lua `on_hit` / `on_cast` handler
5. For **channel**: repeat step 4 at `channel_interval` until `channel_duration` expires or interrupted
6. For **toggle**: apply effect on activate, revert on deactivate; drain mana per second

### Passive Ability Processing

- **stat_modifier**: modifiers applied when ability is added/leveled, reverted on removal
- **aura**: each tick, scan for units in radius, apply/refresh buff, remove from units that left
- **attack_modifier**: hooks into CombatSystem; on each attack hit, apply bonus damage + on_hit_buff
- **trigger bindings**: registered with the event system; fire Lua callback when event occurs

## 6. Applied Abilities (WC3 "Buffs")

In WC3 terms, a "buff" or "debuff" is an ability applied to a unit by another
ability. In Uldum, these are just ability instances in the target's AbilitySet —
not a separate system. An applied ability:
- Has a duration (auto-removed when expired, or permanent if duration = 0)
- Can modify attributes/states while active
- Can tick periodic effects (damage, heal)
- Can be dispelled
- Has a source (the unit that applied it)

### Applied Ability Definition (part of ability definitions)

```json
{
    "frost_slow": {
        "form": "passive",
        "duration": 5.0,
        "tick_interval": 0,
        "dispellable": true,
        "is_positive": false,
        "modifiers": {
            "move_speed_percent": -25
        }
    },
    "poison_sting": {
        "form": "passive",
        "duration": 10.0,
        "tick_interval": 1.0,
        "dispellable": true,
        "is_positive": false,
        "periodic_damage": 4,
        "modifiers": {
            "move_speed_percent": -25
        }
    }
}
```

### Applied Ability Processing (each tick, part of AbilitySystem)

1. Tick `remaining_duration` on all ability instances that have a duration — remove expired
2. Tick periodic effects (damage/heal) at `tick_interval`
3. Recalculate effective values when abilities change:
   - Base attribute + sum of all active ability modifiers = effective attribute
   - Systems read effective values, not base values

## 7. Type Definition System

Types are defined in JSON, loaded from the map's `types/` directory. The engine
defines no gameplay types — maps are fully self-contained. See `docs/map-system.md`
for the map package structure.

### Unit Type Example (Regular Unit)

All units (regular, hero, building) use `"category": "unit"`. Subtypes are determined
by classification flags and presence of extra blocks (`"hero"`, `"building"`).

```json
{
    "footman": {
        "category": "unit",
        "display_name": "Footman",
        "model": "models/units/footman.gltf",
        "icon": "icons/footman.png",
        "health": { "max": 420, "regen": 0.25 },
        "states": {
            "mana": { "max": 0, "regen": 0 }
        },
        "attributes": {
            "armor": 2,
            "armor_type": "heavy",
            "attack_type": "normal"
        },
        "movement": { "speed": 270, "turn_rate": 0.6, "type": "ground" },
        "combat": { "damage": 13, "range": 1.0, "cooldown": 1.35 },
        "vision": { "day": 1400, "night": 800 },
        "abilities": ["attack", "move", "stop", "hold_position", "defend"],
        "classifications": ["ground"],
        "selection": { "radius": 0.8, "priority": 5 }
    }
}
```

### Hero Type Example

A hero is a unit with `"classifications": ["hero", ...]` and a `"hero"` block.

```json
{
    "paladin": {
        "category": "unit",
        "display_name": "Paladin",
        "model": "models/units/paladin.gltf",
        "health": { "max": 650, "regen": 2.0 },
        "states": {
            "mana": { "max": 255, "regen": 0.85 }
        },
        "attributes": {
            "armor": 4,
            "armor_type": "hero",
            "attack_type": "hero",
            "strength": 22, "agility": 13, "intelligence": 17
        },
        "movement": { "speed": 320, "turn_rate": 0.8, "type": "ground" },
        "combat": { "damage": 26, "range": 1.0, "cooldown": 1.8 },
        "vision": { "day": 1800, "night": 800 },
        "abilities": ["attack", "move", "holy_light", "divine_shield", "devotion_aura", "resurrection"],
        "hero": {
            "primary_attr": "strength",
            "attr_per_level": { "strength": 2.7, "agility": 1.5, "intelligence": 1.8 }
        },
        "classifications": ["ground", "hero"],
        "selection": { "radius": 1.0, "priority": 10 }
    }
}
```

### Building Type Example

A building is a unit with `"classifications": ["structure", ...]` and a `"building"` block.
Movement speed is 0 (immobile).

```json
{
    "barracks": {
        "category": "unit",
        "display_name": "Barracks",
        "model": "models/buildings/barracks.gltf",
        "health": { "max": 1500, "regen": 0.5 },
        "attributes": {
            "armor": 5,
            "armor_type": "fortified"
        },
        "movement": { "speed": 0, "turn_rate": 0, "type": "ground" },
        "vision": { "day": 900, "night": 600 },
        "abilities": ["train_footman", "train_rifleman", "research_defend"],
        "classifications": ["structure", "ground"],
        "building": {
            "build_time": 60,
            "trains": ["footman", "rifleman"],
            "researches": ["defend"]
        },
        "pathing": { "blocked_tiles": [[0,0], [1,0], [0,1], [1,1]] },
        "selection": { "radius": 2.0, "priority": 2 }
    }
}
```

### Destructable Type Example

```json
{
    "tree_ashenvale": {
        "category": "destructable",
        "display_name": "Ashenvale Tree",
        "model": "models/destructables/tree_ashenvale.gltf",
        "health": { "max": 50 },
        "attributes": { "armor": 0, "armor_type": "fortified" },
        "pathing": { "blocked_tiles": [[0, 0]] },
        "variations": 3
    }
}
```

### Item Type Example

```json
{
    "potion_healing": {
        "category": "item",
        "display_name": "Potion of Healing",
        "icon": "textures/icons/potion_healing.png",
        "charges": 1,
        "cooldown": 5.0,
        "on_use": {
            "effect": "heal",
            "target": "self",
            "value": 250
        },
        "gold_cost": 150
    }
}
```

### Ability Definition Example

```json
{
    "holy_light": {
        "name": "Holy Light",
        "target_type": "unit",
        "max_level": 3,
        "levels": [
            { "mana_cost": 65, "cooldown": 9, "range": 800, "heal": 200, "damage_vs_undead": 100 },
            { "mana_cost": 65, "cooldown": 9, "range": 800, "heal": 400, "damage_vs_undead": 200 },
            { "mana_cost": 65, "cooldown": 9, "range": 800, "heal": 600, "damage_vs_undead": 300 }
        ]
    },
    "devotion_aura": {
        "name": "Devotion Aura",
        "target_type": "passive",
        "max_level": 3,
        "levels": [
            { "aura_radius": 900, "buff": "devotion_aura_buff_1" },
            { "aura_radius": 900, "buff": "devotion_aura_buff_2" },
            { "aura_radius": 900, "buff": "devotion_aura_buff_3" }
        ]
    }
}
```

### Applied Ability Definition Example

```json
{
    "devotion_aura_buff_1": {
        "name": "Devotion Aura",
        "duration": 0,
        "is_positive": true,
        "dispellable": false,
        "modifiers": {
            "armor_flat": 1.5
        }
    },
    "bloodlust_buff": {
        "name": "Bloodlust",
        "duration": 40,
        "is_positive": true,
        "dispellable": true,
        "modifiers": {
            "attack_speed_percent": 40,
            "move_speed_percent": 25
        }
    },
    "poison_sting": {
        "name": "Poison Sting",
        "duration": 10,
        "tick_interval": 1.0,
        "is_positive": false,
        "dispellable": true,
        "periodic": {
            "damage": 4
        },
        "modifiers": {
            "move_speed_percent": -25
        }
    }
}
```

## 8. ECS Implementation Notes

### Component Storage — Sparse Set

Each component type gets its own sparse set, indexed by handle ID:
- **Dense array**: packed component data (cache-friendly iteration)
- **Sparse array**: handle ID → dense index mapping (O(1) lookup)
- Adding/removing components is O(1)
- Iterating all objects with a component is linear over the dense array

### Systems

Systems are functions that iterate over component tuples each simulation tick:

| System | Components | Responsibility |
|--------|------------|----------------|
| `MovementSystem` | Transform, Movement, OrderQueue | Move units toward targets, handle pathfinding |
| `CombatSystem` | Transform, Combat, OrderQueue | Acquire targets, attack flow (dmg_time fore-swing, backsw_time recovery), spawn projectiles |
| `AbilitySystem` | AbilitySet, OrderQueue, Transform, Owner | Tick cooldowns, execute cast orders, tick applied ability durations, remove expired, scan aura radii and apply/remove |
| `VisionSystem` | Transform, Vision, Owner | Update per-player fog of war |
| `ProjectileSystem` | Projectile, Transform | Move projectiles, check hit, fire impact event |
| `HealthSystem` | Health | Apply regen, check death (HP ≤ 0), fire death event |
| `StateSystem` | StateBlock | Tick regen for all map-defined states (mana, energy, etc.) |
| `TrainSystem` | Building | Progress train queues, spawn trained units |
| `ConstructionSystem` | Construction | Progress building construction |

### World

The `World` struct owns all component storage and the handle allocator.
All public APIs use typed handles (Unit, Destructable, Item) — the raw
`id` inside a handle is an internal index, never exposed directly.

```cpp
namespace uldum::simulation {

struct World {
    // ── Internal component storage (handle.id-indexed sparse sets) ──
    // These are PRIVATE. External code accesses data through the
    // typed facade functions below, never through raw sparse sets.

    SparseSet<Transform>    transforms;
    SparseSet<HandleInfo>   handle_infos;
    SparseSet<Health>       healths;
    SparseSet<Movement>     movements;
    SparseSet<Combat>       combats;
    SparseSet<Owner>        owners;
    SparseSet<OrderQueue>   order_queues;
    SparseSet<AbilitySet>   ability_sets;
    SparseSet<Vision>       visions;
    SparseSet<Hero>         heroes;
    SparseSet<Inventory>    inventories;
    SparseSet<Building>     buildings;
    // ... etc for all component types

    // Handle allocator (manages id + generation)
    HandleAllocator handles;
    bool validate(Handle h) const;  // checks generation match

    // Type registry
    TypeRegistry types;  // loaded from JSON
};

// ── Typed Game Objects ─────────────────────────────────────────
// Public-facing types. Each holds an internal ID + generation.
// These are what C++ game code and Lua scripts work with.

struct Unit          { u32 id; u32 generation; };
struct Destructable  { u32 id; u32 generation; };
struct Item          { u32 id; u32 generation; };
struct Player        { PlayerId id; };

// ── Creation (free functions) ──────────────────────────────────
// create_unit handles ALL unit subtypes. The type definition JSON
// determines whether Building/Hero components are attached:
//   - type has "hero" block → adds Hero + Inventory components
//   - type has "classifications": ["structure"] → adds Building, speed=0

Unit          create_unit(World& world, TypeId type, Player owner, f32 x, f32 y, f32 facing = 0);
Destructable  create_destructable(World& world, TypeId type, f32 x, f32 y, f32 facing = 0, u8 variation = 0);
Item          create_item(World& world, TypeId type, f32 x, f32 y);

void          destroy(World& world, Unit unit);
void          destroy(World& world, Destructable destr);
void          destroy(World& world, Item item);

// ── Unit API ───────────────────────────────────────────────────
// These are what Lua will bind to in Phase 7.

void     issue_order(World& world, Unit unit, Order order);
void     add_ability(World& world, Unit unit, AbilityId ability);
void     remove_ability(World& world, Unit unit, AbilityId ability);
void     apply_ability(World& world, Unit target, AbilityId ability, Unit source);
void     remove_ability(World& world, Unit target, AbilityId ability);

f32      get_health(const World& world, Unit unit);
void     set_health(World& world, Unit unit, f32 health);
vec3     get_position(const World& world, Unit unit);
void     set_position(World& world, Unit unit, f32 x, f32 y);
Player   get_owner(const World& world, Unit unit);
bool     is_alive(const World& world, Unit unit);
bool     is_hero(const World& world, Unit unit);
bool     is_building(const World& world, Unit unit);

// ── Hero API (operates on units that have the Hero component) ──
void     hero_add_xp(World& world, Unit hero, u32 xp);
u32      hero_get_level(const World& world, Unit hero);
bool     hero_give_item(World& world, Unit hero, Item item);
Item     hero_get_item(const World& world, Unit hero, u32 slot);

// ── Destructable API ───────────────────────────────────────────
f32      get_health(const World& world, Destructable d);
void     kill(World& world, Destructable d);

// ── Item API ───────────────────────────────────────────────────
i32      get_charges(const World& world, Item item);
void     set_charges(World& world, Item item, i32 charges);

} // namespace uldum::simulation
```
