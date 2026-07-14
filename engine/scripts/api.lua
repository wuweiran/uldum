--------------------------------------------------------------------------------
-- api.lua — Uldum Engine API Reference (DO NOT LOAD AT RUNTIME)
--
-- This file is documentation and IDE type reference only. It is NOT loaded
-- by the engine. The actual implementations are C++ functions bound via sol2.
-- Runtime constants (events, priorities) are in constants.lua.
--
-- Analogous to Warcraft III's "common.j".
--------------------------------------------------------------------------------

-- NOTE: Function stubs below exist for documentation/IDE autocomplete only.
-- They are NOT executed. Do not load this file — it would overwrite C++ bindings.

--------------------------------------------------------------------------------
-- Triggers
--------------------------------------------------------------------------------

--- Constants (TRIGGER_PRIORITY_*, EVENT_*) are defined in constants.lua.
--- See that file for all available event names and priority levels.

--- Create a trigger — a lifecycle scope for events, conditions, actions, and data.
---@param priority? number   TRIGGER_PRIORITY_* constant. Default: TRIGGER_PRIORITY_NORMAL.
---@return trigger
function CreateTrigger(priority) end

--- Destroy a trigger. All events unregistered, timers stopped, data cleaned up.
---@param trig trigger
function DestroyTrigger(trig) end

--- Register a global event on a trigger.
---@param trig trigger
---@param event_name string
function TriggerRegisterEvent(trig, event_name) end

--- Register an event scoped to a specific unit.
---@param trig trigger
---@param unit unit
---@param event_name string
function TriggerRegisterUnitEvent(trig, unit, event_name) end

--- Register an event scoped to a specific player.
---@param trig trigger
---@param player player
---@param event_name string
function TriggerRegisterPlayerEvent(trig, player, event_name) end

--- Register an event scoped to a specific destructable. Inside the
--- action, GetTriggerDestructable() returns the destructable.
---@param trig trigger
---@param destructable destructable
---@param event_name string  EVENT_DESTRUCTABLE_DEATH
function TriggerRegisterDestructableEvent(trig, destructable, event_name) end

--- Register an event scoped to a specific projectile. The trigger is
--- automatically dropped when the projectile is destroyed.
---@param trig trigger
---@param projectile unit
---@param event_name string  EVENT_PROJECTILE_HIT or EVENT_PROJECTILE_DESTROYED
function TriggerRegisterProjectileEvent(trig, projectile, event_name) end

--- Register a timer event on a trigger.
---@param trig trigger
---@param interval number   seconds
---@param repeating boolean
function TriggerRegisterTimerEvent(trig, interval, repeating) end

--- Add a condition to a trigger. If condition returns false, actions are skipped.
---@param trig trigger
---@param condition function  → bool
function TriggerAddCondition(trig, condition) end

--- Add an action to a trigger. Called when events fire and conditions pass.
---@param trig trigger
---@param action function
function TriggerAddAction(trig, action) end


--------------------------------------------------------------------------------
-- Trigger Context (call inside trigger actions to read event data)
--------------------------------------------------------------------------------

--- Which event caused this trigger to fire.
---@return string
function GetTriggerEvent() end

--- The unit associated with the current event. nil when the trigger
--- widget is a destructable (use GetTriggerDestructable instead).
---@return unit?
function GetTriggerUnit() end

--- The destructable associated with the current event, or nil when the
--- trigger widget is a unit. Set for the widget-level events
--- (damage/dying/death/heal) and EVENT_DESTRUCTABLE_DEATH.
---@return destructable?
function GetTriggerDestructable() end

--- The widget (unit OR destructable) the current event is about,
--- regardless of category. Returns whichever usertype matches, or nil
--- when there is no trigger widget. Use when an action handles both
--- kinds; otherwise prefer GetTriggerUnit / GetTriggerDestructable.
---@return unit|destructable|nil
function GetTriggerWidget() end

--- The player associated with the current event.
---@return player
function GetTriggerPlayer() end

--- The ability id associated with the current event.
---@return string
function GetTriggerAbilityId() end

--- The HUD node id that fired the current node-event trigger
--- (e.g. button_pressed). Empty string outside a node-event action.
---@return string
function GetTriggerNode() end

--- The item the current event is about, or nil. Set during item events
--- (picked_up, dropped) and during ability casts that originated from
--- an item slot. Hook from EVENT_ABILITY_CAST_FINISHED to drive
--- consumption / level-up.
---@return item?
function GetTriggerItem() end

--- The region id of the firing region_enter / region_leave action.
--- Returns UINT32_MAX outside that context.
---@return number
function GetTriggerRegion() end

--- The order type for the current order event. One of: "move",
--- "attack_move", "attack", "stop", "hold", "patrol", "cast", "train",
--- "research", "build", "pickup", "drop", "swap_inventory_slot",
--- "move_direction". For "cast", `GetTriggerAbilityId()` returns the
--- ability id.
---@return string
function GetTriggerOrderType() end

--- Target unit of a unit-targeted ability.
---@return unit
function GetSpellTargetUnit() end

--- Target point of a point-targeted ability.
---@return number, number   x, y
function GetSpellTargetPoint() end

--- Target point X of a point-targeted ability (convenience scalar accessor).
---@return number
function GetSpellTargetX() end

--- Target point Y of a point-targeted ability (convenience scalar accessor).
---@return number
function GetSpellTargetY() end

--- Source of the current damage event.
---@return unit
function GetDamageSource() end

--- Target of the current damage event.
---@return unit
function GetDamageTarget() end

--- Amount of the current damage event.
---@return number
function GetDamageAmount() end

--- Modify damage amount before it's applied.
---@param amount number
function SetDamageAmount(amount) end

--- The damage type of the current damage event (map-defined, e.g. "attack", "spell", "cleave").
---@return string
function GetDamageType() end

--- The unit that killed the current dying unit.
---@return unit
function GetKillingUnit() end

--- The attacker in the current EVENT_UNIT_ATTACKED / EVENT_GLOBAL_ATTACKED
--- event. The target is `GetTriggerUnit()`.
---@return unit
function GetAttacker() end

