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
EVENT_GLOBAL_DEATH           = "global_death"
EVENT_GLOBAL_HEAL            = "global_heal"
EVENT_GLOBAL_UNIT_CREATED    = "global_unit_created"
EVENT_GLOBAL_UNIT_REMOVED    = "global_unit_removed"
EVENT_GLOBAL_ABILITY_EFFECT  = "global_ability_effect"
EVENT_GLOBAL_ABILITY_ADDED   = "global_ability_added"
EVENT_GLOBAL_ABILITY_REMOVED = "global_ability_removed"
EVENT_GLOBAL_ORDER           = "global_order"
EVENT_GLOBAL_SELECT          = "global_select"
EVENT_GLOBAL_GAME_END        = "global_game_end"
EVENT_GLOBAL_DISCONNECT      = "global_disconnect"
EVENT_GLOBAL_LEAVE           = "global_leave"

-- Unit-scoped events (TriggerRegisterUnitEvent)
EVENT_UNIT_DAMAGE          = "unit_damage"
EVENT_UNIT_DEATH           = "unit_death"
EVENT_UNIT_HEAL            = "unit_heal"
EVENT_UNIT_ABILITY_EFFECT  = "unit_ability_effect"
EVENT_UNIT_ABILITY_ADDED   = "unit_ability_added"
EVENT_UNIT_ABILITY_REMOVED = "unit_ability_removed"

-- Player-scoped events (TriggerRegisterPlayerEvent)
EVENT_PLAYER_ORDER         = "player_order"
EVENT_PLAYER_DISCONNECT    = "player_disconnect"
EVENT_PLAYER_LEAVE         = "player_leave"
