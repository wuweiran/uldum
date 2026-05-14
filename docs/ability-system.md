# Uldum Engine — Ability System Design

## Terminology

- **AbilityDef**: the template/definition. Loaded from map JSON. Has an id, form, per-level data. Read-only.
- **Ability**: the runtime instance on a specific unit. Has current level, cooldown remaining, modifiers, duration, etc. Mutable.
- In Lua, `ability` refers to the instance. The def is looked up by the ability's id string.

## API Names

| Action | Lua API | Notes |
|--------|--------------|-------|
| Add an ability to a unit | `AddAbility(unit, ability_id)` | Adds an Ability instance. If `stackable=false` and an instance already exists, the existing instance's duration refreshes instead. |
| Remove an ability | `RemoveAbility(unit, ability_id)` | Removes **all** instances of that id on the unit. |
| Check if unit has an ability | `HasAbility(unit, ability_id) -> bool` | True if at least one instance is present. |
| Get ability level | `GetAbilityLevel(unit, ability_id)` | 0 = not learned. |
| Set ability level | `SetAbilityLevel(unit, ability_id, level)` | Recalculates modifiers. |

The Lua surface deliberately omits a `source` parameter and any per-instance handles. Auras attribute their applications internally; from a map script's perspective every `AddAbility` call is "the engine added this." Stack counts and per-instance source queries are intentionally not exposed — if you need them later, add a query then.

## Ability Forms

