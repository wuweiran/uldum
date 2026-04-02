--------------------------------------------------------------------------------
-- Test Map — main.lua (Scene 02: Ability Test)
-- Creeps spawn from edges and attack toward two heroes at the center.
-- Footman has Cleave (30% AoE on attack), Paladin has Consecration (periodic
-- AoE damage) and Holy Light (auto-heals Footman when HP is low).
-- Coordinate scale: WC3-style (1 tile = 128 game units, map = 8192x8192)
--------------------------------------------------------------------------------

function main()
    Log("[Scene02] main() called — setting up ability test")

    local player1 = GetPlayer(0)   -- heroes
    local player2 = GetPlayer(1)   -- creeps

    -- Find preplaced heroes (center of 8192x8192 map)
    local units = GetUnitsInRange(4096, 4096, 1280)
    local footman, paladin

    for _, unit in ipairs(units) do
        local t = GetUnitTypeId(unit)
        if t == "footman" then footman = unit end
        if t == "paladin" then paladin = unit end
    end

    if not footman then Log("[Scene02] ERROR: footman not found!") return end
    if not paladin then Log("[Scene02] ERROR: paladin not found!") return end

    Log("[Scene02] Heroes found — Footman and Paladin at center")

    -- Give paladin abilities
    AddAbility(paladin, "devotion_aura")
    AddAbility(paladin, "holy_light")

    ---------------------------------------------------------------------------
    -- Cleave: when footman deals damage, deal 30% to nearby enemy ground units
    ---------------------------------------------------------------------------
    local cleave_trig = CreateTrigger()
    TriggerRegisterEvent(cleave_trig, "on_damage")
    TriggerAddCondition(cleave_trig, function()
        return GetDamageType() == "attack" and GetDamageSource() == footman
    end)
    TriggerAddAction(cleave_trig, function()
        local target = GetDamageTarget()
        local dmg = GetDamageAmount() * 0.30
        if dmg <= 0 then return end

        local tx = GetUnitX(target)
        local ty = GetUnitY(target)
        local nearby = GetUnitsInRange(tx, ty, 250, { enemy_of = player1, alive_only = true })
        local hit = 0
        for _, u in ipairs(nearby) do
            if u ~= target then
                DamageUnit(footman, u, dmg, "cleave")
                hit = hit + 1
            end
        end
        if hit > 0 then
            Log("[Cleave] " .. string.format("%.0f", dmg) .. " splash to " .. hit .. " enemies")
        end
    end)
    Log("[Scene02] Cleave trigger registered on Footman")

    ---------------------------------------------------------------------------
    -- Consecration: every 1 second, Paladin deals 15 damage to nearby enemies
    ---------------------------------------------------------------------------
    AddAbility(paladin, "consecration")
    CreateTimer(1.0, true, function()
        if not IsUnitAlive(paladin) then return end
        local px = GetUnitX(paladin)
        local py = GetUnitY(paladin)
        local nearby = GetUnitsInRange(px, py, 500, { enemy_of = player1, alive_only = true })
        for _, u in ipairs(nearby) do
            DamageUnit(paladin, u, 15, "ability")
        end
        if #nearby > 0 then
            Log("[Consecration] Hit " .. #nearby .. " enemies for 15 damage each")
        end
    end)
    Log("[Scene02] Consecration timer active on Paladin")

    ---------------------------------------------------------------------------
    -- Holy Light: on_ability_effect handler — heals the target
    ---------------------------------------------------------------------------
    local hl_effect_trig = CreateTrigger()
    TriggerRegisterUnitAbilityEvent(hl_effect_trig, paladin, "holy_light", "on_ability_effect")
    TriggerAddAction(hl_effect_trig, function()
        local caster = GetTriggerUnit()
        local target = GetSpellTargetUnit()
        if caster and target and IsUnitAlive(target) then
            local hp_before = GetUnitHealth(target)
            HealUnit(caster, target, 200)
            local hp_after = GetUnitHealth(target)
            Log("[Holy Light] Paladin healed " .. GetUnitTypeId(target)
                .. " (HP: " .. string.format("%.0f", hp_before) .. " -> "
                .. string.format("%.0f", hp_after) .. ")")
        end
    end)

    -- Auto-cast: periodically order paladin to cast holy_light on footman when HP < 50%
    CreateTimer(1.0, true, function()
        if not IsUnitAlive(paladin) then return end
        if not IsUnitAlive(footman) then return end

        local hp = GetUnitHealth(footman)
        local max_hp = GetUnitMaxHealth(footman)
        if hp < max_hp * 0.50 then
            IssueOrder(paladin, "cast", "holy_light", footman)
        end
    end)
    Log("[Scene02] Holy Light auto-cast active on Paladin")

    ---------------------------------------------------------------------------
    -- Creep spawning: waves from edges, attack toward center heroes
    -- (Heroes auto-acquire enemies via engine acquire_range)
    ---------------------------------------------------------------------------
    local wave = 0
    local SPAWN_INTERVAL = 6.0

    CreateTimer(SPAWN_INTERVAL, true, function()
        wave = wave + 1
        local count = math.min(2 + wave, 8)

        for i = 1, count do
            local edge = RandomInt(0, 3)
            local x, y
            if edge == 0 then     x, y = 512,  RandomFloat(1920, 6272)  -- left
            elseif edge == 1 then x, y = 7680, RandomFloat(1920, 6272)  -- right
            elseif edge == 2 then x, y = RandomFloat(1920, 6272), 512   -- bottom
            else                  x, y = RandomFloat(1920, 6272), 7680  -- top
            end

            local creep = CreateUnit("creep", player2, x, y, 0)
            if creep then
                local target = nil
                if IsUnitAlive(footman) and IsUnitAlive(paladin) then
                    local df = GetDistanceBetween(creep, footman)
                    local dp = GetDistanceBetween(creep, paladin)
                    target = (df <= dp) and footman or paladin
                elseif IsUnitAlive(footman) then
                    target = footman
                elseif IsUnitAlive(paladin) then
                    target = paladin
                end

                if target then
                    IssueOrder(creep, "attack", target)
                end
            end
        end
        Log("[Wave " .. wave .. "] Spawned " .. count .. " creeps")
    end)
    Log("[Scene02] Creep spawner active (every " .. SPAWN_INTERVAL .. "s)")

    ---------------------------------------------------------------------------
    -- Death logging
    ---------------------------------------------------------------------------
    local death_trig = CreateTrigger()
    TriggerRegisterEvent(death_trig, "on_death")
    TriggerAddAction(death_trig, function()
        local unit = GetTriggerUnit()
        if unit then
            Log("[Death] " .. GetUnitTypeId(unit) .. " has died")
        end
    end)

    Log("[Scene02] Setup complete — heroes defending center against creep waves")
end
