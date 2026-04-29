--------------------------------------------------------------------------------
-- Test Map — Scene 02: Ability Test
-- Creeps spawn from edges and attack toward two heroes at the center.
-- Footman has Cleave (30% AoE on attack), Paladin has Consecration (periodic
-- AoE damage) and Holy Light (auto-heals Footman when HP is low).
-- Coordinate scale: WC3-style (1 tile = 128 game units, map = 8192x8192,
-- world origin at map center so the 64×64 grid extends (-4096..+4096) on each axis).
--------------------------------------------------------------------------------

require("constants")
require("combat")

function main()
    Log("[Scene02] main() called — setting up ability test")

    local player1 = GetPlayer(0)   -- heroes
    local player2 = GetPlayer(1)   -- creeps

    -- Wave panel: top-center, visible only to player 0.
    -- `owner = GetPlayer(0)` marks the node as player-0-owned — the HUD
    -- render filter hides it from anyone else's screen, and once MP sync
    -- lands, the host only sends these updates to player 0's client.
    CreateNode("wave_panel", {
        anchor = "tc", x = 0, y = 20, w = 280, h = 48,
        owner  = GetPlayer(0),
    })

    -- Find preplaced heroes (world origin = map center)
    local units = GetUnitsInRange(0, 0, 1280)
    local footman, paladin

    for _, unit in ipairs(units) do
        local t = GetUnitTypeId(unit)
        if t == "footman" then footman = unit end
        if t == "paladin" then paladin = unit end
    end

    if not footman then Log("[Scene02] ERROR: footman not found!") return end
    if not paladin then Log("[Scene02] ERROR: paladin not found!") return end

    Log("[Scene02] Heroes found — Footman and Paladin at center")

    -- Engine-authored abilities (holy_light, consecration) are seeded
    -- from the paladin's `abilities` list in unit_types.json at create
    -- time — no Lua AddAbility needed for them. devotion_aura's effects
    -- are simulated entirely in this script (aura damage tick + buff
    -- VFX), so we keep adding it here so its def is bound to the
    -- caster for the action_bar to render the icon. If a future engine
    -- aura mechanic supplants the script side, this line moves into
    -- unit_types.json alongside the others.
    AddAbility(paladin, "devotion_aura")

    -- Register standard combat systems (shared)
    register_armor_system()
    register_hit_vfx()
    register_death_vfx()
    register_damage_text()

    ---------------------------------------------------------------------------
    -- Cleave: when footman deals damage, deal 30% to nearby enemy ground units
    ---------------------------------------------------------------------------
    local cleave_trig = CreateTrigger()
    TriggerRegisterEvent(cleave_trig, EVENT_GLOBAL_DAMAGE)
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
    -- Consecration: target-point AoE cast (declared in ability_types.json,
    -- seeded onto the paladin via unit_types.json). On cast resolution,
    -- deals ability.damage to every enemy in `area.radius` around the
    -- target point and bursts a particle effect there. Mobile uses
    -- drag-cast to pick the target point; desktop uses click-to-cast
    -- targeting.
    ---------------------------------------------------------------------------
    local consecration_trig = CreateTrigger()
    TriggerRegisterUnitEvent(consecration_trig, paladin, EVENT_UNIT_ABILITY_EFFECT)
    TriggerAddCondition(consecration_trig, function()
        return GetTriggerAbilityId() == "consecration"
    end)
    TriggerAddAction(consecration_trig, function()
        local caster = GetTriggerUnit()
        local tx = GetSpellTargetX()
        local ty = GetSpellTargetY()
        local radius = 300
        local damage = 80
        PlayEffect("consecration_burst", tx, ty, 0)
        local caster_owner = GetUnitOwner(caster)
        local nearby = GetUnitsInRange(tx, ty, radius,
            { enemy_of = caster_owner, alive_only = true })
        for _, u in ipairs(nearby) do
            DamageUnit(caster, u, damage, "ability")
        end
        Log(string.format("[Consecration] %d enemies hit for %d at (%.0f,%.0f)",
            #nearby, damage, tx, ty))
    end)
    Log("[Scene02] Consecration ability registered (target_point, AoE)")

    ---------------------------------------------------------------------------
    -- Devotion Aura: bonus to allies (handled by engine aura system) AND
    -- a slow holy-damage tick to surrounding enemies. The buff side is
    -- driven by the engine's aura scan (see ability_types.json); this
    -- timer adds the offensive half map-side, which is also where the
    -- pulse VFX lives.
    ---------------------------------------------------------------------------
    local AURA_DAMAGE_INTERVAL = 1.5
    local AURA_RADIUS          = 600
    local AURA_DAMAGE          = 6
    CreateTimer(AURA_DAMAGE_INTERVAL, true, function()
        if not IsUnitAlive(paladin) then return end
        local px = GetUnitX(paladin)
        local py = GetUnitY(paladin)
        local nearby = GetUnitsInRange(px, py, AURA_RADIUS, { enemy_of = player1, alive_only = true })
        if #nearby == 0 then return end
        PlayEffectOnUnit("devotion_pulse", paladin, "overhead")
        for _, u in ipairs(nearby) do
            DamageUnit(paladin, u, AURA_DAMAGE, "aura")
        end
    end)
    Log("[Scene02] Devotion Aura damage tick active on Paladin")

    ---------------------------------------------------------------------------
    -- Holy Light: on_ability_effect handler — heals the target
    ---------------------------------------------------------------------------
    local hl_effect_trig = CreateTrigger()
    TriggerRegisterUnitEvent(hl_effect_trig, paladin, EVENT_UNIT_ABILITY_EFFECT)
    TriggerAddCondition(hl_effect_trig, function()
        return GetTriggerAbilityId() == "holy_light"
    end)
    TriggerAddAction(hl_effect_trig, function()
        local caster = GetTriggerUnit()
        local target = GetSpellTargetUnit()
        if caster and target and IsUnitAlive(target) then
            PlayEffectOnUnit("heal_glow", target, "overhead")
            local hp_before = GetUnitHealth(target)
            HealUnit(caster, target, 200)
            local hp_after = GetUnitHealth(target)
            Log("[Holy Light] Paladin healed " .. GetUnitTypeId(target)
                .. " (HP: " .. string.format("%.0f", hp_before) .. " -> "
                .. string.format("%.0f", hp_after) .. ")")
        end
    end)

    -- Auto-cast: periodically order paladin to cast holy_light on footman when HP < 80%
    CreateTimer(1.0, true, function()
        if not IsUnitAlive(paladin) then return end
        if not IsUnitAlive(footman) then return end

        local hp = GetUnitHealth(footman)
        local max_hp = GetUnitMaxHealth(footman)
        if hp < max_hp * 0.80 then
            IssueOrder(paladin, "cast", "holy_light", footman)
        end
    end)
    Log("[Scene02] Holy Light auto-cast active on Paladin")

    ---------------------------------------------------------------------------
    -- Creep spawning: waves from edges, attack toward center heroes
    -- (Heroes auto-acquire enemies via engine acquire_range)
    ---------------------------------------------------------------------------
    local wave = 0
    local TOTAL_WAVES    = 20
    local SPAWN_INTERVAL = 6.0

    CreateTimer(SPAWN_INTERVAL, true, function()
        wave = wave + 1
        -- Update the wave panel each wave — label shows "Wave N / 20"
        -- and the bar fills proportionally.
        SetLabelText("wave_label", string.format("Wave %d / %d", wave, TOTAL_WAVES))
        SetBarFill("wave_progress", wave / TOTAL_WAVES)

        local count = math.min(2 + wave, 8)

        for i = 1, count do
            local edge = RandomInt(0, 3)
            local x, y
            if edge == 0 then     x, y = -3584, RandomFloat(-2176, 2176)  -- left
            elseif edge == 1 then x, y =  3584, RandomFloat(-2176, 2176)  -- right
            elseif edge == 2 then x, y = RandomFloat(-2176, 2176), -3584  -- bottom
            else                  x, y = RandomFloat(-2176, 2176),  3584  -- top
            end

            local creep = CreateUnit("creep", player2, x, y, 0)
            if creep then
                IssueOrder(creep, "attack", 0, 0)
            end
        end
        Log("[Wave " .. wave .. "] Spawned " .. count .. " creeps")
    end)
    Log("[Scene02] Creep spawner active (every " .. SPAWN_INTERVAL .. "s)")

    ---------------------------------------------------------------------------
    -- Items: drop two healing potions and one ring-of-armor near the
    -- paladin so the player can right-click to pick them up. The cast
    -- of `use_potion_healing` heals the caster and the trigger below
    -- decrements the item's `charges` field, removing the item when it
    -- reaches zero. Ring of Armor is a passive item — its slot icon
    -- shows but clicking does nothing; the armor bonus applies while
    -- carried.
    ---------------------------------------------------------------------------
    CreateItem("potion_healing", -1275, -930)
    Log("[Scene02] Spawned potion_healing at (-1275, -930)")

    -- Heal-on-use: fire when the use_potion_healing instant resolves,
    -- regardless of caster. Heal target = caster (potion form is
    -- self-only).
    local heal_use_trig = CreateTrigger()
    TriggerRegisterEvent(heal_use_trig, EVENT_GLOBAL_ABILITY_EFFECT)
    TriggerAddCondition(heal_use_trig, function()
        return GetTriggerAbilityId() == "use_potion_healing"
    end)
    TriggerAddAction(heal_use_trig, function()
        local caster = GetTriggerUnit()
        local item   = GetTriggerItem()
        if not caster or not IsUnitAlive(caster) then return end
        PlayEffectOnUnit("heal_glow", caster, "overhead")
        HealUnit(caster, caster, 250)
        if item then
            local c = GetItemCharges(item) - 1
            SetItemCharges(item, c)
            if c <= 0 then RemoveItem(item) end
        end
        Log("[Potion] " .. GetUnitTypeId(caster) .. " healed for 250")
    end)
    Log("[Scene02] Healing-potion charge consumption registered")

    Log("[Scene02] Setup complete — heroes defending center against creep waves")
end
