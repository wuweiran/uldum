# Uldum Engine — Ability System Design

## Terminology

- **AbilityDef**: the template/definition. Loaded from map JSON. Has an id, form, per-level data. Read-only.
- **Ability**: the runtime instance on a specific unit. Has current level, cooldown remaining, modifiers, duration, etc. Mutable.
- In Lua, `ability` refers to the instance. The def is looked up by the ability's id string.

## API Names

| Action | C++ / Lua API | Notes |
|--------|--------------|-------|
| Give a unit an ability | `AddAbility(unit, ability_id)` | Adds a new Ability instance to the unit's AbilitySet |
| Remove an ability | `RemoveAbility(unit, ability_id)` | Removes the instance, reverts modifiers |
| Apply a passive to a target (buff) | `ApplyPassiveAbility(target, ability_id, source, duration)` | Creates a passive Ability instance with a duration. Source = the unit that applied it |
| Check if unit has ability | `HasAbility(unit, ability_id)` | Returns true if any instance matches |
| Get ability level | `GetAbilityLevel(unit, ability_id)` | 0 = not learned |
| Set ability level | `SetAbilityLevel(unit, ability_id, level)` | Changes level, recalculates modifiers |

## Ability Forms

The engine provides ~7 mechanical forms. Each handles the complex plumbing (timing, targeting, state management). The actual gameplay effect is either declarative (modifiers) or a Lua callback.

### passive

Always active while the ability is on the unit. No casting required.

**Engine handles:**
- Apply/revert attribute modifiers when ability is added/removed/leveled
- Optional periodic tick at `tick_interval` → calls Lua `on_tick` callback
- Optional duration (`remaining_duration`) — auto-removed when expired
- This is also the form used for buffs/debuffs applied via `ApplyPassiveAbility()`

**Map defines:**
- `modifiers`: attribute changes (e.g., `move_speed_percent: -25`, `armor_flat: 3`)
- `tick_interval`: if > 0, engine calls `on_tick` at this rate (Lua callback)
- `on_tick(unit, ability)`: Lua function called each tick interval
- `on_add(unit, ability, source)`: Lua callback when this passive is applied to a unit
- `on_remove(unit, ability)`: Lua callback when this passive is removed

**Stacking:** Controlled by `stackable` field on the AbilityDef:
- `stackable: false` (default) — `AddAbility` and `ApplyPassiveAbility` check if already present. If yes, refresh duration instead of adding another instance.
- `stackable: true` — always adds a new instance. Modifiers stack additively. Item passive abilities (e.g., +5 armor from multiple items) use this.
- Map scripts can override via `on_ability_added` event for custom logic (e.g., "stack up to 3 times, then reject").

### aura

Periodically scans for nearby units and applies/removes a passive ability.

**Engine handles:**
- Scan units in radius at a fixed interval (default `aura_interval = 0.25s`, uniform for all auras)
- Apply the specified passive to units that enter the radius
- Remove (or let expire) from units that leave the radius
- Uses the target filter to decide who is affected

**Map defines:**
- `aura_radius`: scan radius (per level)
- `aura_ability`: id of the passive ability to apply to nearby units
- `aura_duration`: how long the applied passive lasts (should be slightly > aura_interval so it refreshes)

### instant

Player or AI activates. No target selection.

**Engine handles:**
- Validate: cooldown ready? state cost affordable? (+ Lua `can_cast` stub)
- Deduct state cost
- Cast point (foreswing) → fire effect → backswing (cancellable)
- Start cooldown

**Map defines:**
- `on_effect(caster, ability)`: Lua callback when effect fires

### target_unit

Like instant, but requires selecting a unit target.

**Engine handles:**
- All of instant, plus:
- Target validation: range check, target filter (+ Lua `can_target` stub)
- Turn to face target
- Optional projectile (if `projectile_speed > 0`): spawn homing projectile, effect fires on impact

**Map defines:**
- `on_effect(caster, target, ability)`: Lua callback
- `target_filter`: who can be targeted

### target_point

Like instant, but requires selecting a ground position.

**Engine handles:**
- Range check to the point
- Cast point → fire effect → backswing

**Map defines:**
- `on_effect(caster, point, ability)`: Lua callback

### toggle

On/off ability. May drain a state while active.

