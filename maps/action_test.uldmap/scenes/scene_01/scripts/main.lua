--------------------------------------------------------------------------------
-- Action Test — Scene 01
-- Action preset test bed. One Paladin at the center controlled by the
-- player via WASD + left-click. Creeps spawn in waves from the edges to
-- give the hero something to fight. Hero has Holy Light (Q in positional
-- mode) for self-heal testing and Consecration as a passive.
--------------------------------------------------------------------------------

require("constants")
require("combat")

function main()
    Log("[Action] Scene setup starting")

    local player0 = GetPlayer(0)   -- hero
    local player1 = GetPlayer(1)   -- creeps

    register_armor_system()
    register_hit_vfx()
    register_death_vfx()
    register_damage_text()

    -- Find the preplaced Paladin. The scene places exactly one unit for
    -- player 0; GetUnitsInRange is fine for such a small set.
    local paladin
    for _, unit in ipairs(GetUnitsInRange(0, 0, 2048)) do
        if GetUnitTypeId(unit) == "paladin" then paladin = unit end
    end
    if not paladin then
        Log("[Action] ERROR: paladin not placed")
        return
    end

    AddAbility(paladin, "holy_light")
    AddAbility(paladin, "consecration")

    -- Holy Light is `instant` in this map: no target pick, heals the
    -- caster. The sim handles cooldown + mana cost via the ability
    -- def; the level's `heal` value is plumbed here so future level-ups
    -- scale naturally.
    local hl_trig = CreateTrigger()
    TriggerRegisterUnitEvent(hl_trig, paladin, EVENT_UNIT_ABILITY_EFFECT)
    TriggerAddCondition(hl_trig, function()
        return GetTriggerAbilityId() == "holy_light"
    end)
    TriggerAddAction(hl_trig, function()
        local caster = GetTriggerUnit()
        if caster and IsUnitAlive(caster) then
            PlayEffectOnUnit("heal_glow", caster, "overhead")
            HealUnit(caster, caster, 200)
        end
    end)

    -- Lock the selection to the controlled hero. The Action preset never
    -- mutates selection; the HUD action bar reads it to fill the slot
    -- icons and abilities.
    SetControlledUnit(paladin)
    Log("[Action] Hero designated — paladin")

    -- Manual slot binding: the action bar uses `binding_mode: manual`
    -- so slots map to abilities the author picks explicitly. Only
    -- non-passive abilities make sense here (passive abilities bound
    -- to a slot draw the icon but ignore clicks/hotkeys).
    ActionBarSetSlot(1, "holy_light")
    -- Slot 2 intentionally bound to a passive as a test of the
    -- soft-convention: icon renders, key label suppressed, cast no-ops.
    ActionBarSetSlot(2, "consecration")

    -- Periodic creep spawner so there's something to hit. Waves cap at
    -- a modest count to keep the scene readable for preset testing.
    local wave          = 0
    local TOTAL_WAVES   = 10
    local SPAWN_INTERVAL = 8.0

    -- Creeps spawn on a ring ~800–1000 units from the hero so they're
    -- visible on-screen and engaged within a few seconds. Full map is
    -- ±4096 but the edges are water; keep well inside.
    CreateTimer(SPAWN_INTERVAL, true, function()
        if wave >= TOTAL_WAVES then return end
        wave = wave + 1
        local count = 2 + math.floor(wave / 2)
        for i = 1, count do
            local edge = RandomInt(0, 3)
            local x, y
            if edge == 0 then     x, y = -1000, RandomFloat(-600, 600)
            elseif edge == 1 then x, y =  1000, RandomFloat(-600, 600)
            elseif edge == 2 then x, y = RandomFloat(-600, 600), -1000
            else                  x, y = RandomFloat(-600, 600),  1000
            end
            local creep = CreateUnit("creep", player1, x, y, 0)
            if creep then IssueOrder(creep, "attack", 0, 0) end
        end
        Log(string.format("[Action] Wave %d/%d — %d creeps", wave, TOTAL_WAVES, count))
    end)

    Log("[Action] Setup complete — use WASD to move, left-click to attack, Q for Holy Light")
end
