--------------------------------------------------------------------------------
-- Test Map — Scene 01: Unit Showcase
-- A few preplaced units with basic combat. Archer attacks the grunt.
--------------------------------------------------------------------------------

require("constants")
require("combat")

function main()
    Log("[Scene01] main() called — unit showcase")

    local player1 = GetPlayer(0)

    -- Register standard combat systems (shared)
    register_armor_system()
    register_hit_vfx()
    register_death_vfx()

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
