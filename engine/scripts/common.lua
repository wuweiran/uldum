--------------------------------------------------------------------------------
-- common.lua — Uldum Engine API Declarations
--
-- This file is loaded by the engine before any map script. It declares all
-- engine functions available to map scripts. The actual implementations are
-- C++ functions bound via sol2.
--
-- Analogous to Warcraft III's "common.j".
--------------------------------------------------------------------------------

-- NOTE: All functions below are implemented in C++ and bound to Lua by the engine.
-- This file serves as documentation and type reference for map makers.
-- Do not modify this file — it is part of the engine, not the map.

--------------------------------------------------------------------------------
-- Triggers
--------------------------------------------------------------------------------

--- Create a trigger — a lifecycle scope for events, conditions, actions, and data.
--- @return trigger
function CreateTrigger() end

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

--- Register an event scoped to a specific unit + ability.
--- @param trig trigger
--- @param unit unit
--- @param ability_id string
--- @param event_name string
function TriggerRegisterUnitAbilityEvent(trig, unit, ability_id, event_name) end

--- Register an event scoped to a specific player.
--- @param trig trigger
--- @param player player
--- @param event_name string
function TriggerRegisterPlayerEvent(trig, player, event_name) end

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

--- Bind trigger lifecycle to a unit. Auto-destroyed when unit is removed.
--- @param trig trigger
--- @param unit unit
function TriggerBindToUnit(trig, unit) end

--- Bind trigger lifecycle to an ability on a unit. Auto-destroyed when ability removed.
--- @param trig trigger
--- @param unit unit
--- @param ability_id string
function TriggerBindToAbility(trig, unit, ability_id) end

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
-- RegisterEvent (shorthand)
--------------------------------------------------------------------------------

--- Shorthand: create a trigger with one global event and one action.
--- Returns a handle for UnregisterEvent.
--- @param event_name string
--- @param handler function
--- @return event_handle
function RegisterEvent(event_name, handler) end

--- Unregister a handler created by RegisterEvent.
--- @param handle event_handle
function UnregisterEvent(handle) end

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
-- Orders
--------------------------------------------------------------------------------

--- Issue an order to a unit.
--- @param unit unit
--- @param order_type string   "move", "attack", "stop", "hold", "cast", "patrol"
--- @param ...                 Order-specific arguments
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
function DamageUnit(source, target, amount) end

--- Heal a unit. Does not exceed max HP.
--- @param source unit
--- @param target unit
--- @param amount number
function HealUnit(source, target, amount) end

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

--- @param player1 player
--- @param player2 player
--- @return boolean
function IsPlayerAlly(player1, player2) end

--- @param player1 player
--- @param player2 player
--- @return boolean
function IsPlayerEnemy(player1, player2) end

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

--- Get a named region from the current scene.
--- @param name string
--- @return region
function GetRegion(name) end

--- @param unit unit
--- @param region region
--- @return boolean
function IsUnitInRegion(unit, region) end

--- @param region region
--- @param filter table?
--- @return unit[]
function GetUnitsInRegion(region, filter) end

--- @param region region
--- @return number, number   x, y
function GetRegionCenter(region) end

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
