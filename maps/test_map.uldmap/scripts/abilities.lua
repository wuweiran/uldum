--------------------------------------------------------------------------------
-- Test Map — Shared ability-effect registrations.
--
-- Per-unit handlers take the unit + owner they're bound to; global
-- handlers (potion charge consumption, holy-light heal effect) take
-- only tuning options. Scene scripts require this module and call the
-- registrations they want.
--------------------------------------------------------------------------------

require("constants")

local M = {}

-- Cleave: when `caster` deals attack damage, splash a fraction to
-- enemies of `owner` within `radius` of the primary target.
function M.register_cleave(caster, owner, opts)
    opts = opts or {}
    local radius = opts.radius or 250
    local pct    = opts.pct    or 0.30

    local trig = CreateTrigger()
    TriggerRegisterEvent(trig, EVENT_GLOBAL_DAMAGE)
    TriggerAddCondition(trig, function()
        return GetDamageType() == "attack" and GetDamageSource() == caster
    end)
    TriggerAddAction(trig, function()
        local target = GetDamageTarget()
        local dmg = GetDamageAmount() * pct
        if dmg <= 0 then return end

        local tx = GetUnitX(target)
        local ty = GetUnitY(target)
        local nearby = GetUnitsInRange(tx, ty, radius,
            { enemy_of = owner, alive_only = true })
        local hit = 0
        for _, u in ipairs(nearby) do
            if u ~= target then
                DamageUnit(caster, u, dmg, "cleave")
                hit = hit + 1
            end
        end
        if hit > 0 then
            Log(string.format("[Cleave] %.0f splash to %d enemies", dmg, hit))
        end
    end)
end

-- Consecration: target-point AoE on any consecration cast. Damages
-- enemies of the caster's owner within radius of the target point.
-- Global (any caster) so each scene only has to register once,
-- mirroring how holy_light is wired below.
function M.register_consecration(opts)
    opts = opts or {}
    local radius = opts.radius or 300
    local damage = opts.damage or 80

    local trig = CreateTrigger()
    TriggerRegisterEvent(trig, EVENT_GLOBAL_ABILITY_EFFECT)
    TriggerAddCondition(trig, function()
        return GetTriggerAbilityId() == "consecration"
    end)
    TriggerAddAction(trig, function()
        local caster = GetTriggerUnit()
        if not caster then return end
        local tx = GetSpellTargetX()
        local ty = GetSpellTargetY()
        PlayEffect("consecration_burst", tx, ty, 0)
        local owner = GetUnitOwner(caster)
        local nearby = GetUnitsInRange(tx, ty, radius,
            { enemy_of = owner, alive_only = true })
        for _, u in ipairs(nearby) do
            DamageUnit(caster, u, damage, "ability")
        end
        Log(string.format("[Consecration] %d enemies hit for %d at (%.0f,%.0f)",
            #nearby, damage, tx, ty))
    end)
end

-- Devotion Aura: pulses holy damage to enemies of `owner` within
-- radius of `caster` every `interval` seconds.
function M.register_devotion_aura(caster, owner, opts)
    opts = opts or {}
    local interval = opts.interval or 1.5
    local radius   = opts.radius   or 600
    local damage   = opts.damage   or 6

    CreateTimer(interval, true, function()
        if not IsUnitAlive(caster) then return end
        local px = GetUnitX(caster)
        local py = GetUnitY(caster)
        local nearby = GetUnitsInRange(px, py, radius,
            { enemy_of = owner, alive_only = true })
        if #nearby == 0 then return end
        PlayEffectOnUnit("devotion_pulse", caster, "overhead")
        for _, u in ipairs(nearby) do
            DamageUnit(caster, u, damage, "aura")
        end
    end)
end

-- Holy Light heal effect — global. Any unit casting holy_light heals
-- its target by the configured amount. Scenes choose separately
-- whether to auto-cast it (e.g. paladin healing footman).
function M.register_holy_light_effect(opts)
    opts = opts or {}
    local amount = opts.amount or 200

    local trig = CreateTrigger()
    TriggerRegisterEvent(trig, EVENT_GLOBAL_ABILITY_EFFECT)
    TriggerAddCondition(trig, function()
        return GetTriggerAbilityId() == "holy_light"
    end)
    TriggerAddAction(trig, function()
        local caster = GetTriggerUnit()
        local target = GetSpellTargetUnit()
        if not (caster and target and IsUnitAlive(target)) then return end
        PlayEffectOnUnit("heal_glow", target, "overhead")
        local hp_before = GetUnitHealth(target)
        HealUnit(caster, target, amount)
        local hp_after = GetUnitHealth(target)
        Log(string.format("[Holy Light] %s healed %s (HP: %.0f -> %.0f)",
            GetUnitTypeId(caster), GetUnitTypeId(target), hp_before, hp_after))
    end)
end

-- Healing potion — global. Heals the caster and decrements the item's
-- charges field; removes the item when charges hit zero.
function M.register_healing_potion(opts)
    opts = opts or {}
    local amount = opts.amount or 250

    local trig = CreateTrigger()
    TriggerRegisterEvent(trig, EVENT_GLOBAL_ABILITY_EFFECT)
    TriggerAddCondition(trig, function()
        return GetTriggerAbilityId() == "use_potion_healing"
    end)
    TriggerAddAction(trig, function()
        local caster = GetTriggerUnit()
        local item   = GetTriggerItem()
        if not caster or not IsUnitAlive(caster) then return end
        PlayEffectOnUnit("heal_potion", caster, "overhead")
        HealUnit(caster, caster, amount)
        if item then
            local c = GetItemCharges(item) - 1
            SetItemCharges(item, c)
            if c <= 0 then RemoveItem(item) end
        end
        Log(string.format("[Potion] %s healed for %d", GetUnitTypeId(caster), amount))
    end)
end

return M
