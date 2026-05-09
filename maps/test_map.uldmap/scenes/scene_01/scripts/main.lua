--------------------------------------------------------------------------------
-- Test Map — Scene 01: Unit Showcase
-- A few preplaced units with basic combat. Archer attacks the grunt.
--------------------------------------------------------------------------------

require("constants")
local combat    = require("combat")
local abilities = require("abilities")

function main()
    Log("[Scene01] main() called — unit showcase")

    local player1 = GetPlayer(0)

    -- Register standard combat systems (shared)
    combat.register_armor_system()
    combat.register_hit_vfx()
    combat.register_death_vfx()
    combat.register_damage_text()

    -- Global ability-effect handlers — fire whenever any unit casts
    -- the matching ability. Scene_01's preplaced paladin can cast
    -- consecration / holy_light if the player drives it.
    abilities.register_consecration()
    abilities.register_holy_light_effect()
    abilities.register_healing_potion()

    -- Find preplaced units (world origin = map center in centered coords)
    local units = GetUnitsInRange(-596, -396, 2000)
    local archer, grunt

    for _, unit in ipairs(units) do
        local t = GetUnitTypeId(unit)
        if t == "archer" then archer = unit end
        if t == "grunt"  then grunt = unit end
    end

    -- Archer attacks the grunt
    if archer and grunt then
        IssueOrder(archer, "attack", grunt)
        Log("[Scene01] Archer ordered to attack Grunt")
    end

    Log("[Scene01] Setup complete")
end