--------------------------------------------------------------------------------
-- Unit Creation & Lifecycle
--------------------------------------------------------------------------------

--- Create a new unit at the given position.
---@param type_id string   Unit type from map's units.json
---@param player player    Owner
---@param x number         X position (game coords)
---@param y number         Y position (game coords)
---@param facing number?   Facing in degrees (default 0, facing +Y)
---@return unit
function CreateUnit(type_id, player, x, y, facing) end

--- Create a destructable (crate, tree, etc.) at the given position. Z is
--- sampled from the terrain. Returns the handle, or nil if the type id is
--- unknown. Dies via the normal Health/death path; fires the
--- widget-level EVENT_GLOBAL_DEATH plus EVENT_DESTRUCTABLE_DEATH (catch
--- the latter with TriggerRegisterDestructableEvent, read the dying
--- crate via GetTriggerDestructable).
---@param type_id string    Destructable type from map's destructables.json
---@param x number          X position (game coords)
---@param y number          Y position (game coords)
---@param facing number?    Facing in degrees (default 0)
---@param variation number? Model variation index (default 0)
---@return destructable?
function CreateDestructable(type_id, x, y, facing, variation) end

--- Remove a unit from the game world.
---@param unit unit
function RemoveUnit(unit) end

--- Transform a unit into a different type in place. Same handle, same
--- position, same owner. Swaps every type-derived component: model,
--- scale, max HP / regen, state defaults (mana, energy, …),
--- attribute base values, movement, combat, vision, classifications,
--- and the structure-building flag. Carries Health and shared State
--- ids by percentage (60% mana on morph in → 60% of new max). Cancels
--- in-flight cast / attack / movement.
---
--- Abilities are NOT touched. The map manages the kit explicitly:
--- before morphing, RemoveAbility old type's abilities (read cooldowns
--- via GetAbilityCooldown first if you want to preserve them); after
--- morphing, AddAbility new type's abilities (and SetAbilityCooldown
--- to restore). See the morph helper pattern in the engine scripts.
---
--- Returns false if the type id is unknown or the handle is stale.
---@param unit unit
---@param new_type_id string
---@return boolean
function MorphUnit(unit, new_type_id) end

--- Read the cooldown remaining (seconds) on a unit's ability.
--- Returns 0 if the unit doesn't have the ability.
---@param unit unit
---@param ability_id string
---@return number
function GetAbilityCooldown(unit, ability_id) end

--- Set the cooldown remaining (seconds) on a unit's ability. No-op
--- when the unit doesn't have the ability. Used by morph helpers to
--- carry cooldowns across a Remove/Add round-trip.
---@param unit unit
---@param ability_id string
---@param seconds number
function SetAbilityCooldown(unit, ability_id, seconds) end

--- List the ability ids declared in a unit type's `abilities` array.
--- Used by morph helpers that need to iterate the type's kit without
--- duplicating the list in Lua. Returns an empty table if the type id
--- is unknown.
---@param type_id string
---@return string[]
function GetUnitTypeAbilities(type_id) end

--- Check if a unit is alive (HP > 0 and not in dead state).
---@param unit unit
---@return boolean
function IsUnitAlive(unit) end

--- Check if a unit is dead (in dead/corpse state).
---@param unit unit
---@return boolean
function IsUnitDead(unit) end

--- Check if a unit has the "hero" classification.
---@param unit unit
---@return boolean
function IsUnitHero(unit) end

--- Check if a unit has the "structure" classification.
---@param unit unit
---@return boolean
function IsUnitBuilding(unit) end

--- Get the unit's type id string.
---@param unit unit
---@return string
function GetUnitTypeId(unit) end

--- Get the unit's movement layer: "ground" / "air" / "water" / "amphibious".
--- Engine truth (reads Movement.type), so it can't be mistagged like a
--- classification string. Buildings / destructables read as "ground".
---@param unit unit
---@return string
function GetUnitMoveType(unit) end

--------------------------------------------------------------------------------
-- Unit Position & Facing
--------------------------------------------------------------------------------

---@param unit unit
---@return number
function GetUnitX(unit) end

---@param unit unit
---@return number
function GetUnitY(unit) end

---@param unit unit
---@return number
function GetUnitZ(unit) end

---@param unit unit
---@return number, number, number   x, y, z
function GetUnitPosition(unit) end

--- Set unit position on the XY plane. Z is set from terrain height.
---@param unit unit
---@param x number
---@param y number
function SetUnitPosition(unit, x, y) end

---@param unit unit
---@return number   degrees, 0 = facing +Y
function GetUnitFacing(unit) end

---@param unit unit
---@param facing number   degrees, 0 = facing +Y
function SetUnitFacing(unit, facing) end

--------------------------------------------------------------------------------
-- Health (engine built-in state)
--------------------------------------------------------------------------------

---@param unit unit
---@return number
function GetUnitHealth(unit) end

---@param unit unit
---@return number
function GetUnitMaxHealth(unit) end

---@param unit unit
---@param hp number
function SetUnitHealth(unit, hp) end

--------------------------------------------------------------------------------
-- States (map-defined: mana, energy, etc.)
--------------------------------------------------------------------------------

---@param unit unit
---@param state_id string   e.g. "mana"
---@return number
function GetUnitState(unit, state_id) end

---@param unit unit
---@param state_id string
---@return number
function GetUnitMaxState(unit, state_id) end

---@param unit unit
---@param state_id string
---@param value number
function SetUnitState(unit, state_id, value) end

--------------------------------------------------------------------------------
-- Attributes (map-defined: armor, strength, etc.)
--------------------------------------------------------------------------------

--- Get a numeric attribute.
---@param unit unit
---@param attr_id string   e.g. "armor", "strength"
---@return number
function GetUnitAttribute(unit, attr_id) end

--- Set a numeric attribute.
---@param unit unit
---@param attr_id string
---@param value number
function SetUnitAttribute(unit, attr_id, value) end

--- Get a string attribute (e.g. "armor_type").
---@param unit unit
---@param attr_id string
---@return string
function GetUnitStringAttribute(unit, attr_id) end

