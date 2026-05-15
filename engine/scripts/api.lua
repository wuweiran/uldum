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
--- @param priority? number   TRIGGER_PRIORITY_* constant. Default: TRIGGER_PRIORITY_NORMAL.
--- @return trigger
function CreateTrigger(priority) end

--- Destroy a trigger. All events unregistered, timers stopped, data cleaned up.
--- @param trig trigger
function DestroyTrigger(trig) end

--- Register a global event on a trigger.
--- @param trig trigger
--- @param event_name string
function TriggerRegisterEvent(trig, event_name) end

--- Register an event scoped to a specific unit.
--- @param trig trigger
--- @param unit unit
--- @param event_name string
function TriggerRegisterUnitEvent(trig, unit, event_name) end

--- Register an event scoped to a specific player.
--- @param trig trigger
--- @param player player
--- @param event_name string
function TriggerRegisterPlayerEvent(trig, player, event_name) end

--- Register an event scoped to a specific projectile. The trigger is
--- automatically dropped when the projectile is destroyed.
--- @param trig trigger
--- @param projectile unit
--- @param event_name string  EVENT_PROJECTILE_HIT or EVENT_PROJECTILE_DESTROYED
function TriggerRegisterProjectileEvent(trig, projectile, event_name) end

--- Register a timer event on a trigger.
--- @param trig trigger
--- @param interval number   seconds
--- @param repeating boolean
function TriggerRegisterTimerEvent(trig, interval, repeating) end

--- Add a condition to a trigger. If condition returns false, actions are skipped.
--- @param trig trigger
--- @param condition function  → bool
function TriggerAddCondition(trig, condition) end

--- Add an action to a trigger. Called when events fire and conditions pass.
--- @param trig trigger
--- @param action function
function TriggerAddAction(trig, action) end


--------------------------------------------------------------------------------
-- Trigger Context (call inside trigger actions to read event data)
--------------------------------------------------------------------------------

--- Which event caused this trigger to fire.
--- @return string
function GetTriggerEvent() end

--- The unit associated with the current event.
--- @return unit
function GetTriggerUnit() end

--- The player associated with the current event.
--- @return player
function GetTriggerPlayer() end

--- The ability id associated with the current event.
--- @return string
function GetTriggerAbilityId() end

--- Target unit of a unit-targeted ability.
--- @return unit
function GetSpellTargetUnit() end

--- Target point of a point-targeted ability.
--- @return number, number   x, y
function GetSpellTargetPoint() end

--- Target point X of a point-targeted ability (convenience scalar accessor).
--- @return number
function GetSpellTargetX() end

--- Target point Y of a point-targeted ability (convenience scalar accessor).
--- @return number
function GetSpellTargetY() end

--- Source of the current damage event.
--- @return unit
function GetDamageSource() end

--- Target of the current damage event.
--- @return unit
function GetDamageTarget() end

--- Amount of the current damage event.
--- @return number
function GetDamageAmount() end

--- Modify damage amount before it's applied.
--- @param amount number
function SetDamageAmount(amount) end

--- The damage type of the current damage event (map-defined, e.g. "attack", "spell", "cleave").
--- @return string
function GetDamageType() end

--- The unit that killed the current dying unit.
--- @return unit
function GetKillingUnit() end

--- The attacker in the current attack event.
--- @return unit
function GetAttacker() end

--- The target of the current attack event.
--- @return unit
function GetAttackTarget() end

--------------------------------------------------------------------------------
-- Unit Creation & Lifecycle
--------------------------------------------------------------------------------

--- Create a new unit at the given position.
--- @param type_id string   Unit type from map's unit_types.json
--- @param player player    Owner
--- @param x number         X position (game coords)
--- @param y number         Y position (game coords)
--- @param facing number?   Facing in radians (default 0, facing +Y)
--- @return unit
function CreateUnit(type_id, player, x, y, facing) end

--- Remove a unit from the game world.
--- @param unit unit
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
--- @param unit unit
--- @param new_type_id string
--- @return boolean
function MorphUnit(unit, new_type_id) end

--- Read the cooldown remaining (seconds) on a unit's ability.
--- Returns 0 if the unit doesn't have the ability.
--- @param unit unit
--- @param ability_id string
--- @return number
function GetAbilityCooldown(unit, ability_id) end

