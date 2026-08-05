local M = {}

function M.register_paladin(paladin, player)
    local revived = CreateTrigger()
    TriggerRegisterUnitEvent(revived, paladin, EVENT_UNIT_REVIVED)
    TriggerAddAction(revived, function()
        PlayEffectOnUnit("revive_glow", paladin, "overhead")
        Log("[Revive] Paladin revived at death position")
    end)

    local death = CreateTrigger()
    TriggerRegisterUnitEvent(death, paladin, EVENT_UNIT_DEATH)
    TriggerAddAction(death, function()
        local x = GetUnitX(paladin)
        local y = GetUnitY(paladin)
        local remaining = 60
        local countdown

        local tag = CreateTextTag({
            text = L("ui.revive_countdown", { seconds = remaining }),
            style = "permanent",
            size = 22,
            pos = { x, y, 150 },
            color = "#FFD65CFF",
            owner = player,
        })

        countdown = CreateTimer(1.0, true, function()
            remaining = remaining - 1
            if remaining <= 0 then
                DestroyTimer(countdown)
                DestroyTextTag(tag)
                ReviveUnit(paladin, x, y)
                return
            end
            SetTextTagText(tag, L("ui.revive_countdown", { seconds = remaining }))
        end)
    end)
end

return M