**Engine handles:**
- Toggle state management
- Periodic state cost drain while active (`toggle_cost_per_sec`)
- Auto-deactivate when state runs out

**Map defines:**
- `on_activate(caster, ability)`: Lua callback
- `on_deactivate(caster, ability)`: Lua callback
- `toggle_cost_per_sec`: map of state_name → amount per second

### channel

Sustained cast. Ticks over duration. Interrupted by stun or new order.

**Engine handles:**
- Cast point → begin channel
- Tick at `channel_interval` for `channel_duration`
- Interrupt on stun, movement order, or another cast
- Backswing after channel ends (if not interrupted)

**Map defines:**
- `on_channel_tick(caster, ability, tick_number)`: Lua callback per interval
- `on_channel_end(caster, ability, completed)`: Lua callback (completed = true if finished, false if interrupted)
- `channel_duration`, `channel_interval`: per level

## AbilityDef Structure

### Field Defaults

All fields are optional except `form`. Omitted fields use these defaults:

| Field | Default | Notes |
|-------|---------|-------|
| `name` | `""` | Display name |
| `icon` | `""` | Icon texture path |
| `form` | **(required)** | passive, aura, instant, target_unit, target_point, toggle, channel |
| `stackable` | `false` | If false, duplicate instances refresh duration instead of stacking |
| `max_level` | `1` | Number of learnable levels |
| `target_filter` | `{}` (match all) | Who can be targeted |
| `levels` | `[{}]` | Per-level data array (at least one entry) |

### Per-Level Field Defaults

Each entry in `levels[]` uses these defaults for omitted fields:

| Field | Default | Notes |
|-------|---------|-------|
| `cost` | `{}` | No cost — free to cast |
| `cost_can_kill` | `false` | Health cost won't kill caster |
| `cooldown` | `0` | No cooldown |
| `range` | `0` | Unlimited range (for instant) or melee range |
| `cast_point` | `0` | No foreswing — instant fire |
| `backswing` | `0` | No recovery |
| `damage` | `0` | |
| `heal` | `0` | |
| `modifiers` | `{}` | No attribute modifiers |
| `aura_radius` | `0` | Aura form only |
| `aura_ability` | `""` | Aura form: passive ability id to apply |
| `channel_duration` | `0` | Channel form only — 0 means not a channel |
| `toggle_cost_per_sec` | `{}` | Toggle form: no drain |
| `projectile_speed` | `0` | 0 = no projectile (instant hit) |

A minimal ability definition only needs `form`:
```json
{ "my_passive_buff": { "form": "passive", "levels": [{ "modifiers": { "armor_flat": 3 } }] } }
```

### Examples

```json
{
    "holy_light": {
        "name": "Holy Light",
        "icon": "icons/holy_light.png",
        "form": "target_unit",
        "stackable": false,
        "max_level": 3,
        "target_filter": { "ally": true, "self": true, "alive": true },
        "levels": [
            { "cost": { "mana": 65 }, "cooldown": 9, "range": 20, "cast_point": 0.4, "backswing": 0.5 },
            { "cost": { "mana": 65 }, "cooldown": 9, "range": 20, "cast_point": 0.4, "backswing": 0.5 },
            { "cost": { "mana": 65 }, "cooldown": 9, "range": 20, "cast_point": 0.4, "backswing": 0.5 }
        ]
    },
    "devotion_aura": {
        "name": "Devotion Aura",
        "icon": "icons/devotion_aura.png",
        "form": "aura",
        "max_level": 3,
        "target_filter": { "ally": true },
        "levels": [
            { "aura_radius": 30, "aura_ability": "devotion_aura_buff" },
            { "aura_radius": 30, "aura_ability": "devotion_aura_buff" },
            { "aura_radius": 30, "aura_ability": "devotion_aura_buff" }
        ]
    },
    "devotion_aura_buff": {
        "name": "Devotion Aura",
        "form": "passive",
        "max_level": 3,
        "levels": [
            { "modifiers": { "armor_flat": 1.5 } },
            { "modifiers": { "armor_flat": 3.0 } },
            { "modifiers": { "armor_flat": 4.5 } }
        ]
    },
    "frost_slow": {
        "name": "Frost Slow",
        "form": "passive",
        "stackable": false,
        "max_level": 1,
        "levels": [
            { "modifiers": { "move_speed_percent": -25 } }
        ]
    },
    "item_armor_bonus": {
        "name": "Armor Bonus",
        "form": "passive",
        "stackable": true,
        "max_level": 1,
        "levels": [
            { "modifiers": { "armor_flat": 5 } }
        ]
    },
    "immolation": {
        "name": "Immolation",
        "form": "toggle",
        "max_level": 3,
        "levels": [
            { "toggle_cost_per_sec": { "mana": 5 }, "aura_radius": 10, "aura_ability": "immolation_damage" },
            { "toggle_cost_per_sec": { "mana": 5 }, "aura_radius": 10, "aura_ability": "immolation_damage" },
            { "toggle_cost_per_sec": { "mana": 5 }, "aura_radius": 10, "aura_ability": "immolation_damage" }
        ]
    }
}
```