--- Set the cooldown remaining (seconds) on a unit's ability. No-op
--- when the unit doesn't have the ability. Used by morph helpers to
--- carry cooldowns across a Remove/Add round-trip.
--- @param unit unit
--- @param ability_id string
--- @param seconds number
function SetAbilityCooldown(unit, ability_id, seconds) end

--- List the ability ids declared in a unit type's `abilities` array.
--- Used by morph helpers that need to iterate the type's kit without
--- duplicating the list in Lua. Returns an empty table if the type id
--- is unknown.
--- @param type_id string
--- @return string[]
function GetUnitTypeAbilities(type_id) end

--- Check if a unit is alive (HP > 0 and not in dead state).
--- @param unit unit
--- @return boolean
function IsUnitAlive(unit) end

--- Check if a unit is dead (in dead/corpse state).
--- @param unit unit
--- @return boolean
function IsUnitDead(unit) end

--- Check if a unit has the "hero" classification.
--- @param unit unit
--- @return boolean
function IsUnitHero(unit) end

--- Check if a unit has the "structure" classification.
--- @param unit unit
--- @return boolean
function IsUnitBuilding(unit) end

--- Get the unit's type id string.
--- @param unit unit
--- @return string
function GetUnitTypeId(unit) end

--------------------------------------------------------------------------------
-- Unit Position & Facing
--------------------------------------------------------------------------------

--- @param unit unit
--- @return number
function GetUnitX(unit) end

--- @param unit unit
--- @return number
function GetUnitY(unit) end

--- @param unit unit
--- @return number
function GetUnitZ(unit) end

--- @param unit unit
--- @return number, number, number   x, y, z
function GetUnitPosition(unit) end

--- Set unit position on the XY plane. Z is set from terrain height.
--- @param unit unit
--- @param x number
--- @param y number
function SetUnitPosition(unit, x, y) end

--- @param unit unit
--- @return number   radians, 0 = facing +Y
function GetUnitFacing(unit) end

--- @param unit unit
--- @param facing number   radians
function SetUnitFacing(unit, facing) end

--------------------------------------------------------------------------------
-- Health (engine built-in state)
--------------------------------------------------------------------------------

--- @param unit unit
--- @return number
function GetUnitHealth(unit) end

--- @param unit unit
--- @return number
function GetUnitMaxHealth(unit) end

--- @param unit unit
--- @param hp number
function SetUnitHealth(unit, hp) end

--------------------------------------------------------------------------------
-- States (map-defined: mana, energy, etc.)
--------------------------------------------------------------------------------

--- @param unit unit
--- @param state_id string   e.g. "mana"
--- @return number
function GetUnitState(unit, state_id) end

--- @param unit unit
--- @param state_id string
--- @return number
function GetUnitMaxState(unit, state_id) end

--- @param unit unit
--- @param state_id string
--- @param value number
function SetUnitState(unit, state_id, value) end

--------------------------------------------------------------------------------
-- Attributes (map-defined: armor, strength, etc.)
--------------------------------------------------------------------------------

--- Get a numeric attribute.
--- @param unit unit
--- @param attr_id string   e.g. "armor", "strength"
--- @return number
function GetUnitAttribute(unit, attr_id) end

--- Set a numeric attribute.
--- @param unit unit
--- @param attr_id string
--- @param value number
function SetUnitAttribute(unit, attr_id, value) end

--- Get a string attribute (e.g. "armor_type").
--- @param unit unit
--- @param attr_id string
--- @return string
function GetUnitStringAttribute(unit, attr_id) end

--- Set a string attribute.
--- @param unit unit
--- @param attr_id string
--- @param value string
function SetUnitStringAttribute(unit, attr_id, value) end

--------------------------------------------------------------------------------
-- Owner & Classification
--------------------------------------------------------------------------------

--- @param unit unit
--- @return player
function GetUnitOwner(unit) end

--- @param unit unit
--- @param player player
function SetUnitOwner(unit, player) end

--- @param unit unit
--- @param flag string   e.g. "ground", "hero", "structure"
--- @return boolean
function HasClassification(unit, flag) end

--- @param unit unit
--- @param flag string
function AddClassification(unit, flag) end

--- @param unit unit
--- @param flag string
function RemoveClassification(unit, flag) end

--------------------------------------------------------------------------------
-- Status flags
--------------------------------------------------------------------------------