The engine provides the following mechanical forms (`AbilityForm` enum). Each form handles the complex plumbing (timing, targeting, state management). The actual gameplay effect is a Lua trigger that registers against the relevant ability event — see [Engine Events for Scripts](#engine-events-for-scripts) below for the full list.

Channelling is not itself a form. Any active form can carry a `channel_time` on its per-level data, which inserts a sustained-cast phase between the cast-time effect fire and the backswing. Cancelling a channel (by issuing another order) drops the cast clean — no cooldown, no backswing; natural completion runs cooldown + backswing as usual.

### passive_modifier

Always active while on the unit. Contributes numeric modifiers — either unit attributes (damage, armor, move_speed, …) or ability-scoped attributes (cooldown, cast_range, mana_cost, …). The two share the same `modifiers` map; the key namespace is what differs (see [Modifier Surface](#modifier-surface) below).

**Engine handles:**
- Apply/revert contributions on add / remove / level-change via `recalculate_modifiers`
- Optional `duration` on the per-level data — instance auto-removes on expiry

**Map defines:**
- `modifiers`: attribute deltas keyed by attribute name + `_flat` / `_percent` suffix (e.g., `armor_flat: 3`, `move_speed_percent: -25`, `cooldown_percent: -10`). Engine looks up which namespace each key belongs to.

### passive_flag

Always active while on the unit. Toggles one or more **refcounted** status flags (`silenced`, `disarmed`, `invisible`, `rooted`, `no_acquire`, …) for as long as the ability is present.

**Engine handles:**
- On add: increments the per-flag refcount for each listed flag
- On remove: decrements; flag becomes "off" only when the count reaches zero
- All engine sites that previously read a status bitset now query `is_flag_set(unit, flag) = refcount > 0`

**Map defines:**
- `flags`: list of flag names to apply (e.g., `["silenced", "no_acquire"]`)

The refcount semantics mean two abilities both applying `silenced` cleanly compose — removing one leaves silence in effect until the other is also removed. Multiple sources of the same flag never trample each other.

### aura

Periodically scans for nearby units and calls `AddAbility` on the unit with a configured buff ability id. The buff is a `passive_modifier` or `passive_flag` — the aura form itself only handles the broadcast.

**Engine handles:**
- Scan units in radius at a fixed cadence (~0.25s)
- For each matching unit, apply the configured buff (duration ~0.35s — slightly longer than the scan interval so units still in range get refreshed before it decays)
- Units that leave the radius stop being refreshed; the buff decays via duration → modifier/flag contributions revert through normal recalc
- Uses the buff ability's `target_filter` to decide who is affected

**Map defines:**
- `aura_radius`: scan radius (per level)
- `aura_ability`: id of the `passive_modifier` / `passive_flag` to apply

**Multiple auras of the same kind overlap correctly:** the applied buff is typically declared `stackable: false`. Each tick's `AddAbility` call refreshes the existing instance's duration rather than creating a duplicate, so two paladins running Devotion Aura on the same footman do not stack the buff (matching WC3 semantics). For genuinely stacking effects (Veno-style poison), declare the buff `stackable: true`.

### instant

Player or AI activates. No target selection.

**Engine handles:**
- Validate: cooldown ready? state cost affordable?
- Deduct state cost
- `cast_time` (foreswing) → fire effect → `backsw_time` (cancellable)
- Start cooldown

### target

Cast on something in the world — a widget (unit / destructable / item) and/or a ground point. One form covers all "pick a target" shapes; what the cursor accepts is driven by two flags on the AbilityDef:

- `widget_kinds: ["unit", "destructable", "item"]` — bitmask of widget categories the cursor snaps to. Empty array (or omitted) means no widget snap.
- `accept_point: true` — does the cursor fall through to the ground when no widget snaps?

Three resulting shapes from this one form:

| Shape | `widget_kinds` | `accept_point` | Examples |
|---|---|---|---|
| Widget-only | `["unit"]` | omitted / `false` | Holy Light, Hex, Storm Bolt |
| Point-only | `[]` / omitted | `true` | Blizzard, Flame Strike |
| Hybrid (widget-first, point fallback) | `["unit"]` | `true` | Frost Bolt, Cluster Rockets, Chain Lightning |

**Engine handles:**
- Validate: cooldown ready? state cost affordable?
- Target validation: range check, target filter (filter still applies after widget-kind selection)
- Turn to face target
- `cast_time` (foreswing) → fire effect → `backsw_time` (cancellable)
- Optional projectile (if `projectile_speed > 0`): spawn homing projectile (widget cast) or directional projectile (point cast); effect fires on impact

**Hit callback receives both** `target_widget` (may be nil if the cast resolved on a point) and `target_point` (always set — equal to the widget's position when a widget was picked). Lua branches with `if target_widget`.

**Map defines:**
- `widget_kinds` and/or `accept_point` (at least one must be present and non-empty)
- `target_filter`: who can be picked when a widget is under the cursor

## AbilityDef Structure

### Field Defaults

All fields are optional except `form`. Omitted fields use these defaults:

| Field | Default | Notes |
|-------|---------|-------|
| `name` | `""` | Display name |
| `icon` | `""` | Icon texture path |
| `form` | **(required)** | `passive_modifier`, `passive_flag`, `aura`, `instant`, `target` |
| `widget_kinds` | `[]` | Target form only: widget categories the cursor snaps to (`unit` / `destructable` / `item`) |
| `accept_point` | `false` | Target form only: cursor falls through to ground when no widget snaps |
| `stackable` | `false` | If false, duplicate instances refresh duration instead of stacking |
| `max_level` | `1` | Number of learnable levels |
| `target_filter` | `{}` (match all) | Who can be targeted |
| `levels` | `[{}]` | Per-level data array (at least one entry) |
| `hotkey` | `""` | Key for RTS preset (e.g., `"T"` for Holy Light). Empty = no hotkey. |
| `hidden` | `false` | If true, ability is not auto-assigned to a slot (no UI presence). Used for system-level passives like item stat bonuses. |
| `pierces_immune` | `false` | Target form only: bypasses `UNIT_STATUS_MAGIC_IMMUNE` on hostile cast targets. Dispels / purges / hexes set this. |

### Per-Level Field Defaults

Each entry in `levels[]` uses these defaults for omitted fields:

| Field | Default | Notes |
|-------|---------|-------|
| `cost` | `{}` | No cost — free to cast |
| `cost_can_kill` | `false` | Health cost won't kill caster |
| `cooldown` | `0` | No cooldown |
| `range` | `0` | Unlimited range (for instant) or melee range |
| `cast_time` | `0` | No foreswing — instant fire |
| `backsw_time` | `0` | No recovery |
| `damage` | `0` | |
| `heal` | `0` | |
| `modifiers` | `{}` | Attribute deltas keyed by `<attr>_flat` / `<attr>_percent`. Usually paired with `form: passive_modifier`, but a `passive_flag` buff may also declare them when the same conceptual effect bundles flags and stat changes (e.g. Wind Walk: invisible flag + alpha modifier). |
| `flags` | `[]` | Refcounted status flags applied while the ability is active. Usually paired with `form: passive_flag`, but `passive_modifier` may also declare them when the bundle is naturally one effect. |
| `duration` | `-1` | `-1` = permanent (innate), `>= 0` = timed (auto-removed on expiry) |
| `aura_radius` | `0` | Aura form only |
| `aura_ability` | `""` | Aura form: id of the `passive_modifier` / `passive_flag` to broadcast |
| `channel_time` | `0` | 0 means not a channel; > 0 inserts a sustained phase between fire and backswing |
| `projectile_speed` | `0` | 0 = no projectile (instant hit) |

A minimal ability definition only needs `form`:
```json
{ "my_passive_buff": { "form": "passive_modifier", "levels": [{ "modifiers": { "armor_flat": 3 } }] } }
```

### Examples

```json
{
    "holy_light": {
        "name": "Holy Light",
        "icon": "icons/holy_light.ktx2",
        "form": "target",
        "widget_kinds": ["unit"],
        "hotkey": "T",
        "stackable": false,
        "max_level": 3,
        "target_filter": { "ally": true, "self": true, "alive": true },
        "levels": [
            { "cost": { "mana": 65 }, "cooldown": 9, "range": 20, "cast_time": 0.4, "backsw_time": 0.5 },
            { "cost": { "mana": 65 }, "cooldown": 9, "range": 20, "cast_time": 0.4, "backsw_time": 0.5 },
            { "cost": { "mana": 65 }, "cooldown": 9, "range": 20, "cast_time": 0.4, "backsw_time": 0.5 }
        ]
    },
    "devotion_aura": {
        "name": "Devotion Aura",
        "icon": "icons/devotion_aura.ktx2",
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
        "form": "passive_modifier",
        "max_level": 3,
        "levels": [
            { "modifiers": { "armor_flat": 1.5 }, "duration": 0.35 },
            { "modifiers": { "armor_flat": 3.0 }, "duration": 0.35 },
            { "modifiers": { "armor_flat": 4.5 }, "duration": 0.35 }
        ]
    },
    "frost_slow": {
        "name": "Frost Slow",
        "form": "passive_modifier",
        "stackable": false,
        "max_level": 1,
        "levels": [
            { "modifiers": { "move_speed_percent": -25 }, "duration": 3.0 }
        ]
    },
    "windwalk_invisible": {
        "name": "Wind Walk",
        "form": "passive_flag",
        "stackable": false,
        "max_level": 1,
        "levels": [
            { "flags": ["invisible", "no_acquire"], "duration": 10.0 }
        ]
    },
    "item_armor_bonus": {
        "name": "Armor Bonus",
        "form": "passive_modifier",
        "hidden": true,
        "stackable": true,
        "max_level": 1,
        "levels": [
            { "modifiers": { "armor_flat": 5 } }
        ]
    },
    "immolation": {
        "name": "Immolation",
        "form": "instant",
        "hotkey": "N",
        "max_level": 3,
        "levels": [
            { "cost": { "mana": 25 } },
            { "cost": { "mana": 25 } },
            { "cost": { "mana": 25 } }
        ]
    }
}
```

Toggle-style abilities (Immolation, Defend, Permanent Invisibility) are authored as plain `instant` abilities. The map's Lua script flips a per-unit "active" flag on cast, applies / revokes the effect (e.g. an aura passive), and runs its own periodic timer for any mana drain. The engine doesn't bake the on/off pattern in.

Spellbook-style "icon that opens a sub-bar of contained abilities" (class kits, hero ult selection, item-granted spell groups) is also a composition, not a form. The button is an `instant` ability whose Lua callback toggles a HUD sub-bar node's visibility (`ShowNode` / `HideNode`); the contained abilities are added to the unit normally (`AddAbility`) and laid out on that sub-bar in `hud.json`. Cooldowns / costs / casts run through the regular pipeline; the spellbook button is just a UI gate. Active and passive abilities can both live inside — WC3's passive-only restriction was an implementation artifact, not an inherent constraint.

## Ability Slots

Units have a fixed-size slot array (16 slots max, engine constant). Slots
connect abilities to the UI and input system. See `docs/input-system.md`
for the full input/UI design.

### Initial Assignment

When a unit is created, its `"abilities"` list determines initial slot layout:

```json
"abilities": ["holy_light", "divine_shield", "devotion_aura", "resurrection"]
```

→ slot 0 = holy_light, slot 1 = divine_shield, slot 2 = devotion_aura,
  slot 3 = resurrection, slots 4-15 = empty.

### Runtime Slot Management (Lua API)

| Function | Behavior |
|----------|----------|
| `AddAbility(unit, id)` | Add ability. Auto-slots to first empty slot if not hidden. |
| `RemoveAbility(unit, id)` | Remove ability entirely. Clears its slot if it had one. |
| `SetAbilitySlot(unit, id, slot)` | Move an existing ability to a specific slot. |
| `ClearSlot(unit, slot)` | Unslot whatever is in that slot (ability stays on unit). |
| `UnslotAbility(unit, id)` | Unslot a specific ability by ID (ability stays on unit). |
| `SwapSlots(unit, a, b)` | Swap two slots. Either or both can be empty. |
| `GetAbilitySlot(unit, id)` | Returns slot index, or -1 if not slotted. |
| `GetSlotAbility(unit, slot)` | Returns ability ID, or nil if empty. |

### Slot Rules

- `RemoveAbility` clears the slot without shifting — other slots keep their indices.
- `AddAbility` auto-assigns to the first empty slot if `hidden` is false.
- If all slots are full, the ability still gets added (functional) but not slotted.
- `hidden` abilities bypass auto-slot assignment. They function normally but have no UI presence.
- Aura-applied buffs (transient `passive_modifier` / `passive_flag` instances added by `aura` ticks) are never slotted.
- Passive abilities that are not hidden CAN be slotted — they show icon/tooltip in the UI.

### Hotkey

The `"hotkey"` field on an ability definition is used by the RTS input preset.
When the player presses a key, the preset checks all slotted abilities on the
selected unit(s) for a matching hotkey. If found, the ability is activated
(instant-cast, or enter targeting mode for targeted forms).

The RPG preset ignores `"hotkey"` — it uses slot-based key mapping instead
(slot 0 = first key, slot 1 = second key, etc.). See `docs/input-system.md`.

## Ability Instance (Runtime)

```
Ability {
    ability_id: string          // references AbilityDef
    level: u32                  // current level (0 = not learned, 1+ = active)
    cooldown_remaining: f32     // ticks down each frame
    auto_cast: bool             // auto-cast enabled

    remaining_duration: f32     // -1 = permanent, >= 0 = timed
    tick_timer: f32             // countdown to next periodic tick

    // Active modifiers (computed from def at current level)
    active_modifiers: map<string, f32>
}
```

No `source` field. Stacking and refresh decisions use only `(ability_id, target)` — see [aura](#aura) for the per-tick refresh semantics.

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
4. cast_time (foreswing) — unit is committed, can't cancel
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

`passive_modifier` abilities declare attribute deltas in a flat `modifiers` map. Each key carries its composition rule in the suffix:

- `<attr>_flat` — additive (sums across all active instances)
- `<attr>_percent` — percentage of base (sums and composes via `(1 + sum/100)`)

```json
"modifiers": {
    "armor_flat": 3,
    "move_speed_percent": -25,
    "attack_speed_percent": 15,
    "cooldown_percent": -10
}
```

### Two attribute namespaces

The engine routes keys to one of two namespaces based on the attribute name. Authors don't pick the namespace — it's an attribute property.

| Namespace | Scope | Attributes |
|---|---|---|
| **unit** | the carrier's own stats | `damage`, `armor`, `move_speed`, `max_hp`, `hp_regen`, `max_mana`, `mana_regen`, `sight_range`, `true_sight`, `acquire_range`, `attack_range`, `attack_speed`, `magic_resistance`, `damage_taken`, `scale`, `visual_alpha` |
| **ability** | every ability on the carrier | `cooldown`, `cast_range`, `mana_cost`, `cast_time`, `duration` |

To scope an ability-namespace modifier to specific ability ids, dot-qualify the key:

```json
"modifiers": {
    "wind_walk.cooldown_flat": -2,
    "cooldown_percent": -10
}
```

The first row applies only to `wind_walk`'s cooldown; the second applies to every ability on the carrier.

### Recalculation

When an ability is added / removed / leveled, the engine recomputes effective values for the affected attributes:

```
effective(attr) = base(attr) + sum(*_flat contributions)
                            * (1 + sum(*_percent contributions) / 100)
```

Systems read effective values, not base values. The recompute is keyed off attribute name so unrelated stats aren't disturbed.

## Modifier Surface

The full list of properties an ability can modify, what kind of modifier each one accepts, and which form expresses it.

### Unit-namespace attributes (`passive_modifier`)

| Attribute | `_flat` | `_percent` | Notes |
|---|---|---|---|
| `damage` | ✓ | ✓ | mult covers crit-amp passives |
| `attack_range` | ✓ | — | |
| `attack_speed` | — | ✓ | scales `attack_cooldown` |
| `acquire_range` | ✓ | — | numeric only; on/off goes to the `no_acquire` flag |
| `move_speed` | ✓ | ✓ | slow is `_percent`, boots are `_flat` |
| `turn_rate` | — | ✓ | rare |
| `max_hp` | ✓ | — | Strength items |
| `hp_regen` | ✓ | — | |
| `damage_taken` | — | ✓ | defensive auras |
| `max_mana` | ✓ | — | Intelligence |
| `mana_regen` | ✓ | — | |
| `sight_range` | ✓ | — | |
| `true_sight` | ✓ | — | gem |
| `armor` | ✓ | — | Faerie Fire = −2 |
| `magic_resistance` | ✓ | ✓ | |
| `scale` | — | ✓ | Grow / Shrink |
| `visual_alpha` | — | ✓ | each source contributes a factor; final = product |

### Ability-namespace attributes (`passive_modifier`)

| Attribute | `_flat` | `_percent` | Notes |
|---|---|---|---|
| `cooldown` | ✓ | ✓ | Aghs-style ult cooldown reduction |
| `cast_range` | ✓ | ✓ | |
| `mana_cost` | ✓ | ✓ | |
| `cast_time` | — | ✓ | |
| `duration` | — | ✓ | scales the duration of effects this ability applies |

### Flags (`passive_flag`)

All refcounted booleans — flag is "on" while at least one source applies it.

| Flag | Engine behavior |
|---|---|
| `stunned` | skip move / attack / cast / turn |
| `silenced` | reject cast orders |
| `disarmed` | reject auto-attack and explicit attack orders |
| `rooted` | movement system skips translation; reject blink-class abilities |
| `paused` | freeze all systems |
| `sleeping` | composite like stunned (wake on damage) |
| `no_acquire` | block auto-acquire scan; drop current auto-acquired target |
| `invulnerable` | `deal_damage` short-circuits |
| `magic_immune` | reject hostile casts, unless `pierces_immune` |
| `untargetable` | reject clicks / direct casts / picker |
| `unattackable` | reject auto-attack acquire and attack orders |
| `ethereal` | composite — typically `untargetable` + a `damage_taken_percent` shift |
| `invisible` | per-player visibility cull |

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
| `on_ability_cast_filter` | Before cast (after engine checks) | caster, ability_id, target → return bool |
| `on_ability_target_filter` | Target validation (after engine filter) | caster, ability_id, target → return bool |
| `on_ability_effect` | Effect fires (cast point / projectile impact) | caster, ability_id, target_or_point |
| `on_ability_cast` | Active ability finishes full cast cycle | caster, ability_id, target |
| `on_channel_start` | Channel begins | caster, ability_id |
| `on_channel_end` | Channel finished/interrupted | caster, ability_id, completed |
| `on_damage` | Damage dealt | source, target, amount, context |
| `on_death` | Unit dies | unit, killer |
| `on_attack` | Attack hits | attacker, target |

No `on_ability_added` / `on_ability_removed` lifecycle events — buffs are pure data (modifiers, flags, duration). The engine applies and removes them, including natural duration-expiry, with no script involvement. If a buff needs a per-tick side effect (DoT damage, periodic visual), that should be expressed as a property of the buff definition rather than as a Lua trigger attached to add/remove events.

### Examples

Using the trigger system (see docs/scripting.md for full trigger API):

```lua
-- Holy Light: heal on effect (scoped to specific unit + ability)
function SetupHolyLight(hero)
    local trig = CreateTrigger()
    TriggerRegisterUnitAbilityEvent(trig, hero, "holy_light", "on_ability_effect")
    TriggerAddAction(trig, function()
        local target = GetSpellTargetUnit()
        local heal = 200 * GetAbilityLevel(hero, "holy_light")
        HealUnit(hero, target, heal)
    end)
    return trig  -- caller manages lifecycle
end
```

### Stacking API

Only `HasAbility(unit, ability_id) -> bool` is exposed. Stack count and per-instance source are deliberately not part of the Lua surface; the engine handles stacking via the buff's `stackable` field and refresh-on-add semantics, so map scripts don't need to inspect instances. If a future ability genuinely needs a stack count, add the query then.

## Engine Events for Scripts

Global events that map scripts can hook (via trigger system in Phase 8).
These are fire-and-forget — they notify, they don't control.

| Event | Fires when | Context |
|-------|-----------|---------|
| `on_ability_cast` | An active ability finishes casting | caster, ability_id, target |

Buff add / remove / expiry deliberately do not fire script events — buffs are pure data and the engine owns their full lifecycle. Stacking is handled by the engine via the buff's `stackable` field; map scripts do not inspect or react to instance lifecycle.

## Script Binding Design

All ability logic lives in Lua — the JSON definition is purely mechanical data (no script
references). Map scripts register per-ability callbacks using the Registration API above.

The engine validates built-in checks first (cooldown, cost, range, target_filter), then
calls the registered Lua callback if one exists for that ability id. If no callback is
registered, the engine just runs the mechanical part (e.g., a passive with modifiers needs
no Lua at all).

## AoE-around-caster Indicator (`IndicatorShape::AreaSelf`)

`IndicatorShape` only renders for `target`-form abilities that hit the ground. Instant abilities with an area effect (Thunder Clap, War Stomp) need their own indicator — the player can't see the radius before casting.

`IndicatorShape::AreaSelf` is valid on `instant`. Geometry is a disc centered on the caster with radius from `level.area.radius`. Doesn't move with a cursor (it's caster-anchored).

**Preview trigger** — same affordance for all indicator shapes (range circles too, not just AreaSelf):

- Desktop: hover the ability's action_bar slot for ~300ms → indicator shows under the caster until the cursor leaves.
- Mobile: long-press the action_bar slot → indicator shows until release.

One generic "preview-on-hover" path in the HUD covers every indicator shape.

## Render Integrations

Two categories, split by who owns the visual.

**Engine-side (multiplayer-aware).** Visibility states the engine must know about because they're per-player. Invisibility (Wind Walk, Permanent Invisibility) lives here — engine hides the unit from non-allied players, reveals on attack / cast / damage taken (or on entering the vision of a true-sight unit). Same family as fog of war (see [scripting.md](scripting.md)).

**Map-driven (via abilities).** Per-unit visual modulation goes through `passive_modifier` on visual attributes (`scale_percent`, `visual_alpha_percent`) — multiple effects compose cleanly via the same recalc as combat stats. Authoring example: a Wind Walk fade declares a `passive_modifier` with `visual_alpha_percent: -50` and a 1 s duration; expiry restores alpha automatically.

`visual_alpha` is now ability-driven: `recalculate_modifiers` sums `visual_alpha_percent` contributions from every active instance on the unit and writes the clamped result into `Renderable::visual_alpha`. Buffs declare e.g. `"modifiers": { "visual_alpha_percent": -50 }` for a half-translucent ghost; multiple sources sum (two −50 sources stack to fully invisible). `SetUnitAlpha` / `GetUnitAlpha` are gone from the Lua surface.

Other direct setters (`SetUnitColor`, `SetUnitShaderVariant`, `EnableUnitAcquire`, `SetUnitStatus` for ability-domain flags, `SetUnitAttribute` for modifier-domain attributes) are slated for the same treatment as their effects migrate to the modifier system. Imperative-only operations (`SetUnitPosition`, `SetUnitFacing`, `SetUnitHealth`, `DamageUnit`, `HealUnit`) are not deprecated — they are actions, not modifiers.

Engine still does **not** bake state visuals (ethereal-blue, petrified-grey, on-fire-glow) into ability forms — those compose from a `passive_modifier` plus a particle effect attached to the buff.
