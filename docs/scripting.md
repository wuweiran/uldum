# Uldum Engine — Scripting System Design

## Overview

Maps define all gameplay logic via Lua 5.4 scripts. The engine provides a C++ API
exposed to Lua through sol2 bindings. This is analogous to WC3's JASS system, but
using Lua instead of a custom language.

### Key Files

| File | Purpose |
|------|---------|
| `engine/scripts/common.lua` | Engine API function declarations — the "common.j" equivalent. Loaded before any map script. Defines all engine functions available to map scripts. |
| `engine/scripts/blizzard.lua` | Optional convenience library — helper functions built on top of the engine API (like WC3's Blizzard.j). |
| Map's `shared/scripts/main.lua` | Map entry point — runs after common.lua. Registers events, sets up gameplay. |

### Execution Order

1. Engine initializes Lua VM (sandboxed — no os, io, require from filesystem)
2. Engine loads and executes `engine/scripts/common.lua` (defines API)
3. Engine loads and executes map's `shared/scripts/main.lua`
4. Map's main.lua registers event handlers, creates initial game state
5. Game loop begins — events fire, Lua handlers run synchronously within simulation ticks

## Lua VM

- **Lua 5.4** with **sol2** for C++ ↔ Lua binding
- One VM per map (sandboxed)
- **Disabled**: `os`, `io`, `loadfile`, `dofile`, `require` (filesystem access)
- **Available**: `math`, `string`, `table`, `coroutine`, `print` (→ engine log)
- Scripts run synchronously within the simulation tick — no threading
- Errors in Lua are caught and logged, don't crash the engine

## Trigger System

WC3-style triggers adapted to Lua. A trigger is a lifecycle scope that owns events,
conditions, actions, timers, and custom data. When destroyed, everything it owns is
cleaned up automatically.

### Trigger Basics

```lua
local trig = CreateTrigger()

-- Register events (what causes the trigger to fire)
TriggerRegisterEvent(trig, "on_death")
TriggerRegisterTimerEvent(trig, 1.0, true)

-- Condition (optional — if returns false, action is skipped)
TriggerAddCondition(trig, function()
    return GetTriggerUnit() ~= nil
end)

-- Action (what happens when the trigger fires and conditions pass)
TriggerAddAction(trig, function()
    local event = GetTriggerEvent()
    if event == "on_death" then
        Log("Someone died: " .. GetUnitTypeId(GetTriggerUnit()))
    elseif event == "timer" then
        Log("Timer tick")
    end
end)

-- Custom data scoped to the trigger
trig.data.counter = 0

-- Destroy: all events unregistered, timers stopped, data gone
DestroyTrigger(trig)
```

### Scoped Registration (avoid boilerplate)

```lua
-- Global event: fires for ANY unit death
TriggerRegisterEvent(trig, "on_death")

-- Unit-scoped: fires only when THIS unit dies
TriggerRegisterUnitEvent(trig, my_hero, "on_death")

-- Unit+ability scoped: fires only for THIS unit's THIS ability
TriggerRegisterUnitAbilityEvent(trig, my_hero, "holy_light", "on_ability_effect")

-- Player-scoped: fires for any unit owned by this player
TriggerRegisterPlayerEvent(trig, player, "on_unit_created")
```

With scoped registration, actions don't need to filter manually:

```lua
-- Without scoping (verbose):
TriggerRegisterEvent(trig, "on_ability_effect")
TriggerAddAction(trig, function()
    if GetTriggerUnit() == my_hero and GetTriggerAbilityId() == "holy_light" then
        HealUnit(my_hero, GetSpellTargetUnit(), 200)
    end
end)

-- With scoping (clean):
TriggerRegisterUnitAbilityEvent(trig, my_hero, "holy_light", "on_ability_effect")
TriggerAddAction(trig, function()
    HealUnit(my_hero, GetSpellTargetUnit(), 200)
end)
```

### Lifecycle Binding

Triggers can be bound to a unit or ability — auto-destroyed when the unit/ability is removed.

```lua
-- Auto-destroyed when my_hero is removed from the game
local trig = CreateTrigger()
TriggerBindToUnit(trig, my_hero)

-- Auto-destroyed when "rampage" is removed from my_hero
local trig = CreateTrigger()
TriggerBindToAbility(trig, my_hero, "rampage")
```

### Context Functions

Inside a trigger action, use context functions to read event-specific data:

```lua
-- General
GetTriggerEvent() → string          -- which event fired this trigger
GetTriggerUnit() → unit             -- the unit that caused the event
GetTriggerPlayer() → player         -- the player involved

-- Ability
GetTriggerAbilityId() → string      -- ability that was cast/added/removed
GetSpellTargetUnit() → unit         -- target of a unit-targeted ability
GetSpellTargetPoint() → x, y        -- target of a point-targeted ability

-- Combat
GetDamageSource() → unit
GetDamageTarget() → unit
GetDamageAmount() → number
SetDamageAmount(amount)              -- modify damage before it's applied
GetKillingUnit() → unit
GetAttacker() → unit
GetAttackTarget() → unit
```

### RegisterEvent (shorthand)

For simple cases where you don't need trigger lifecycle, `RegisterEvent` is sugar:

```lua
-- Shorthand (creates a trigger internally)
local handle = RegisterEvent("on_damage", function(source, target, amount, context) ... end)
UnregisterEvent(handle)

-- Equivalent to:
local trig = CreateTrigger()
TriggerRegisterEvent(trig, "on_damage")
TriggerAddAction(trig, function() ... end)
DestroyTrigger(trig)  -- to unregister
```

### Event List

#### Ability Events

| Event | Arguments | Notes |
|-------|-----------|-------|
| `on_ability_added` | unit, ability_id, source | Fires for all abilities including buffs |
| `on_ability_removed` | unit, ability_id | Fires on remove and expiry |
| `on_ability_cast_filter` | caster, ability_id, target → bool | Return false to block cast |
| `on_ability_target_filter` | caster, ability_id, target → bool | Return false to reject target |
| `on_ability_effect` | caster, ability_id, target_or_point | Effect fires (cast point / projectile impact) |
| `on_ability_cast` | caster, ability_id, target | Full cast cycle completed |
| `on_toggle_activate` | caster, ability_id | |
| `on_toggle_deactivate` | caster, ability_id | |
| `on_channel_start` | caster, ability_id | |
| `on_channel_end` | caster, ability_id, completed | completed = true if finished normally |

#### Combat Events

| Event | Arguments | Notes |
|-------|-----------|-------|
| `on_damage` | source, target, amount, context | Context has attack_type, armor_type, etc. Map can modify amount |
| `on_death` | unit, killer | |
| `on_attack` | attacker, target | Normal attack lands |

#### Unit Lifecycle Events

| Event | Arguments | Notes |
|-------|-----------|-------|
| `on_unit_created` | unit | After entity creation |
| `on_unit_removed` | unit | Before entity cleanup |

#### Game Events

| Event | Arguments | Notes |
|-------|-----------|-------|
| `on_game_start` | | After map fully loaded, before first tick |
| `on_game_tick` | dt | Every simulation tick (use sparingly) |

## Timer System

Lua scripts create timers for periodic effects, delayed actions, etc.

```lua
-- Create a one-shot timer (fires once after delay)
local t = CreateTimer(delay, false, function()
    -- do something after delay
end)

-- Create a repeating timer
local t = CreateTimer(interval, true, function()
    if some_condition then
        DestroyTimer(t)
    end
end)

-- Destroy a timer
DestroyTimer(timer)
```

Timers are ticked by the engine each simulation tick. Timer callbacks run synchronously.

## Engine API (common.lua)

The engine API is the set of C++ functions exposed to Lua. They are declared in
`engine/scripts/common.lua` with documentation comments. Map scripts call these.

### Unit API

```lua
-- Creation / destruction
CreateUnit(type_id, player, x, y, facing) → unit
RemoveUnit(unit)
IsUnitAlive(unit) → bool
IsUnitDead(unit) → bool
IsUnitHero(unit) → bool
IsUnitBuilding(unit) → bool

-- Position
GetUnitX(unit) → float
GetUnitY(unit) → float
GetUnitZ(unit) → float
GetUnitPosition(unit) → x, y, z
SetUnitPosition(unit, x, y)
GetUnitFacing(unit) → float
SetUnitFacing(unit, facing)

-- Health
GetUnitHealth(unit) → float
GetUnitMaxHealth(unit) → float
SetUnitHealth(unit, hp)

-- State (map-defined: mana, energy, etc.)
GetUnitState(unit, state_id) → float
GetUnitMaxState(unit, state_id) → float
SetUnitState(unit, state_id, value)

-- Attributes (map-defined: armor, strength, etc.)
GetUnitAttribute(unit, attr_id) → float (numeric)
SetUnitAttribute(unit, attr_id, value)
GetUnitStringAttribute(unit, attr_id) → string
SetUnitStringAttribute(unit, attr_id, value)

-- Owner
GetUnitOwner(unit) → player
SetUnitOwner(unit, player)

-- Classification
HasClassification(unit, flag) → bool
AddClassification(unit, flag)
RemoveClassification(unit, flag)

-- Type
GetUnitTypeId(unit) → string
```

### Order API

```lua
IssueOrder(unit, "move", x, y)
IssueOrder(unit, "attack", target_unit)
IssueOrder(unit, "stop")
IssueOrder(unit, "hold")
IssueOrder(unit, "cast", ability_id, target_unit_or_point)
IssueOrder(unit, "patrol", x, y)
```

### Ability API

```lua
AddAbility(unit, ability_id, level?)
RemoveAbility(unit, ability_id)
ApplyPassiveAbility(target, ability_id, source, duration)
HasAbility(unit, ability_id) → bool
GetAbilityLevel(unit, ability_id) → int
SetAbilityLevel(unit, ability_id, level)
GetAbilityStackCount(unit, ability_id) → int
RefreshAbilityDuration(unit, ability_id, duration?)
GetAbilitySource(unit, ability_id) → unit
GetAbilityCooldown(unit, ability_id) → float
ResetAbilityCooldown(unit, ability_id)
```

### Damage API

```lua
DamageUnit(source, target, amount)
HealUnit(source, target, amount)
KillUnit(unit, killer?)
```

### Hero API

```lua
GetHeroLevel(unit) → int
SetHeroLevel(unit, level)
AddHeroXP(unit, xp)
GetHeroXP(unit) → int
```

### Player API

```lua
GetPlayer(slot) → player
GetPlayerName(player) → string
IsPlayerAlly(player1, player2) → bool
IsPlayerEnemy(player1, player2) → bool
```

### Spatial Query API

```lua
-- Filter table: { owner=player, enemy_of=player, classifications={"ground"}, alive_only=true }
GetUnitsInRange(x, y, radius, filter?) → unit[]
GetUnitsInRect(x, y, width, height, filter?) → unit[]
GetNearestUnit(x, y, radius, filter?) → unit or nil
```

### Timer API

```lua
CreateTimer(delay, repeating, callback) → timer
DestroyTimer(timer)
```

### Utility API

```lua
GetGameTime() → float           -- in-game elapsed time
GetGameSpeed() → float          -- speed multiplier
SetGameSpeed(speed)
DisplayMessage(text, duration?)  -- show text on screen
Log(text)                        -- print to engine log

-- Math helpers
GetDistanceBetween(unit1, unit2) → float
GetAngleBetween(unit1, unit2) → float
RandomInt(min, max) → int
RandomFloat(min, max) → float
```

### Region API

```lua
GetRegion(name) → region
IsUnitInRegion(unit, region) → bool
GetUnitsInRegion(region, filter?) → unit[]
GetRegionCenter(region) → x, y
```

## Sandboxing

Map scripts are sandboxed:
- No filesystem access (no `io`, `os.execute`, `loadfile`)
- No `require` — all map scripts are loaded by the engine from the map package
- `print()` redirects to engine log
- Engine API is the only way to interact with the game world
- Infinite loop protection: engine counts instructions per Lua call, aborts if limit exceeded

## Error Handling

- Lua errors are caught by sol2's protected call mechanism
- Errors are logged with script file + line number
- The game continues running — one bad event handler doesn't crash the map
- Event handlers that error are skipped for subsequent calls until re-registered