--- Set a string attribute.
---@param unit unit
---@param attr_id string
---@param value string
function SetUnitStringAttribute(unit, attr_id, value) end

--------------------------------------------------------------------------------
-- Owner & Classification
--------------------------------------------------------------------------------

---@param unit unit
---@return player
function GetUnitOwner(unit) end

---@param unit unit
---@param player player
function SetUnitOwner(unit, player) end

---@param unit unit
---@param flag string   e.g. "ground", "hero", "structure"
---@return boolean
function HasClassification(unit, flag) end

--------------------------------------------------------------------------------
-- Status flags
--------------------------------------------------------------------------------

--- Read a status flag on a unit. Returns false when the unit has no
--- StatusFlags component (treated as "no flags set"). Pass a
--- `UNIT_STATUS_*` constant for `flag` (`UNIT_STATUS_STUNNED`,
--- `UNIT_STATUS_SILENCED`, `UNIT_STATUS_MUTED`, `UNIT_STATUS_DISARMED`,
--- `UNIT_STATUS_ROOTED`, `UNIT_STATUS_INVULNERABLE`,
--- `UNIT_STATUS_MAGIC_IMMUNE`, `UNIT_STATUS_UNTARGETABLE`,
--- `UNIT_STATUS_UNATTACKABLE`, `UNIT_STATUS_PAUSED`,
--- `UNIT_STATUS_INVISIBLE`). See gameplay-model.md `## Status Flags`
--- for the per-flag enforcement semantics.
---
--- **Status mutation is ability-driven.** To apply a status flag,
--- author a `passive_flag` ability whose level grants the flag (e.g.
--- `{ flags = ["stunned"] }`) and call `ApplyPassiveAbility` (timed)
--- or `AddAbility` (innate). Remove via `RemoveAbility`. Dispel
--- ("purify") is the map's own Lua iterating the buff ids it wants
--- to strip and calling RemoveAbility on each — the engine doesn't
--- expose a generic "clear all status".
---@param unit unit
---@param flag string  -- a UNIT_STATUS_* constant
---@return boolean
function GetUnitStatus(unit, flag) end

--------------------------------------------------------------------------------
-- Visuals
--------------------------------------------------------------------------------

--- Play a glTF animation clip on a unit, overriding the engine's
--- per-frame state derivation (idle/walk/attack/etc). The clip name
--- must match a clip in the unit's loaded model. Replaces whatever
--- script-driven clip was previously set (back-to-back calls do not
--- queue — use QueueUnitAnimation for that). When the clip finishes
--- (one-shot) and the queue is empty, the engine takes back over
--- automatically. Death always wins: a dying unit's queue is cleared
--- so the death clip plays.
---@param unit unit
---@param clip string    clip name in the unit's glTF
---@param looping boolean? if true and this clip is the last in the queue, it loops; default false (one-shot)
function SetUnitAnimation(unit, clip, looping) end

--- Append `clip` to the unit's animation queue. If the unit isn't
--- currently script-controlled, this behaves like SetUnitAnimation
--- for the first call. The renderer advances the queue each time
--- the current clip finishes.
---@param unit unit
---@param clip string
function QueueUnitAnimation(unit, clip) end

--- Drop the unit's script-driven animation queue. The engine's
--- per-frame state derivation resumes on the next frame and
--- crossfades back to idle / walk / whatever sim state implies.
---@param unit unit
function ResetUnitAnimation(unit) end

--------------------------------------------------------------------------------
-- Orders
--------------------------------------------------------------------------------

--- Issue an order to a unit.
---@param unit unit
---@param order_type string   "move", "attack", "stop", "hold", "cast", "patrol"
---@param ...                 Order-specific arguments
--- Cast usage:
---   IssueOrder(unit, "cast", ability_id)                 -- instant, no target
---   IssueOrder(unit, "cast", ability_id, target_unit)    -- target_unit ability
---   IssueOrder(unit, "cast", ability_id, x, y)           -- target_point ability
--- The unit will move into range, turn, play cast_point, fire on_ability_effect, then backswing.
function IssueOrder(unit, order_type, ...) end

--------------------------------------------------------------------------------
-- Abilities
--------------------------------------------------------------------------------

--- Add an ability to a unit (innate). Rejects if non-stackable and already present.
---@param unit unit
---@param ability_id string
---@param level number?   default 1
---@return boolean   true if added
function AddAbility(unit, ability_id, level) end

--- Remove an ability from a unit. Reverts modifiers.
---@param unit unit
---@param ability_id string
---@return boolean   true if found and removed
function RemoveAbility(unit, ability_id) end

--- Apply a passive ability (buff) to a target from a source, with duration.
--- Non-stackable abilities refresh duration if already present.
---@param target unit
---@param ability_id string
---@param source unit
---@param duration number   seconds
---@return boolean
function ApplyPassiveAbility(target, ability_id, source, duration) end

---@param unit unit
---@param ability_id string
---@return boolean
function HasAbility(unit, ability_id) end

--------------------------------------------------------------------------------
--- Projectiles (agents — handles, not widgets)
--------------------------------------------------------------------------------

--- Create an idle projectile at `source`'s position. The projectile
--- exists as an agent handle but doesn't move or collide until
--- `EmitProjectileTarget` / `EmitProjectileLoc` is called. This
--- two-stage shape gives you a window to attach
--- `EVENT_PROJECTILE_HIT` / `EVENT_PROJECTILE_DESTROYED` triggers and
--- to set Lua-side side-table state keyed by the handle.
---@param source unit
---@param model string     glTF path (empty falls back to engine "projectile" mesh)
---@return unit            projectile handle (or nil)
function CreateProjectile(source, model) end

--- Launch a homing projectile at `target`. Auto-destroys on impact;
--- fires PROJECTILE_HIT once with the target as `GetTriggerUnit()`.
---@param projectile unit
---@param target unit
---@param speed number
---@param arc_height number?  (reserved; arc rendering pending)
function EmitProjectileTarget(projectile, target, speed, arc_height) end