--- Set or clear a status flag on a unit. Use the `UNIT_STATUS_*`
--- constants from constants.lua (`UNIT_STATUS_STUNNED`,
--- `UNIT_STATUS_SILENCED`, `UNIT_STATUS_MUTED`, `UNIT_STATUS_DISARMED`,
--- `UNIT_STATUS_ROOTED`, `UNIT_STATUS_INVULNERABLE`,
--- `UNIT_STATUS_MAGIC_IMMUNE`, `UNIT_STATUS_UNTARGETABLE`,
--- `UNIT_STATUS_UNATTACKABLE`, `UNIT_STATUS_PAUSED`,
--- `UNIT_STATUS_INVISIBLE`). See gameplay-model.md `## Status Flags`
--- for the per-flag enforcement semantics.
--- @param unit unit
--- @param flag string  -- a UNIT_STATUS_* constant
--- @param on boolean
function SetUnitStatus(unit, flag, on) end

--- Read a status flag on a unit. Returns false when the unit has no
--- StatusFlags component (treated as "no flags set"). Pass a
--- `UNIT_STATUS_*` constant for `flag`.
--- @param unit unit
--- @param flag string  -- a UNIT_STATUS_* constant
--- @return boolean
function GetUnitStatus(unit, flag) end

--- Clear every status flag on a unit (resets the bitset to 0).
--- @param unit unit
function ClearAllUnitStatus(unit) end

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
--- @param unit unit
--- @param clip string    clip name in the unit's glTF
--- @param looping boolean? if true and this clip is the last in the queue, it loops; default false (one-shot)
function SetUnitAnimation(unit, clip, looping) end

--- Append `clip` to the unit's animation queue. If the unit isn't
--- currently script-controlled, this behaves like SetUnitAnimation
--- for the first call. The renderer advances the queue each time
--- the current clip finishes.
--- @param unit unit
--- @param clip string
function QueueUnitAnimation(unit, clip) end

--- Drop the unit's script-driven animation queue. The engine's
--- per-frame state derivation resumes on the next frame and
--- crossfades back to idle / walk / whatever sim state implies.
--- @param unit unit
function ResetUnitAnimation(unit) end

--------------------------------------------------------------------------------
-- Orders
--------------------------------------------------------------------------------

--- Issue an order to a unit.
--- @param unit unit
--- @param order_type string   "move", "attack", "stop", "hold", "cast", "patrol"
--- @param ...                 Order-specific arguments
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
--- @param unit unit
--- @param ability_id string
--- @param level number?   default 1
--- @return boolean   true if added
function AddAbility(unit, ability_id, level) end

--- Remove an ability from a unit. Reverts modifiers.
--- @param unit unit
--- @param ability_id string
--- @return boolean   true if found and removed
function RemoveAbility(unit, ability_id) end

--- Apply a passive ability (buff) to a target from a source, with duration.
--- Non-stackable abilities refresh duration if already present.
--- @param target unit
--- @param ability_id string
--- @param source unit
--- @param duration number   seconds
--- @return boolean
function ApplyPassiveAbility(target, ability_id, source, duration) end

--- @param unit unit
--- @param ability_id string
--- @return boolean
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
--- @param source unit
--- @param model string     glTF path (empty falls back to engine "projectile" mesh)
--- @return unit            projectile handle (or nil)
function CreateProjectile(source, model) end

--- Launch a homing projectile at `target`. Auto-destroys on impact;
--- fires PROJECTILE_HIT once with the target as `GetTriggerUnit()`.
--- @param projectile unit
--- @param target unit
--- @param speed number
--- @param arc_height number?  (reserved; arc rendering pending)
function EmitProjectileTarget(projectile, target, speed, arc_height) end

--- Launch a linear (skillshot) projectile toward `(x,y,z)`. Fires
--- PROJECTILE_HIT once for every unit within `hit_radius` along the
--- path (pierce-by-default). Auto-destroys at `max_distance`. Stops
--- on first hit only if the trigger handler calls
--- `DestroyProjectile(GetTriggerProjectile())`.
--- @param projectile unit
--- @param x number
--- @param y number
--- @param z number
--- @param speed number
--- @param hit_radius number
--- @param max_distance number
function EmitProjectileLoc(projectile, x, y, z, speed, hit_radius, max_distance) end

