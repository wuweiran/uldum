# Uldum Engine — Scripting System Design

## Overview

Maps define all gameplay logic via Lua 5.4 scripts. The engine provides a C++ API
exposed to Lua through sol2 bindings. This is analogous to WC3's JASS system, but
using Lua instead of a custom language.

### Script Directory Structure

```
my_map.uldmap/
├── scripts/                  -- map-wide modules (shared across all scenes)
│   ├── combat.lua            -- reusable combat systems
│   └── utils.lua             -- helper functions
└── scenes/
    ├── scene_01/scripts/
    │   └── main.lua          -- scene 01 entry point
    └── scene_02/scripts/
        ├── main.lua          -- scene 02 entry point
        └── hero_setup.lua    -- scene-specific module
```

Each scene has its own `main.lua` entry point. Scripts use `require()` to include
other files from the scene's scripts directory, the map-wide scripts directory, or
the engine scripts directory.

### Key Files

| File | Purpose |
|------|---------|
| `engine/scripts/constants.lua` | Engine-defined constants (event names, priority levels). Available via `require("constants")`. |
| `engine/scripts/api.lua` | API documentation stubs for IDE autocomplete. NOT loaded at runtime. |
| `<map>/scripts/*.lua` | Map-wide modules available to all scenes via `require()`. |
| `<map>/scenes/<scene>/scripts/main.lua` | Per-scene entry point. Defines `main()` function. |

### Execution Order

1. Engine initializes Lua VM (sandboxed — no os, io, dofile, loadfile)
2. Engine configures `package.path`: scene scripts → map scripts → engine scripts
3. Engine configures save data path (`%APPDATA%/Uldum/saves/<map>/`)
4. Engine loads `engine/scripts/constants.lua` (event constants)
5. Engine loads and executes `scenes/<start_scene>/scripts/main.lua`
6. Map's `main.lua` uses `require()` to include shared modules, then defines `main()`
7. Engine calls `main()` — registers event handlers, creates initial game state
8. Game loop begins — events fire, Lua handlers run synchronously within simulation ticks

### require() Search Order

When a script calls `require("combat")`, Lua searches these directories in order:

1. `scenes/<active_scene>/scripts/combat.lua` — scene-specific
2. `<map>/scripts/combat.lua` — map-wide
3. `engine/scripts/combat.lua` — engine-provided

The first match wins. This lets a scene override a map-wide module if needed.

## Lua VM

- **Lua 5.4** with **sol2** for C++ ↔ Lua binding
- One VM per scene (fresh state on scene change)
- **Disabled**: `os`, `io`, `loadfile`, `dofile` (direct filesystem access)
- **Enabled**: `require()` with controlled `package.path` (scene → shared → engine scripts only)
- **Available**: `math`, `string`, `table`, `coroutine`, `package`, `print` (→ engine log)
- Native C module loading disabled (`package.cpath = ""`)
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
TriggerRegisterEvent(trig, EVENT_GLOBAL_DEATH)
TriggerRegisterTimerEvent(trig, 1.0, true)  -- every 1s, repeating

-- Condition (optional — if returns false, action is skipped)
TriggerAddCondition(trig, function()
    return GetTriggerUnit() ~= nil
end)

-- Action (what happens when the trigger fires and conditions pass)
TriggerAddAction(trig, function()
    local event = GetTriggerEvent()
    if event == EVENT_GLOBAL_DEATH then
        Log("Someone died: " .. GetUnitTypeId(GetTriggerUnit()))
    end
end)

-- Custom data scoped to the trigger
trig.data.counter = 0

-- Destroy: all events unregistered, timers stopped, data gone
DestroyTrigger(trig)
```

### Scoped Registration (avoid boilerplate)

Every event comes in a **global** form (fires for any subject) and a **scoped** form (fires only for one subject). Scoped registrations let the action skip its own filtering.

```lua
-- Global: fires for ANY unit death
TriggerRegisterEvent(trig, EVENT_GLOBAL_DEATH)

