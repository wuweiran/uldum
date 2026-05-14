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

-- Wind Walk — global. The caster fades from full alpha to a translucent
-- ghost over `fade_time` (allies still see the ghost; enemies see
-- nothing once the invisibility flag lands), stays invisible for the
-- buff's declared duration, and pops back to full alpha on expiry.
-- Attacking while fully invisible lands an additional `unveil_bonus`
-- damage on the struck target and immediately ends invisibility.
--
-- All state-toggling is driven by two passive_flag buffs declared in
-- ability_types.json:
--   * windwalk_fading     — no_acquire, 1s
--   * windwalk_invisible  — invisible + no_acquire, 10s
-- The Lua side only owns the alpha tween (cosmetic, not yet a modifier)
-- and the unveil-strike trigger. Stack/refresh semantics, refcount
-- bookkeeping, and natural expiry all come from the engine.
function M.register_wind_walk(opts)
    opts = opts or {}
    local FADE_TIME    = opts.fade_time    or 1.0
    local FADE_STEPS   = 20
    local GHOST_PCT    = -50    -- target visual_alpha_percent at end of fade
    local UNVEIL_BONUS = opts.unveil_bonus or 80

    local cast_trig = CreateTrigger()
    TriggerRegisterEvent(cast_trig, EVENT_GLOBAL_ABILITY_EFFECT)
    TriggerAddCondition(cast_trig, function()
        return GetTriggerAbilityId() == "wind_walk"
    end)
    TriggerAddAction(cast_trig, function()
        local caster = GetTriggerUnit()
        if not caster or not IsUnitAlive(caster) then return end

        -- Recast: drop both buffs so refcounts reset, then re-apply.
        RemoveAbility(caster, "windwalk_fading")
        RemoveAbility(caster, "windwalk_invisible")

        AddAbility(caster, "windwalk_fading")

        -- Ramp the fading buff's visual_alpha_percent from 0 to
        -- GHOST_PCT over FADE_TIME. The ability owns the modifier
        -- state; we just drive the value. recalculate_modifiers fires
        -- on every SetAbilityModifier call, so the alpha tracks live.
        --
        -- Self-aborts if the buff is gone — wind walk can be broken
        -- mid-fade by another cast (see break_trig below) or by the
        -- unit dying; without this guard the timer would still finish
        -- the swap to windwalk_invisible at step 20.
        local step = 0
        local dt   = FADE_TIME / FADE_STEPS
        local fade_timer
        fade_timer = CreateTimer(dt, true, function()
            -- Self-abort if the buff was stripped externally (cast
            -- break, death, recast). Without this guard the timer
            -- would still finish the swap to windwalk_invisible even
            -- after wind walk was explicitly ended.
            if not (IsUnitAlive(caster) and HasAbility(caster, "windwalk_fading")) then
                DestroyTimer(fade_timer)
                return
            end
            step = step + 1
            local t = step / FADE_STEPS
            SetAbilityModifier(caster, "windwalk_fading",
                               "visual_alpha_percent", GHOST_PCT * t)
            if step >= FADE_STEPS then
                DestroyTimer(fade_timer)
                RemoveAbility(caster, "windwalk_fading")
                AddAbility(caster, "windwalk_invisible")
            end
        end)
    end)

    -- Break-on-cast: any ability except wind_walk itself, fired by a
    -- unit currently carrying either wind walk buff, ends invisibility.
    -- The fade-timer's self-abort above keeps the fade ramp from
    -- continuing once the fading buff has been removed.
    local break_trig = CreateTrigger()
    TriggerRegisterEvent(break_trig, EVENT_GLOBAL_ABILITY_EFFECT)
    TriggerAddCondition(break_trig, function()
        if GetTriggerAbilityId() == "wind_walk" then return false end
        local caster = GetTriggerUnit()
        if not caster then return false end
        return HasAbility(caster, "windwalk_fading")
            or HasAbility(caster, "windwalk_invisible")
    end)
    TriggerAddAction(break_trig, function()
        local caster = GetTriggerUnit()
        RemoveAbility(caster, "windwalk_fading")
        RemoveAbility(caster, "windwalk_invisible")
        Log(string.format("[WindWalk] %s revealed by cast", GetUnitTypeId(caster)))
    end)

    -- Unveiling strike: when a unit carrying windwalk_invisible deals
    -- attack damage, boost it and clear the buff (which cascades to
    -- dropping invisibility + no_acquire via refcount).
    -- HIGH priority so the strike's damage boost lands BEFORE other
    -- damage-event triggers read GetDamageAmount() — notably cleave,
    -- which then splashes a share of the boosted hit rather than the
    -- base swing.
    local strike_trig = CreateTrigger(TRIGGER_PRIORITY_HIGH)
    TriggerRegisterEvent(strike_trig, EVENT_GLOBAL_DAMAGE)
    TriggerAddCondition(strike_trig, function()
        if GetDamageType() ~= "attack" then return false end
        local src = GetDamageSource()
        if not src then return false end
        return HasAbility(src, "windwalk_invisible")
    end)
    TriggerAddAction(strike_trig, function()
        local src    = GetDamageSource()
        local target = GetDamageTarget()
        SetDamageAmount(GetDamageAmount() + UNVEIL_BONUS)
        RemoveAbility(src, "windwalk_invisible")
        local total = math.floor(GetDamageAmount() + 0.5)
        CreateTextTag{
            text      = string.format("%d!", total),
            unit      = target,
            z_offset  = 140.0,
            size      = 20,
            color     = "#FFD040FF",
            velocity  = { 0, -50 },
            lifespan  = 1.4,
            fadepoint = 0.7,
        }
        Log(string.format("[WindWalk] %s unveiling strike +%d on %s (total %d)",
            GetUnitTypeId(src), UNVEIL_BONUS, GetUnitTypeId(target), total))
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