--- Manually destroy a projectile. Fires PROJECTILE_DESTROYED.
--- @param projectile unit
function DestroyProjectile(projectile) end

--- Read the damage carried by a projectile. The engine consumes this
--- for auto-attack projectiles; ability projectiles can use it as a
--- payload slot.
--- @param projectile unit
--- @return number
function GetProjectileDamage(projectile) end

--- Set the damage on a projectile. For `is_attack` projectiles this
--- changes what the engine deals at hit time — intercept in
--- PROJECTILE_HIT to apply crit/lifesteal multipliers before the
--- engine commits damage.
--- @param projectile unit
--- @param damage number
function SetProjectileDamage(projectile, damage) end

--- True when the engine spawned this projectile from an auto-attack.
--- Filter use: a global PROJECTILE_HIT trigger that only modifies
--- auto-attack damage without touching ability projectiles.
--- @param projectile unit
--- @return boolean
function IsProjectileNormalAttack(projectile) end

--- Inside a PROJECTILE_HIT or PROJECTILE_DESTROYED handler, returns
--- the projectile that fired the event. Nil outside those handlers.
--- @return unit
function GetTriggerProjectile() end

--- Inside a PROJECTILE_HIT or PROJECTILE_DESTROYED handler, returns
--- the unit that emitted the projectile (from CreateProjectile).
--- @return unit
function GetProjectileSource() end

--- Mutate a modifier value on every active instance of `ability_id`
--- on the unit and re-run modifier recalculation immediately. The
--- ability owns the modifier state; Lua just drives the curve. Used
--- for smooth transitions (Wind Walk fade-in, slow ramp-down, etc.).
--- Returns true if any instance was touched.
--- @param unit unit
--- @param ability_id string
--- @param key string         modifier key (e.g. "visual_alpha_mult")
--- @param value number
--- @return boolean
function SetAbilityModifier(unit, ability_id, key, value) end

--- @param unit unit
--- @param ability_id string
--- @return number   0 if not found
function GetAbilityLevel(unit, ability_id) end

--- @param unit unit
--- @param ability_id string
--- @param level number
function SetAbilityLevel(unit, ability_id, level) end

--- How many instances of this ability are on the unit (for stackable abilities).
--- @param unit unit
--- @param ability_id string
--- @return number
function GetAbilityStackCount(unit, ability_id) end

--- Reset the duration of an existing passive ability on a unit.
--- @param unit unit
--- @param ability_id string
--- @param duration number?   if omitted, uses the original duration
function RefreshAbilityDuration(unit, ability_id, duration) end

--- Get the source unit that applied a passive ability.
--- @param unit unit
--- @param ability_id string
--- @return unit   or nil if innate
function GetAbilitySource(unit, ability_id) end

--- @param unit unit
--- @param ability_id string
--- @return number   remaining cooldown in seconds
function GetAbilityCooldown(unit, ability_id) end

--- Reset cooldown to 0.
--- @param unit unit
--- @param ability_id string
function ResetAbilityCooldown(unit, ability_id) end

--------------------------------------------------------------------------------
-- Damage & Healing
--------------------------------------------------------------------------------

--- Deal damage from source to target. Fires on_damage event.
--- @param source unit
--- @param target unit
--- @param amount number
--- @param damage_type? string  -- map-defined type (default: "spell"). Combat uses "attack".
function DamageUnit(source, target, amount, damage_type) end

--- Heal a unit. Does not exceed max HP. Fires EVENT_HEAL / EVENT_UNIT_HEAL.
--- Triggers may call SetHealAmount() to modify the heal before it's applied.
--- @param source unit
--- @param target unit
--- @param amount number
function HealUnit(source, target, amount) end

--- Source of the current heal event.
--- @return unit
function GetHealSource() end

--- Target of the current heal event.
--- @return unit
function GetHealTarget() end

--- Amount of the current heal event.
--- @return number
function GetHealAmount() end

--- Modify heal amount before it's applied (call inside EVENT_HEAL trigger).
--- @param amount number
function SetHealAmount(amount) end

--- Instantly kill a unit. Fires on_death event.
--- @param unit unit
--- @param killer unit?
function KillUnit(unit, killer) end

--------------------------------------------------------------------------------
-- Hero
--------------------------------------------------------------------------------

--- @param unit unit
--- @return number
function GetHeroLevel(unit) end