-- Unit-scoped: fires only when THIS unit dies
TriggerRegisterUnitEvent(trig, my_hero, EVENT_UNIT_DEATH)

-- Player-scoped: fires for any order issued by THIS player
TriggerRegisterPlayerEvent(trig, player, EVENT_PLAYER_ORDER)

-- Projectile-scoped: fires when THIS projectile hits or expires
TriggerRegisterProjectileEvent(trig, proj, EVENT_PROJECTILE_HIT)

-- Region: fires when any unit enters / leaves
TriggerRegisterEnterRegion(trig, region)
TriggerRegisterLeaveRegion(trig, region)

-- Timer: fires on an interval (one-shot or repeating)
TriggerRegisterTimerEvent(trig, 1.0, true)  -- every 1s, repeating
```

Inside the action, `GetTriggerEvent()` returns the event name string; `GetTriggerUnit()` / `GetTriggerPlayer()` / etc. return the subject.

### Trigger Lifecycle

Triggers are destroyed manually via `DestroyTrigger`. If you want cleanup on unit death or ability removal, register an event and destroy the trigger in the handler:

```lua
local trig = CreateTrigger()
TriggerRegisterUnitEvent(trig, my_hero, EVENT_UNIT_DEATH)
TriggerAddAction(trig, function()
    -- hero died, clean up related triggers
    DestroyTrigger(buff_trigger)
    DestroyTrigger(trig)  -- can destroy self
end)
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

-- Items
GetTriggerItem() → item              -- the item involved (pickup/drop)

-- HUD / orders
GetTriggerNode() → string            -- node id for HUD events
GetTriggerOrderType() → string       -- order type for issued-order events
```

### Event List

Event-name constants live in `engine/scripts/constants.lua`. Every event comes in a **global** flavor (registered via `TriggerRegisterEvent`) and most also have a **unit-scoped** flavor (registered via `TriggerRegisterUnitEvent(trig, unit, ...)`).

#### Combat events

| Global | Unit-scoped | Subject (`GetTriggerUnit`) | Notes |
|---|---|---|---|
| `EVENT_GLOBAL_DAMAGE` | `EVENT_UNIT_DAMAGE` | the victim | `GetDamageSource/Target/Amount/Type`; `SetDamageAmount` modifies |
| `EVENT_GLOBAL_DYING` | `EVENT_UNIT_DYING` | the victim | Fires before death is finalized |
| `EVENT_GLOBAL_DEATH` | `EVENT_UNIT_DEATH` | the victim | `GetKillingUnit` returns the killer |
| `EVENT_GLOBAL_HEAL` | `EVENT_UNIT_HEAL` | the healed unit | |
| `EVENT_GLOBAL_ATTACKED` | `EVENT_UNIT_ATTACKED` | the target | `GetAttacker` returns the attacker |

#### Ability events

| Global | Unit-scoped | Notes |
|---|---|---|
| `EVENT_GLOBAL_ABILITY_EFFECT` | `EVENT_UNIT_ABILITY_EFFECT` | The ability's effect resolves (cast-point fire / impact). `GetTriggerAbilityId`, `GetSpellTargetUnit/Point` |
| `EVENT_GLOBAL_ABILITY_ENDCAST` | `EVENT_UNIT_ABILITY_ENDCAST` | Full cast cycle finished (incl. backswing) |
| `EVENT_GLOBAL_ABILITY_CHANNEL` | `EVENT_UNIT_ABILITY_CHANNEL` | Channelled ability state changed |

#### Unit lifecycle

| Global | Notes |
|---|---|
| `EVENT_GLOBAL_UNIT_CREATED` | After entity creation |
| `EVENT_GLOBAL_UNIT_REMOVED` | Before entity cleanup |

#### Orders + selection

| Global | Unit-scoped | Notes |
|---|---|---|
| `EVENT_GLOBAL_ISSUED_ORDER` | `EVENT_UNIT_ISSUED_ORDER` | `GetOrderType`, `GetOrderTargetX/Y`, `GetOrderTargetUnit`, `GetOrderPlayer` |
| `EVENT_GLOBAL_SELECT` | — | Selection changed. `GetSelectedUnits`, `GetSelectedUnitCount` |

#### Items

| Global | Unit-scoped | Notes |
|---|---|---|
| `EVENT_GLOBAL_ITEM_PICKED_UP` | `EVENT_UNIT_ITEM_PICKED_UP` | `GetTriggerItem` returns the item |
| `EVENT_GLOBAL_ITEM_DROPPED` | `EVENT_UNIT_ITEM_DROPPED` | |

#### Projectiles

| Global | Projectile-scoped | Notes |
|---|---|---|
| `EVENT_GLOBAL_PROJECTILE_HIT` | `EVENT_PROJECTILE_HIT` | `GetTriggerProjectile`; `GetTriggerUnit` returns the target |
| `EVENT_GLOBAL_PROJECTILE_DESTROYED` | `EVENT_PROJECTILE_DESTROYED` | Fires when a projectile expires without hitting |

#### Session + player

| Constant | Notes |
|---|---|
| `EVENT_GLOBAL_GAME_END` | Fires after `EndGame()` is called |
| `EVENT_GLOBAL_DISCONNECT` | A peer connection dropped |
| `EVENT_GLOBAL_LEAVE` | A player intentionally left |
| `EVENT_PLAYER_ORDER` / `EVENT_PLAYER_DISCONNECT` / `EVENT_PLAYER_LEAVE` | Player-scoped variants — registered via `TriggerRegisterPlayerEvent` |

#### HUD

| Constant | Notes |
|---|---|
| `EVENT_BUTTON_PRESSED` | Button click on a HUD node. Registered via `TriggerRegisterNodeEvent(trig, node, EVENT_BUTTON_PRESSED)`. `GetTriggerNode`, `GetTriggerPlayer` |

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
SetUnitAbilityLevel(unit, ability_id, level)
GetAbilityStackCount(unit, ability_id) → int
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
Player(slot) → player
GetPlayerName(player) → string
IsPlayerAlly(player1, player2) → bool
IsPlayerEnemy(player1, player2) → bool
```