## Ability Instance (Runtime)

```
Ability {
    ability_id: string          // references AbilityDef
    level: u32                  // current level (0 = not learned, 1+ = active)
    cooldown_remaining: f32     // ticks down each frame
    auto_cast: bool             // auto-cast enabled
    toggle_active: bool         // for toggle form
    
    // Applied passive (buff) fields
    source: Unit                // who applied this (null if innate/self)
    remaining_duration: f32     // -1 = permanent, >= 0 = timed
    tick_timer: f32             // countdown to next periodic tick
    
    // Active modifiers (computed from def at current level)
    active_modifiers: map<string, f32>
}
```

## Cast Flow (Active Forms)

```
1. Player issues Cast order (ability_id + target)
2. Engine checks:
   a. Cooldown ready?
   b. State cost affordable? (check each state >= cost, health cost won't kill unless cost_can_kill)
   c. Target passes filter? (engine basic check)
   d. Lua can_cast(caster, ability, target) returns true? (custom validation)
   e. In range?
3. Turn to face target (if unit-targeted)
4. Cast point (foreswing) — unit is committed, can't cancel
5. FIRE:
   - Deduct state costs
   - Start cooldown
   - If projectile_speed > 0: spawn projectile, Lua on_effect fires on impact
   - Else: Lua on_effect fires immediately
6. Backswing (recovery) — cancellable by new order
```

## State Cost

Ability cost references map-defined states by name:

```json
"cost": { "mana": 75, "health": 50 }
```

- Engine checks `state.current >= cost` for each entry
- For health cost: default behavior is the ability **cannot** kill the caster. Set `"cost_can_kill": true` to allow suicide casts.
- Custom validation beyond this → Lua `can_cast` stub

## Modifier System

Passive abilities can declare attribute modifiers:

```json
"modifiers": {
    "armor_flat": 3,
    "move_speed_percent": -25,
    "attack_speed_percent": 15
}
```

Convention: `_flat` = additive, `_percent` = percentage of base.

When modifiers change (ability added/removed/leveled), the engine recalculates:
```
effective_value = base_value + sum(flat modifiers) * (1 + sum(percent modifiers) / 100)
```

Systems read effective values, not base values.

## Aura Scanning

- All auras share a **uniform scan interval** (engine constant, e.g., every 8 ticks = 0.25s at 32Hz)
- Each aura scan: find units in radius matching filter, apply the aura's passive ability with a short duration (slightly longer than the scan interval, e.g., 0.35s)
- Units that leave the radius: the applied passive expires naturally
- Units that stay: the applied passive is refreshed each scan

## Lua Scripting

All ability logic is handled through **one unified event system**. The JSON ability
definition contains no script references — only mechanical data. If no event handler
is registered, the engine just runs the mechanical part (modifiers, timing, etc.).

### Event Registration

```lua
-- One system for everything
RegisterEvent(event_name, handler_fn)
```

### Events

| Event | Fires when | Arguments |
|-------|-----------|-----------|
| `on_ability_added` | Ability added to any unit (including buffs) | unit, ability_id, source |
| `on_ability_removed` | Ability removed from any unit (including expiry) | unit, ability_id |
| `on_ability_cast_filter` | Before cast (after engine checks) | caster, ability_id, target → return bool |
| `on_ability_target_filter` | Target validation (after engine filter) | caster, ability_id, target → return bool |
| `on_ability_effect` | Effect fires (cast point / projectile impact) | caster, ability_id, target_or_point |
| `on_ability_cast` | Active ability finishes full cast cycle | caster, ability_id, target |
| `on_toggle_activate` | Toggle turned on | caster, ability_id |
| `on_toggle_deactivate` | Toggle turned off | caster, ability_id |
| `on_channel_start` | Channel begins | caster, ability_id |
| `on_channel_end` | Channel finished/interrupted | caster, ability_id, completed |
| `on_damage` | Damage dealt | source, target, amount, context |
| `on_death` | Unit dies | unit, killer |
| `on_attack` | Attack hits | attacker, target |

