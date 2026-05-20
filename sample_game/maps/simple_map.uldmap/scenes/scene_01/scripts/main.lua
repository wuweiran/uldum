--------------------------------------------------------------------------------
-- Simple Map — Walk-to-destination demo
--
-- Orders the paladin to walk east. When he reaches the goal region, the
-- session ends with the elapsed wall-clock time; the Shell UI picks that
-- up via the Results screen.
--
-- Doubles as a tiny showcase: localized DisplayMessage at intro + goal,
-- a particle burst from types/effects.json when the goal lands.
--------------------------------------------------------------------------------

-- Centered coords: world (0, 0) is map center. 32×32 tile map extends
-- (-2048..+2048) on each axis. Paladin spawns west of center, walks east
-- past the origin to the goal.
local paladin
local destination_x = 1024
local destination_y = 0
local reach_radius  = 96  -- ~3/4 of a tile at 128 u/tile

local elapsed = 0
local done    = false

function main()
    Log("[Scene01] Simple map — walk-to-destination demo")

    -- Find the preplaced paladin (only unit in the scene).
    for _, u in ipairs(GetUnitsInRange(0, 0, 4096)) do
        if GetUnitTypeId(u) == "paladin" then paladin = u end
    end
    if not paladin then
        Log("[Scene01] No paladin found — cannot start demo")
        return
    end

    -- Start walking east.
    IssueOrder(paladin, "move", destination_x, destination_y)
    DisplayMessage(L("demo.intro"))

    -- Poll 10×/s: accumulate elapsed time, check proximity to goal.
    -- GetGameTime is currently a stub (returns 0), so we count ticks
    -- ourselves. Good enough for a demo; later replace with a proper
    -- scripted stopwatch when the engine exposes one.
    CreateTimer(0.1, true, function()
        if done then return end
        elapsed = elapsed + 0.1

        local dx = GetUnitX(paladin) - destination_x
        local dy = GetUnitY(paladin) - destination_y
        local dist = math.sqrt(dx * dx + dy * dy)

        if dist < reach_radius then
            done = true
            local elapsed_str = string.format("%.1f", elapsed)
            DisplayMessage(L("demo.reached", { elapsed = elapsed_str }))
            PlayEffect("goal_burst", destination_x, destination_y, 32)
            EndGame(0, string.format('{"elapsed": %.2f}', elapsed))
        end
    end)
end