### Spatial Query API

```lua
-- Filter table: { owner=player, enemy_of=player, classifications={"ground"}, alive_only=true }
GetUnitsInRange(x, y, radius, filter?) → unit[]
GetUnitsInRect(x, y, width, height, filter?) → unit[]
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

### Save Data API (Cross-Scene Persistence)

Data saved via `SaveData` persists to disk (`%APPDATA%/saves/<map_id>/save_data.json`).
It survives scene changes, Lua VM resets, and game restarts. Use it for campaign progress,
player choices, inventory carried between missions.

```lua
-- Save values (flushed to disk immediately)
SaveData("boss_defeated", true)
SaveData("gold_carried", 500)
SaveData("player_name", "Arthas")
SaveData("completion_time", 142.5)

-- Load values (with default if key doesn't exist)
local gold = LoadData("gold_carried", 0)
local name = LoadData("player_name", "Unknown")
local defeated = LoadData("boss_defeated", false)

-- Clear all save data for this map
ClearSaveData()
```

Supported types: boolean, integer, float, string.

### Region API

```lua
GetRegion(name) → region
IsUnitInRegion(unit, region) → bool
GetUnitsInRegion(region, filter?) → unit[]
GetRegionCenter(region) → x, y
```

## Sandboxing

Map scripts are sandboxed:
- No filesystem access (no `io`, `os.execute`, `loadfile`, `dofile`)
- `require()` only searches controlled directories (scene → shared → engine scripts)
- No native C module loading (`package.cpath` is empty)
- `print()` redirects to engine log
- Engine API is the only way to interact with the game world
- `SaveData`/`LoadData` provide controlled persistence via engine-managed JSON files
- Infinite loop protection: engine counts instructions per Lua call, aborts if limit exceeded

## Error Handling

- Lua errors are caught by sol2's protected call mechanism
- Errors are logged with script file + line number
- The game continues running — one bad event handler doesn't crash the map
- Event handlers that error are skipped for subsequent calls until re-registered