--- @param unit unit
--- @param level number
function SetHeroLevel(unit, level) end

--- @param unit unit
--- @param xp number
function AddHeroXP(unit, xp) end

--- @param unit unit
--- @return number
function GetHeroXP(unit) end

--------------------------------------------------------------------------------
-- Player
--------------------------------------------------------------------------------

--- Get a player by slot index (0-based).
--- @param slot number
--- @return player
function GetPlayer(slot) end

--- @param player player
--- @return string
function GetPlayerName(player) end

--- Set alliance from player_a toward player_b (asymmetric).
--- If allied, units won't auto-attack and are excluded from enemy_of queries.
--- If passive, units won't retaliate when attacked.
--- @param player_a player
--- @param player_b player
--- @param allied boolean
--- @param passive? boolean   default false
function SetAlliance(player_a, player_b, allied, passive) end

--- Does player1 consider player2 an ally?
--- @param player1 player
--- @param player2 player
--- @return boolean
function IsPlayerAlly(player1, player2) end

--- Does player1 consider player2 an enemy? (not allied and not same player)
--- @param player1 player
--- @param player2 player
--- @return boolean
function IsPlayerEnemy(player1, player2) end

--------------------------------------------------------------------------------
-- Fog of war / vision
--------------------------------------------------------------------------------

--- Turn fog of war on/off globally. `false` reveals the entire map to
--- every player; `true` restores the authored fog mode (set in the
--- manifest). Use for cinematic reveals.
--- @param on boolean
function FogEnable(on) end

--- Whether fog of war is currently active for this map.
--- @return boolean
function IsFogEnabled() end

--- Tile-state predicates at a world position. Mutually exclusive: at
--- any (player, x, y) exactly one of Visible / Fogged / Masked is true.
--- IsPointExplored is a convenience for Visible OR Fogged.
--- All return true when fog is disabled.
--- @param player player
--- @param x number
--- @param y number
--- @return boolean
function IsPointVisible(player, x, y) end
function IsPointFogged(player, x, y) end
function IsPointMasked(player, x, y) end
function IsPointExplored(player, x, y) end

--- Create a persistent fog-state override on a rectangular area for a
--- player. State is "visible" | "fogged" | "masked". Returns a handle
--- for later destruction.
--- @param player player
--- @param state string
--- @param x0 number
--- @param y0 number
--- @param x1 number
--- @param y1 number
--- @return number
function CreateFogModifierRect(player, state, x0, y0, x1, y1) end

--- Create a persistent fog-state override on a circular area for a
--- player. See CreateFogModifierRect for `state`.
--- @param player player
--- @param state string
--- @param cx number
--- @param cy number
--- @param radius number
--- @return number
function CreateFogModifierRadius(player, state, cx, cy, radius) end

--- Destroy a fog modifier by handle. No-op if the handle is unknown.
--- @param handle number
function DestroyFogModifier(handle) end

--- Pause / resume a fog modifier without destroying it. Useful for
--- scouting items that toggle on/off as the holder moves.
--- @param handle number
--- @param active boolean
function SetFogModifierActive(handle, active) end

--- Is `unit` visible to `player` right now? Combines owner/ally check,
--- UnitReveal overrides, invisibility + true sight, and fog of war.
--- @param unit unit
--- @param player player
--- @return boolean
function IsUnitVisibleToPlayer(unit, player) end