--- Launch a linear (skillshot) projectile toward `(x,y,z)`. Fires
--- PROJECTILE_HIT once for every unit within `hit_radius` along the
--- path (pierce-by-default). Auto-destroys at `max_distance`. Stops
--- on first hit only if the trigger handler calls
--- `DestroyProjectile(GetTriggerProjectile())`.
---@param projectile unit
---@param x number
---@param y number
---@param z number
---@param speed number
---@param hit_radius number
---@param max_distance number
function EmitProjectileLoc(projectile, x, y, z, speed, hit_radius, max_distance) end

--- Manually destroy a projectile. Fires PROJECTILE_DESTROYED.
---@param projectile unit
function DestroyProjectile(projectile) end

--- Read the damage carried by a projectile. The engine consumes this
--- for auto-attack projectiles; ability projectiles can use it as a
--- payload slot.
---@param projectile unit
---@return number
function GetProjectileDamage(projectile) end

--- Set the damage on a projectile. For `is_attack` projectiles this
--- changes what the engine deals at hit time — intercept in
--- PROJECTILE_HIT to apply crit/lifesteal multipliers before the
--- engine commits damage.
---@param projectile unit
---@param damage number
function SetProjectileDamage(projectile, damage) end

--- True when the engine spawned this projectile from an auto-attack.
--- Filter use: a global PROJECTILE_HIT trigger that only modifies
--- auto-attack damage without touching ability projectiles.
---@param projectile unit
---@return boolean
function IsProjectileNormalAttack(projectile) end

--- Projectile world X — the impact point when read inside a PROJECTILE_HIT
--- handler (splash / AoE centers here). X/Y only, WC3 convention (no Z).
---@param projectile unit
---@return number
function GetProjectileX(projectile) end

--- Projectile world Y — the impact point when read inside a PROJECTILE_HIT
--- handler. X/Y only, WC3 convention (no Z).
---@param projectile unit
---@return number
function GetProjectileY(projectile) end

--- Inside a PROJECTILE_HIT or PROJECTILE_DESTROYED handler, returns
--- the projectile that fired the event. Nil outside those handlers.
---@return unit
function GetTriggerProjectile() end

--- Inside a PROJECTILE_HIT or PROJECTILE_DESTROYED handler, returns
--- the unit that emitted the projectile (from CreateProjectile).
---@return unit
function GetProjectileSource() end

--- Mutate a modifier value on every active instance of `ability_id`
--- on the unit and re-run modifier recalculation immediately. The
--- ability owns the modifier state; Lua just drives the curve. Used
--- for smooth transitions (Wind Walk fade-in, slow ramp-down, etc.).
--- Returns true if any instance was touched.
---@param unit unit
---@param ability_id string
---@param key string         modifier key (e.g. "visual_alpha_mult")
---@param value number
---@return boolean
function SetAbilityModifier(unit, ability_id, key, value) end

---@param unit unit
---@param ability_id string
---@return number   0 if not found
function GetAbilityLevel(unit, ability_id) end

---@param unit unit
---@param ability_id string
---@param level number
function SetUnitAbilityLevel(unit, ability_id, level) end

--- How many instances of this ability are on the unit (for stackable abilities).
---@param unit unit
---@param ability_id string
---@return number
function GetAbilityStackCount(unit, ability_id) end

---@param unit unit
---@param ability_id string
---@return number   remaining cooldown in seconds
function GetAbilityCooldown(unit, ability_id) end

--- Reset cooldown to 0.
---@param unit unit
---@param ability_id string
function ResetAbilityCooldown(unit, ability_id) end

--------------------------------------------------------------------------------
-- Damage & Healing
--------------------------------------------------------------------------------

--- Deal damage from source to target. Fires on_damage event.
---@param source unit
---@param target unit
---@param amount number
---@param damage_type? string  -- map-defined type (default: "spell"). Combat uses "attack".
function DamageUnit(source, target, amount, damage_type) end

--- Heal a unit. Does not exceed max HP. Fires EVENT_HEAL / EVENT_UNIT_HEAL.
--- Triggers may call SetHealAmount() to modify the heal before it's applied.
---@param source unit
---@param target unit
---@param amount number
function HealUnit(source, target, amount) end

--- Source of the current heal event.
---@return unit
function GetHealSource() end

--- Target of the current heal event.
---@return unit
function GetHealTarget() end

--- Amount of the current heal event.
---@return number
function GetHealAmount() end

--- Modify heal amount before it's applied (call inside EVENT_HEAL trigger).
---@param amount number
function SetHealAmount(amount) end

--- Set a unit's health to zero. Death processing runs through the normal
--- on_dying/on_death pipeline on the next simulation tick.
---@param unit unit
---@param killer unit?
function KillUnit(unit, killer) end

--------------------------------------------------------------------------------
-- Player
--------------------------------------------------------------------------------

--- Construct a player handle for slot index (0-based).
---@param slot number
---@return player
function Player(slot) end

---@param player player
---@return string
function GetPlayerName(player) end

--- Set alliance from player_a toward player_b (asymmetric).
--- If allied, units won't auto-attack and are excluded from enemy_of queries.
--- If passive, units won't retaliate when attacked.
---@param player_a player
---@param player_b player
---@param allied boolean
---@param passive? boolean   default false
function SetAlliance(player_a, player_b, allied, passive) end

--- Does player1 consider player2 an ally?
---@param player1 player
---@param player2 player
---@return boolean
function IsPlayerAlly(player1, player2) end

--- Does player1 consider player2 an enemy? (not allied and not same player)
---@param player1 player
---@param player2 player
---@return boolean
function IsPlayerEnemy(player1, player2) end

--------------------------------------------------------------------------------
-- Fog of war / vision
--------------------------------------------------------------------------------

--- Turn fog of war on/off globally. `false` reveals the entire map to
--- every player; `true` restores the authored fog mode (set in the
--- manifest). Use for cinematic reveals.
---@param on boolean
function FogEnable(on) end

--- Whether fog of war is currently active for this map.
---@return boolean
function IsFogEnabled() end

