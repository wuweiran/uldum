--------------------------------------------------------------------------------
-- Test Map — main.lua
-- The engine calls main() after loading this file.
--------------------------------------------------------------------------------

function main()
    Log("[TestMap] main() called — setting up gameplay")

    local player1 = GetPlayer(0)

    -- Find preplaced units by type
    local units = GetUnitsInRange(55, 58, 40)
    local footman, paladin, archer, grunt

    for _, unit in ipairs(units) do
        local type_id = GetUnitTypeId(unit)
        if type_id == "footman" then footman = unit
        elseif type_id == "paladin" then paladin = unit
        elseif type_id == "archer" then archer = unit
        elseif type_id == "grunt" then grunt = unit
        end
    end

    -- Melee combat: footman attacks paladin
    if footman and paladin then
        IssueOrder(footman, "attack", paladin)
        Log("[TestMap] Footman ordered to attack Paladin")
    end

    -- Ranged combat: archer attacks grunt
    if archer and grunt then
        IssueOrder(archer, "attack", grunt)
        Log("[TestMap] Archer ordered to attack Grunt")
    end

    -- Give paladin devotion aura
    if paladin then
        AddAbility(paladin, "devotion_aura")
        Log("[TestMap] Paladin granted Devotion Aura")
    end

    -- Log deaths via trigger
    local death_trig = CreateTrigger()
    TriggerRegisterEvent(death_trig, "on_death")
    TriggerAddAction(death_trig, function()
        local unit = GetTriggerUnit()
        if unit then
            Log("[TestMap] " .. GetUnitTypeId(unit) .. " has died!")
        end
    end)

    Log("[TestMap] Setup complete")
end
