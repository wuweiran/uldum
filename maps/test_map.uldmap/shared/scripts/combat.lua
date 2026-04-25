--------------------------------------------------------------------------------
-- combat.lua — Shared combat systems (armor, VFX)
-- Used by all scenes via require("combat")
--------------------------------------------------------------------------------

--- Register WC3-style armor damage reduction.
-- Runs at HIGH priority so cleave/other effects see the reduced amount.
-- Formula: reduction = armor * 0.06 / (1 + armor * 0.06)
function register_armor_system()
    local armor_trig = CreateTrigger(TRIGGER_PRIORITY_HIGH)
    TriggerRegisterEvent(armor_trig, EVENT_GLOBAL_DAMAGE)
    TriggerAddCondition(armor_trig, function()
        return GetDamageType() == "attack"
    end)
    TriggerAddAction(armor_trig, function()
        local target = GetDamageTarget()
        local armor = GetUnitAttribute(target, "armor")
        if armor > 0 then
            local reduction = armor * 0.06 / (1 + armor * 0.06)
            local dmg = GetDamageAmount() * (1 - reduction)
            SetDamageAmount(dmg)
        end
    end)
    Log("[Combat] Armor damage reduction active (WC3 formula)")
end

--- Register hit spark VFX on attack damage.
function register_hit_vfx()
    local hit_vfx = CreateTrigger(TRIGGER_PRIORITY_LOW)
    TriggerRegisterEvent(hit_vfx, EVENT_GLOBAL_DAMAGE)
    TriggerAddCondition(hit_vfx, function()
        return GetDamageType() == "attack" and GetDamageAmount() > 0
    end)
    TriggerAddAction(hit_vfx, function()
        local target = GetDamageTarget()
        if target then
            PlayEffectOnUnit("hit_spark", target, "chest")
        end
    end)
end

--- Register floating damage numbers. Fires on attack damage and creates
--- a red text tag above the target unit that rises and fades out.
function register_damage_text()
    local trig = CreateTrigger(TRIGGER_PRIORITY_LOW)
    TriggerRegisterEvent(trig, EVENT_GLOBAL_DAMAGE)
    TriggerAddCondition(trig, function()
        return GetDamageAmount() > 0
    end)
    TriggerAddAction(trig, function()
        local target = GetDamageTarget()
        if not target then return end
        CreateTextTag{
            text      = string.format("-%d", math.floor(GetDamageAmount() + 0.5)),
            unit      = target,
            z_offset  = 120.0,
            size      = 12,
            color     = "#FF5050FF",
            velocity  = { 0, -40 },     -- rise up the screen (y-down)
            lifespan  = 1.2,
            fadepoint = 0.6,
        }
    end)
    Log("[Combat] Damage text numbers active")
end

--- Register death burst VFX + logging.
function register_death_vfx()
    local death_trig = CreateTrigger()
    TriggerRegisterEvent(death_trig, EVENT_GLOBAL_DEATH)
    TriggerAddAction(death_trig, function()
        local unit = GetTriggerUnit()
        if unit then
            PlayEffectOnUnit("death_burst", unit, "chest")
            Log("[Death] " .. GetUnitTypeId(unit) .. " has died")
        end
    end)
end