--- Tile-state predicates at a world position. Mutually exclusive: at
--- any (player, x, y) exactly one of Visible / Fogged / Masked is true.
--- IsPointExplored is a convenience for Visible OR Fogged.
--- All return true when fog is disabled.
---@param player player
---@param x number
---@param y number
---@return boolean
function IsPointVisible(player, x, y) end
function IsPointFogged(player, x, y) end
function IsPointMasked(player, x, y) end
function IsPointExplored(player, x, y) end

--- Create a persistent fog-state override on a rectangular area for a
--- player. State is "visible" | "fogged" | "masked". Returns a handle
--- for later destruction.
---@param player player
---@param state string
---@param x0 number
---@param y0 number
---@param x1 number
---@param y1 number
---@return number
function CreateFogModifierRect(player, state, x0, y0, x1, y1) end

--- Create a persistent fog-state override on a circular area for a
--- player. See CreateFogModifierRect for `state`.
---@param player player
---@param state string
---@param cx number
---@param cy number
---@param radius number
---@return number
function CreateFogModifierRadius(player, state, cx, cy, radius) end

--- Destroy a fog modifier by handle. No-op if the handle is unknown.
---@param handle number
function DestroyFogModifier(handle) end

--- Pause / resume a fog modifier without destroying it. Useful for
--- scouting items that toggle on/off as the holder moves.
---@param handle number
---@param active boolean
function SetFogModifierActive(handle, active) end

--- Is `unit` visible to `player` right now? Combines owner/ally check,
--- UnitReveal overrides, invisibility + true sight, and fog of war.
---@param unit unit
---@param player player
---@return boolean
function IsUnitVisibleToPlayer(unit, player) end