### Examples

Using the trigger system (see docs/scripting.md for full trigger API):

```lua
-- Holy Light: heal on effect (scoped to specific ability)
function SetupHolyLight(hero)
    local trig = CreateTrigger()
    TriggerBindToAbility(trig, hero, "holy_light")
    TriggerRegisterUnitAbilityEvent(trig, hero, "holy_light", "on_ability_effect")
    TriggerAddAction(trig, function()
        local target = GetSpellTargetUnit()
        local heal = 200 * GetAbilityLevel(hero, "holy_light")
        HealUnit(hero, target, heal)
    end)
end

-- Custom cast filter: Holy Light only at night
function SetupHolyLightFilter(hero)
    local trig = CreateTrigger()
    TriggerBindToAbility(trig, hero, "holy_light")
    TriggerRegisterUnitAbilityEvent(trig, hero, "holy_light", "on_ability_cast_filter")
    TriggerAddCondition(trig, function()
        return IsNight()
    end)
end

-- Periodic damage: poison sting via timer, bound to ability lifecycle
RegisterEvent("on_ability_added", function(unit, ability_id, source)
    if ability_id == "poison_sting" then
        local trig = CreateTrigger()
        TriggerBindToAbility(trig, unit, "poison_sting")
        TriggerRegisterTimerEvent(trig, 1.0, true)
        TriggerAddAction(trig, function()
            DamageUnit(source, unit, 4)
        end)
        -- auto-destroyed when poison_sting is removed from unit
    end
end)

-- Channel: Blizzard ticks via trigger-owned timer
RegisterEvent("on_channel_start", function(caster, ability_id)
    if ability_id == "blizzard" then
        local trig = CreateTrigger()
        TriggerBindToUnit(trig, caster)
        local point_x, point_y = GetSpellTargetPoint()
        TriggerRegisterTimerEvent(trig, 1.0, true)
        TriggerAddAction(trig, function()
            if IsChanneling(caster, "blizzard") then
                CreateAOEDamage(caster, point_x, point_y, 8.0, 50)
            else
                DestroyTrigger(trig)
            end
        end)
    end
end)
```
```

### Stacking API

The engine provides query functions for buff stacking:

```lua
GetAbilityStackCount(unit, ability_id)  -- how many instances of this ability
HasAbility(unit, ability_id)            -- at least one instance exists
GetAbilitySource(unit, ability_id)      -- who applied it (for the first instance)
RefreshAbilityDuration(unit, ability_id, duration)  -- reset timer on existing instance
```

## Engine Events for Scripts

Global events that map scripts can hook (via trigger system in Phase 8).
These are fire-and-forget — they notify, they don't control.

| Event | Fires when | Context |
|-------|-----------|---------|
| `on_ability_added` | Any ability added to any unit (including passives/buffs) | unit, ability_id, source |
| `on_ability_removed` | Any ability removed from any unit (including passive expiry) | unit, ability_id |
| `on_ability_cast` | An active ability finishes casting | caster, ability_id, target |

No separate passive/buff events — `on_ability_added`/`removed` covers all cases.

Example: buff stacking logic via global events:

```lua
RegisterEvent("on_ability_added", function(unit, ability_id, source)
    if ability_id == "frost_slow" then
        -- Only allow one instance: if already present, refresh duration instead
        if GetAbilityStackCount(unit, "frost_slow") > 1 then
            RemoveAbility(unit, "frost_slow")  -- remove the older one
            -- the new one stays with fresh duration
        end
    end
end)
```

## Script Binding Design

All ability logic lives in Lua — the JSON definition is purely mechanical data (no script
references). Map scripts register per-ability callbacks using the Registration API above.

The engine validates built-in checks first (cooldown, cost, range, target_filter), then
calls the registered Lua callback if one exists for that ability id. If no callback is
registered, the engine just runs the mechanical part (e.g., a passive with modifiers needs
no Lua at all).
