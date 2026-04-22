--------------------------------------------------------------------------------
-- Simple Map — Walk-to-destination demo
--
-- Orders the paladin to walk east. When he reaches the goal region, the
-- session ends with the elapsed wall-clock time; the Shell UI picks that
-- up via the Results screen.
--------------------------------------------------------------------------------

local paladin
local destination_x = 3072
local destination_y = 2048
local reach_radius  = 96  -- ~3/4 of a tile at 128 u/tile

local elapsed = 0
local done    = false

function main()
    Log("[Scene01] Simple map — walk-to-destination demo")

    -- Find the preplaced paladin (only unit in the scene).
    for _, u in ipairs(GetUnitsInRange(2048, 2048, 4096)) do
        if GetUnitTypeId(u) == "paladin" then paladin = u end
    end
    if not paladin then
        Log("[Scene01] No paladin found — cannot start demo")
        return
    end

    -- Start walking east.
    IssueOrder(paladin, "move", destination_x, destination_y)
    Log(string.format("[Scene01] Ordered paladin to (%d, %d)", destination_x, destination_y))

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
            Log(string.format("[Scene01] Reached goal in %.1fs", elapsed))
            EndGame(0, string.format('{"elapsed": %.2f}', elapsed))
        end
    end)
end