--- Is `unit` currently revealed to `player` by a true-sight detector?
--- (False if the unit isn't invisible to begin with.)
---@param unit unit
---@param player player
---@return boolean
function IsUnitDetected(unit, player) end

--- Force-reveal a unit to a specific player (bypasses fog and
--- invisibility). Pass `false` to clear. Multiple players can hold
--- independent reveals on the same unit.
---@param unit unit
---@param player player
---@param on boolean
function UnitReveal(unit, player, on) end

--- Share `unit`'s vision with another player — the unit's sight
--- circle stamps that player's fog map as well. Pass `false` to stop
--- sharing.
---@param unit unit
---@param player player
---@param on boolean
function UnitShareVision(unit, player, on) end

--------------------------------------------------------------------------------
-- Effects
--------------------------------------------------------------------------------

--- Create a persistent effect at a world position. Returns handle.
---@param name string
---@param x number
---@param y number
---@param z number
---@param opts table?  { players = Player | {Player, ...} } selects which
---                     clients receive the effect; omit for broadcast.
---@return number      effect handle (0 on failure)
function CreateEffect(name, x, y, z, opts) end

--- Create a persistent effect attached to a unit. Follows the unit. Returns handle.
---@param name string
---@param unit unit
---@param attach_point string?
---@param opts table?  { players = ... } as above
---@return number      effect handle (0 on failure)
function CreateEffectOnUnit(name, unit, attach_point, opts) end

--- Destroy a persistent effect by handle.
---@param handle number
function DestroyEffect(handle) end

--- Play a fire-and-forget effect at a world position (auto-destroys when done).
---@param name string
---@param x number
---@param y number
---@param z number
---@param opts table?  { players = ... } as above
function PlayEffect(name, x, y, z, opts) end

--- Play a fire-and-forget effect at a unit's position.
---@param name string
---@param unit unit
---@param attach_point string?
---@param opts table?  { players = ... } as above
function PlayEffectOnUnit(name, unit, attach_point, opts) end

--- Engine-defined effects:
---   "hit_spark"    — orange sparks on attack hit
---   "death_burst"  — red burst on unit death
---   "heal_glow"    — green upward glow on healing
---   "spell_cast"   — blue burst on ability cast
---   "blood_splat"  — dark red on heavy damage
---   "aura_glow"    — continuous blue glow (use with CreateEffectOnUnit)

--------------------------------------------------------------------------------
-- Audio
--------------------------------------------------------------------------------

--- Play a positional one-shot sound at a unit's current position.
---@param path string   asset path (OGG)
---@param unit unit
function PlaySound(path, unit) end

--- Play a positional one-shot sound at a world point. Z is 0.
---@param path string
---@param x number
---@param y number
function PlaySoundAtPoint(path, x, y) end

--- Play a non-positional one-shot sound (UI cue, voice line).
---@param path string
function PlaySound2D(path) end

--- Start streaming a music track. Crossfades from the current track.
---@param path string
---@param fade_in number?   seconds (default 1.0)
function PlayMusic(path, fade_in) end

--- Stop the current music track.
---@param fade_out number?  seconds (default 1.0)
function StopMusic(fade_out) end

--- Start a looping positional ambient sound at a world point. Returns a
--- handle for StopAmbientLoop.
---@param path string
---@param x number
---@param y number
---@return number   handle (0 on failure)
function PlayAmbientLoop(path, x, y) end

--- Stop a looping ambient by handle.
---@param handle number
---@param fade_out number?  seconds (default 0.5)
function StopAmbientLoop(handle, fade_out) end

--- Set a channel's gain. Channel names: "master" | "sfx" | "music" |
--- "ambient" | "voice". Volume is linear, 0.0–1.0+ (>1 amplifies).
---@param channel string
---@param volume number
function SetVolume(channel, volume) end

--------------------------------------------------------------------------------
-- Camera (WC3-aligned)
--------------------------------------------------------------------------------
-- Target-based pose: a `CameraSetup` is (target_x, target_y, target_z) +
-- distance + pitch + yaw (degrees on the Lua surface). The eye position
-- is derived as `target - distance * forward_dir`.
--
-- Each call takes a `players` arg that accepts:
--   nil               → all players (broadcast)
--   Player handle     → just that player
--   {Player, ...}     → a list
--
-- Distance is the eye-to-target world distance. Pitch / yaw are degrees
-- (matches WC3 World Editor convention).

--- Look up a named CameraSetup authored in the current scene's
--- `placements.bin` (cameras section). Returns nil if no setup with
--- that id exists. The handle is read-only — its fields (id,
--- target_x, target_y, target_z, distance, pitch, yaw) can be
--- inspected but not mutated. Use CameraSetupApply to apply.
--- Note: ids are capped at 255 bytes on disk.
---@param id string
---@return CameraSetup?
function GetCameraSetup(id) end

--- Apply a whole CameraSetup. `duration` ≤ 0 / nil snaps every axis;
--- > 0 interpolates target, distance, pitch, yaw together.
---@param setup CameraSetup
---@param players player | table | nil
---@param duration number?
function CameraSetupApply(setup, players, duration) end

--- Pan the look-at point. Other axes (distance, pitch, yaw) stay put.
--- `duration` ≤ 0 / nil snaps; > 0 tweens.
---@param players player | table | nil
---@param x number
---@param y number
---@param z number
---@param duration number?
function CameraSetTargetPosition(players, x, y, z, duration) end

--- Adjust eye-to-target distance ("zoom").
---@param players player | table | nil
---@param distance number
---@param duration number?
function CameraSetSourceDistance(players, distance, duration) end

--- Hard-lock target to a unit — the look-at XY follows the unit each
--- frame. Pass nil to release. While locked, the player's WASD pan
--- does nothing; the camera tracks the unit until released.
---@param players player | table | nil
---@param unit unit?
function CameraSetTargetController(players, unit) end

--- Trauma-decay shake. Re-calling mid-shake takes the max intensity
--- and the longer remaining window. `intensity` in world units.
---@param players player | table | nil
---@param intensity number
---@param duration number   seconds
function CameraShake(players, intensity, duration) end

--------------------------------------------------------------------------------
-- Environment
--------------------------------------------------------------------------------

--- Override the sun direction (world-space). Useful for cinematic
--- lighting changes. Vector is normalized internally.
---@param x number
---@param y number
---@param z number
function SetSunDirection(x, y, z) end

--------------------------------------------------------------------------------
-- Spatial Queries
--------------------------------------------------------------------------------

--- Find all units within a radius.
---@param x number
---@param y number
---@param radius number
---@param filter table?   { owner=player, enemy_of=player, classifications={"ground"}, alive_only=true }
---@return unit[]
function GetUnitsInRange(x, y, radius, filter) end

--- Find all units within a rectangle.
---@param x number
---@param y number
---@param width number
---@param height number
---@param filter table?
---@return unit[]
function GetUnitsInRect(x, y, width, height, filter) end

--------------------------------------------------------------------------------
-- Regions
--------------------------------------------------------------------------------

--- Create a new empty region. Returns an opaque region handle.
--- Add geometry with `AddRegionRect` / `AddRegionCircle` before using.
---@return region
function CreateRegion() end

--- Get an authored region from the current scene by its id (the
--- `id` field in the scene's `placements.bin` regions section).
--- Returns nil if no region with that id exists. The handle is
--- interchangeable with one returned by `CreateRegion`.
--- Note: ids are capped at 255 bytes on disk.
---@param id string
---@return region?
function GetRegion(id) end

--- Add an axis-aligned rectangle to the region. Corners may be passed
--- in any order; the region normalises so x0 ≤ x1, y0 ≤ y1.
---@param region region
---@param x0 number
---@param y0 number
---@param x1 number
---@param y1 number
function AddRegionRect(region, x0, y0, x1, y1) end

--- Add a circle to the region. Negative radii are clamped to 0.
---@param region region
---@param cx number
---@param cy number
---@param radius number
function AddRegionCircle(region, cx, cy, radius) end

--- Soft-delete a region. Any in-flight enter/leave actions still
--- complete; the region is fully erased on the next session reset.
---@param region region
function RemoveRegion(region) end

---@param unit unit
---@param region region
---@return boolean
function IsUnitInRegion(unit, region) end

---@param region region
---@return unit[]
function GetUnitsInRegion(region) end

--- Axis-aligned bounding box union over the region's rects and
--- circles. Returns four zeros if the region has no shapes.
---@param region region
---@return number, number, number, number   x0, y0, x1, y1
function GetRegionBounds(region) end

--- Bind a trigger to fire when a unit enters `region`. The trigger's
--- action can read the entering unit via `GetTriggerUnit()` and the
--- region via `GetTriggerRegion()`.
---@param trig trigger
---@param region region
function TriggerRegisterEnterRegion(trig, region) end

--- Bind a trigger to fire when a unit leaves `region`.
---@param trig trigger
---@param region region
function TriggerRegisterLeaveRegion(trig, region) end

--------------------------------------------------------------------------------
-- Items & Inventory
--------------------------------------------------------------------------------
-- Items are a thin collection of abilities. The engine stores two free
-- integer fields (charges, level) but does not interpret them — map Lua
-- drives consumption / level-up / merge / drop-on-death.

--- Spawn a free item entity at a world position. Z is sampled from the
--- terrain. Returns the item handle, or nil if the type id is unknown.
---@param type_id string
---@param x number
---@param y number
---@return item?
function CreateItem(type_id, x, y) end

--- Destroy an item. If the item is carried, its granted abilities are
--- revoked from the carrier and its inventory slot is cleared first.
---@param item item
function RemoveItem(item) end

--- Put an item into a unit's first free inventory slot. Returns the
--- slot index that received it, or -1 if the unit has no Inventory
--- component / no free slot.
---@param unit unit
---@param item item
---@return number
function GiveItem(unit, item) end

--- Drop the item in `slot` from `unit`'s inventory onto the ground.
--- Optional `x, y` override the drop point; default is the unit's
--- current position. Returns true on success.
---@param unit unit
---@param slot number       0-based
---@param x number?
---@param y number?
---@return boolean
function UnitDropItemFromSlot(unit, slot, x, y) end

--- Read the item handle in the given inventory slot. Returns nil for
--- empty slots or invalid slot indices.
---@param unit unit
---@param slot number   0-based
---@return item?
function UnitGetItemFromSlot(unit, slot) end

--- Number of valid (non-empty) items currently in the unit's inventory.
---@param unit unit
---@return number
function UnitItemCount(unit) end

--- Capacity of the unit's inventory (0 if the unit can't hold items).
---@param unit unit
---@return number
function UnitInventorySize(unit) end

--- Whether the unit carries at least one item of `type_id`.
---@param unit unit
---@param type_id string
---@return boolean
function UnitHasItemOfType(unit, type_id) end

---@param item item
---@return string
function GetItemTypeId(item) end

---@param item item
---@return number
function GetItemCharges(item) end

---@param item item
---@param n number
function SetItemCharges(item, n) end

---@param item item
---@return number
function GetItemLevel(item) end

---@param item item
---@param n number
function SetItemLevel(item, n) end

--- The unit currently carrying `item`, or nil if it's on the ground.
---@param item item
---@return unit?
function GetItemOwner(item) end

--- World position of `item`. Returns the carrier's position when held.
---@param item item
---@return number, number, number   x, y, z
function GetItemPosition(item) end

--------------------------------------------------------------------------------
-- Selection
--------------------------------------------------------------------------------
-- The host's view of "what unit ids are currently selected." Wired to
-- the input preset; mutating it from Lua is visible to the preset on
-- the next frame.

---@return number[]   unit ids
function GetSelectedUnits() end

---@return number
function GetSelectedUnitCount() end

--- Replace the selection with a single unit.
---@param unit unit
function SelectUnit(unit) end

--- Action-preset semantic alias for `SelectUnit`. The preset locks
--- selection to this unit and the HUD action bar reads its abilities.
---@param unit unit
function SetControlledUnit(unit) end

--- Replace the selection with an array of unit ids.
---@param unit_ids number[]
function SelectUnits(unit_ids) end

--- Empty the selection.
function ClearSelection() end

---@param unit_id number
---@return boolean
function IsUnitSelected(unit_id) end

--------------------------------------------------------------------------------
-- Order context (input preset events)
--------------------------------------------------------------------------------
-- These read the order currently being dispatched by the local player's
-- input preset. Distinct from the trigger context (`GetTriggerOrderType`
-- / `GetOrderTargetUnit` in the trigger-context section): those return
-- handles, these return raw ids.

---@return string
function GetOrderType() end

---@return number
function GetOrderTargetX() end

---@return number
function GetOrderTargetY() end

---@return number   unit id (0 if not unit-targeted)
function GetOrderTargetUnit() end

---@return number   player id
function GetOrderPlayer() end

--- True if the order was issued with the shift/queue modifier.
---@return boolean
function IsOrderQueued() end

--------------------------------------------------------------------------------
-- Timers
--------------------------------------------------------------------------------

--- Create a timer.
---@param delay number      seconds
---@param repeating boolean  if true, fires every `delay` seconds
---@param callback function
---@return timer
function CreateTimer(delay, repeating, callback) end

--- Destroy a timer. It will not fire again.
---@param timer timer
function DestroyTimer(timer) end

--------------------------------------------------------------------------------
-- Utility
--------------------------------------------------------------------------------

--- Get in-game elapsed time (affected by game speed).
---@return number
function GetGameTime() end

--- Get game speed multiplier.
---@return number
function GetGameSpeed() end

--- Set game speed multiplier (0 = paused, 1 = normal, 2 = fast).
---@param speed number
function SetGameSpeed(speed) end

--- Push a line into the `composites.display_message` overlay (system
--- messages: "Save point reached", "Defeat the boss within 60s", ...).
--- Requires LocalizedString — plain strings are warned + dropped.
--- Newest line sits at the bottom; oldest scrolls off when `max_lines`
--- is exceeded; each line fades out over its lifespan. If the map
--- didn't declare the composite, the call is logged to console.
---
--- opts fields:
---   owner    player            single-player target (alias for players={p})
---   players  player | table    target mask; omit for broadcast
---
---@param text LocalizedString
---@param duration number?  seconds (0 / nil = composite's authored default)
---@param opts table?
function DisplayMessage(text, duration, opts) end

--------------------------------------------------------------------------------
-- I18n
--------------------------------------------------------------------------------

--- Construct a LocalizedString — an opaque handle that player-facing APIs
--- (CreateTextTag, SetLabelText, ...) accept in place of a plain string.
--- The engine ships {key, args} on the network; each client resolves
--- against its own active locale at render time.
---
--- The handle is a plain Lua table `{ __loc = true, key = ..., args = ... }`
--- with no metamethods — there is no host-local coercion to a string. Pass
--- it to the engine APIs that consume LocalizedString.
---
---@param key string         dotted lookup key (e.g. "ability.holy_light.tooltip")
---@param args table?        named-arg substitutions (e.g. {damage = 150})
---@return LocalizedString
function L(key, args) end

--------------------------------------------------------------------------------
-- HUD: floating text tags
--------------------------------------------------------------------------------

--- Create a floating text tag at a world position or attached to a unit.
--- Returns a handle (0 on failure). The `text` field MUST be a
--- LocalizedString; pass a plain string and the call is dropped with a
--- warning.
---
--- args fields:
---   text       LocalizedString   required
---   pos        {x, y, z}         world point (if no unit)
---   unit       unit              attach to a unit (overrides pos)
---   z_offset   number            world-up height above anchor
---   size       number            text px size (default 14)
---   color      string            "#RRGGBB" or "#RRGGBBAA"
---   velocity   {vx, vy}          screen px/sec drift
---   lifespan   number            seconds; 0 = permanent
---   fadepoint  number            seconds before end of lifespan to fade
---   owner      player            single-player target (alias for players={p})
---   players    player | table    target mask; omit for broadcast
---
---@param args table
---@return number  text-tag handle (0 on failure)
function CreateTextTag(args) end

--- Destroy a text tag by handle.
---@param handle number
function DestroyTextTag(handle) end

--- Replace the text of an existing tag. Requires LocalizedString.
---@param handle number
---@param text LocalizedString
function SetTextTagText(handle, text) end

--- Move a tag to a world point.
---@param handle number
---@param x number
---@param y number
---@param z number
function SetTextTagPos(handle, x, y, z) end

--- Attach a tag to a unit (replaces world-position anchor).
---@param handle number
---@param unit unit
---@param z_offset number
function SetTextTagPosUnit(handle, unit, z_offset) end

--- Update the text color.
---@param handle number
---@param color string   "#RRGGBB" or "#RRGGBBAA"
function SetTextTagColor(handle, color) end

--- Update screen-space drift velocity.
---@param handle number
---@param vx number   px/sec
---@param vy number   px/sec
function SetTextTagVelocity(handle, vx, vy) end

--- Show / hide the tag.
---@param handle number
---@param visible boolean
function SetTextTagVisible(handle, visible) end

--------------------------------------------------------------------------------
-- HUD: nodes (panels, labels, bars, buttons, images) from hud.json templates
--------------------------------------------------------------------------------

--- Instantiate a node template (registered in hud.json) at a screen-anchored
--- placement. Returns the template id on success or nil.
---
--- placement fields:
---   anchor     "tl" | "tc" | "tr" | "cl" | "cc" | "cr" | "bl" | "bc" | "br"
---   x, y, w, h number   anchor-relative offset + size in dp
---   owner      player   single-player visibility (alias for players={p})
---   players    player | table   target mask; omit for broadcast
---
---@param template_id string
---@param placement table
---@return string?
function CreateNode(template_id, placement) end

--- Remove an instantiated node (and its subtree).
---@param id string
---@return boolean
function DestroyNode(id) end

--- Whether a node with this id currently exists in the tree.
---@param id string
---@return boolean
function GetNode(id) end

--- Show / hide nodes.
---@param id string
function ShowNode(id) end
---@param id string
function HideNode(id) end
---@param id string
---@param visible boolean
function SetNodeVisible(id, visible) end

--- Update a Label node's text. Requires LocalizedString (no plain strings).
---@param id string
---@param text LocalizedString
function SetLabelText(id, text) end

--- Update a Bar node's fill fraction [0..1].
---@param id string
---@param fill number
function SetBarFill(id, fill) end

--- Update an Image node's texture path.
---@param id string
---@param source string  asset path (e.g. "textures/icons/foo.ktx2")
function SetImageSource(id, source) end

--- Enable / disable a Button node (visual + interaction).
---@param id string
---@param enabled boolean
function SetButtonEnabled(id, enabled) end

--- Bind a trigger to fire on a node event filtered by node id. `node`
--- accepts the id string returned by `CreateNode` / `GetNode` (or a
--- table with a `_id` field for handle-style wrappers). Event names
--- are node-specific (e.g. "button_pressed" for Button nodes).
---@param trig trigger
---@param node string
---@param event_name string
function TriggerRegisterNodeEvent(trig, node, event_name) end

--------------------------------------------------------------------------------
-- HUD: composites (action_bar, minimap, joystick)
--------------------------------------------------------------------------------
-- Composites are engine-authored node groups whose layout comes from
-- hud.json. These calls only toggle visibility / bindings; styling is
-- not script-driven.

--- Show / hide the action_bar composite (whole bar).
---@param visible boolean
function ActionBarSetVisible(visible) end

--- Show / hide a single action_bar slot. Slot indices are 1-based.
---@param slot number
---@param visible boolean
function ActionBarSetSlotVisible(slot, visible) end

--- Bind a specific ability to a slot (action_bar's `binding_mode` must
--- be "manual" for this to take effect). Passive abilities can be
--- bound but won't fire when triggered — a log warning surfaces if
--- the ability is passive / aura.
---@param slot number
---@param ability_id string
function ActionBarSetSlot(slot, ability_id) end

--- Remove a manual slot → ability binding. The slot renders empty
--- afterward (in manual mode).
---@param slot number
function ActionBarClearSlot(slot) end

--- Show / hide the minimap composite.
---@param visible boolean
function MinimapSetVisible(visible) end

--- Show / hide the mobile joystick composite. Inert on desktop maps.
---@param visible boolean
function JoystickSetVisible(visible) end

--------------------------------------------------------------------------------
-- Game flow & session
--------------------------------------------------------------------------------

--- Request a scene swap within the currently loaded map. The actual
--- transition runs between sim ticks, so it's safe to call from inside
--- trigger actions.
---@param scene_name string
function LoadScene(scene_name) end

--- End the current game and report a winner + optional stats payload
--- (JSON string). Fires `global_game_end` before exiting.
---@param winner_id number
---@param stats_json string?   JSON string (default "{}")
function EndGame(winner_id, stats_json) end

--- Pause / unpause the simulation. Independent of the network's
--- reconnect-pause; safe during dialogs / cutscenes. Single-player
--- only — pausing the host's sim in MP would freeze all clients.
function PauseGame() end
function UnpauseGame() end

---@return boolean
function IsGamePaused() end

--- True when launched offline (no network session). Lets map authors
--- gate features that don't make sense in MP (script-driven pause,
--- save-to-file from gameplay).
---@return boolean
function IsSinglePlayer() end

--------------------------------------------------------------------------------
-- Persistent save data (single-player)
--------------------------------------------------------------------------------
-- Key/value store flushed to a per-map JSON file. Supports bool, integer,
-- float, and string values. Multi-player saves are out of scope for v1.

--- Write a value under `key`. Persists to disk synchronously.
---@param key string
---@param value boolean | number | string
function SaveData(key, value) end

--- Read a value under `key`, returning `default_val` (or nil) on miss.
---@param key string
---@param default_val any?
---@return any
function LoadData(key, default_val) end

--- Wipe the entire save store and flush.
function ClearSaveData() end

--- Print to engine log (debug).
---@param text string
function Log(text) end

--- Distance between two units on the XY plane.
---@param unit1 unit
---@param unit2 unit
---@return number
function GetDistanceBetween(unit1, unit2) end

--- Angle from unit1 to unit2 in radians.
---@param unit1 unit
---@param unit2 unit
---@return number
function GetAngleBetween(unit1, unit2) end

--- Random integer in [min, max].
---@param min number
---@param max number
---@return number
function RandomInt(min, max) end

--- Random float in [min, max].
---@param min number
---@param max number
---@return number
function RandomFloat(min, max) end
