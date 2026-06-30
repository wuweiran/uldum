--------------------------------------------------------------------------------
-- constants.lua — Engine constants loaded at runtime before map scripts.
-- Defines event names, priority levels, and other engine constants.
--------------------------------------------------------------------------------

-- Trigger priority (higher fires first)
TRIGGER_PRIORITY_LOW    = 0
TRIGGER_PRIORITY_NORMAL = 1
TRIGGER_PRIORITY_HIGH   = 2

-- Global events (TriggerRegisterEvent)
EVENT_GLOBAL_DAMAGE          = "global_damage"
EVENT_GLOBAL_DYING           = "global_dying"
EVENT_GLOBAL_DEATH           = "global_death"
EVENT_GLOBAL_HEAL            = "global_heal"
EVENT_GLOBAL_ATTACKED        = "global_attacked"
EVENT_GLOBAL_UNIT_CREATED    = "global_unit_created"
EVENT_GLOBAL_UNIT_REMOVED    = "global_unit_removed"
EVENT_GLOBAL_ABILITY_CHANNEL = "global_ability_channel"
EVENT_GLOBAL_ABILITY_ENDCAST = "global_ability_endcast"
EVENT_GLOBAL_ABILITY_EFFECT  = "global_ability_effect"
EVENT_GLOBAL_ITEM_PICKED_UP  = "global_item_picked_up"
EVENT_GLOBAL_ITEM_DROPPED    = "global_item_dropped"
EVENT_GLOBAL_ISSUED_ORDER    = "global_issued_order"
EVENT_GLOBAL_SELECT          = "global_select"
EVENT_GLOBAL_GAME_END        = "global_game_end"
EVENT_GLOBAL_DISCONNECT      = "global_disconnect"
EVENT_GLOBAL_LEAVE           = "global_leave"
EVENT_GLOBAL_PROJECTILE_HIT       = "global_projectile_hit"
EVENT_GLOBAL_PROJECTILE_DESTROYED = "global_projectile_destroyed"

-- Projectile-scoped events (TriggerRegisterProjectileEvent)
EVENT_PROJECTILE_HIT       = "projectile_hit"
EVENT_PROJECTILE_DESTROYED = "projectile_destroyed"

-- Unit-scoped events (TriggerRegisterUnitEvent)
EVENT_UNIT_DAMAGE          = "unit_damage"
EVENT_UNIT_DYING           = "unit_dying"
EVENT_UNIT_DEATH           = "unit_death"
EVENT_UNIT_HEAL            = "unit_heal"
EVENT_UNIT_ATTACKED        = "unit_attacked"
EVENT_UNIT_ABILITY_CHANNEL = "unit_ability_channel"
EVENT_UNIT_ABILITY_ENDCAST = "unit_ability_endcast"
EVENT_UNIT_ABILITY_EFFECT  = "unit_ability_effect"
EVENT_UNIT_ITEM_PICKED_UP  = "unit_item_picked_up"
EVENT_UNIT_ITEM_DROPPED    = "unit_item_dropped"
EVENT_UNIT_ISSUED_ORDER    = "unit_issued_order"

-- Destructable-scoped events (TriggerRegisterDestructableEvent)
EVENT_DESTRUCTABLE_DEATH   = "destructable_death"

-- Player-scoped events (TriggerRegisterPlayerEvent)
EVENT_PLAYER_ORDER         = "player_order"
EVENT_PLAYER_DISCONNECT    = "player_disconnect"
EVENT_PLAYER_LEAVE         = "player_leave"

-- HUD events — player-scoped. A player's client forwards the raw input
-- (button press etc.) to the server via C_NODE_EVENT; the server fires
-- the corresponding event with the player id as the filter. Use
-- GetTriggerPlayer() / GetTriggerNode() inside the action to identify
-- who clicked what.
EVENT_BUTTON_PRESSED       = "button_pressed"

-- Status flags — pass to SetUnitStatus / GetUnitStatus. See
-- gameplay-model.md `## Status Flags` for per-flag semantics.
UNIT_STATUS_STUNNED      = "stunned"
UNIT_STATUS_SILENCED     = "silenced"
UNIT_STATUS_MUTED        = "muted"
UNIT_STATUS_DISARMED     = "disarmed"
UNIT_STATUS_ROOTED       = "rooted"
UNIT_STATUS_INVULNERABLE = "invulnerable"
UNIT_STATUS_MAGIC_IMMUNE = "magic_immune"
UNIT_STATUS_UNTARGETABLE = "untargetable"
UNIT_STATUS_UNATTACKABLE = "unattackable"
UNIT_STATUS_PAUSED       = "paused"
UNIT_STATUS_INVISIBLE    = "invisible"