--- Is `unit` currently revealed to `player` by a true-sight detector?
--- (False if the unit isn't invisible to begin with.)
--- @param unit unit
--- @param player player
--- @return boolean
function IsUnitDetected(unit, player) end

--- Force-reveal a unit to a specific player (bypasses fog and
--- invisibility). Pass `false` to clear. Multiple players can hold
--- independent reveals on the same unit.
--- @param unit unit
--- @param player player
--- @param on boolean
function UnitReveal(unit, player, on) end

--- Share `unit`'s vision with another player — the unit's sight
--- circle stamps that player's fog map as well. Pass `false` to stop
--- sharing.
--- @param unit unit
--- @param player player
--- @param on boolean
function UnitShareVision(unit, player, on) end

--------------------------------------------------------------------------------
-- Effects
--------------------------------------------------------------------------------

--- Create a persistent effect at a world position. Returns handle.
--- @param name string
--- @param x number
--- @param y number
--- @param z number
--- @return number   effect handle (0 on failure)
function CreateEffect(name, x, y, z) end

--- Create a persistent effect attached to a unit. Follows the unit. Returns handle.
--- @param name string
--- @param unit unit
--- @return number   effect handle (0 on failure)
function CreateEffectOnUnit(name, unit) end

--- Destroy a persistent effect by handle.
--- @param handle number
function DestroyEffect(handle) end

--- Play a fire-and-forget effect at a world position (auto-destroys when done).
--- @param name string
--- @param x number
--- @param y number
--- @param z number
function PlayEffect(name, x, y, z) end

--- Play a fire-and-forget effect at a unit's position.
--- @param name string
--- @param unit unit
function PlayEffectOnUnit(name, unit) end

--- Engine-defined effects:
---   "hit_spark"    — orange sparks on attack hit
---   "death_burst"  — red burst on unit death
---   "heal_glow"    — green upward glow on healing
---   "spell_cast"   — blue burst on ability cast
---   "blood_splat"  — dark red on heavy damage
---   "aura_glow"    — continuous blue glow (use with CreateEffectOnUnit)

--------------------------------------------------------------------------------
-- Spatial Queries
--------------------------------------------------------------------------------

--- Find all units within a radius.
--- @param x number
--- @param y number
--- @param radius number
--- @param filter table?   { owner=player, enemy_of=player, classifications={"ground"}, alive_only=true }
--- @return unit[]
function GetUnitsInRange(x, y, radius, filter) end

--- Find all units within a rectangle.
--- @param x number
--- @param y number
--- @param width number
--- @param height number
--- @param filter table?
--- @return unit[]
function GetUnitsInRect(x, y, width, height, filter) end

--- Find the nearest unit matching a filter.
--- @param x number
--- @param y number
--- @param radius number
--- @param filter table?
--- @return unit?   nil if none found
function GetNearestUnit(x, y, radius, filter) end

--------------------------------------------------------------------------------
-- Regions
--------------------------------------------------------------------------------

--- Get an authored region from the current scene by its id (the
--- `id` field in the scene's objects.json `regions[]` array).
--- Returns nil if no region with that id exists. The handle is
--- interchangeable with one returned by `CreateRegion`.
--- @param id string
--- @return region?
function GetRegion(id) end

--- @param unit unit
--- @param region region
--- @return boolean
function IsUnitInRegion(unit, region) end

--- @param region region
--- @param filter table?
--- @return unit[]
function GetUnitsInRegion(region, filter) end

--- Axis-aligned bounding box union over the region's rects and
--- circles. Returns four zeros if the region has no shapes.
--- @param region region
--- @return number, number, number, number   x0, y0, x1, y1
function GetRegionBounds(region) end

--------------------------------------------------------------------------------
-- Timers
--------------------------------------------------------------------------------

--- Create a timer.
--- @param delay number      seconds
--- @param repeating boolean  if true, fires every `delay` seconds
--- @param callback function
--- @return timer
function CreateTimer(delay, repeating, callback) end

--- Destroy a timer. It will not fire again.
--- @param timer timer
function DestroyTimer(timer) end

--------------------------------------------------------------------------------
-- Utility
--------------------------------------------------------------------------------

--- Get in-game elapsed time (affected by game speed).
--- @return number
function GetGameTime() end

--- Get game speed multiplier.
--- @return number
function GetGameSpeed() end

--- Set game speed multiplier (0 = paused, 1 = normal, 2 = fast).
--- @param speed number
function SetGameSpeed(speed) end

--- Display a message on screen.
--- @param text string
--- @param duration number?  seconds (default 5)
function DisplayMessage(text, duration) end

--- Print to engine log (debug).
--- @param text string
function Log(text) end

--- Distance between two units on the XY plane.
--- @param unit1 unit
--- @param unit2 unit
--- @return number
function GetDistanceBetween(unit1, unit2) end

--- Angle from unit1 to unit2 in radians.
--- @param unit1 unit
--- @param unit2 unit
--- @return number
function GetAngleBetween(unit1, unit2) end

--- Random integer in [min, max].
--- @param min number
--- @param max number
--- @return number
function RandomInt(min, max) end

--- Random float in [min, max].
--- @param min number
--- @param max number
--- @return number
function RandomFloat(min, max) end
