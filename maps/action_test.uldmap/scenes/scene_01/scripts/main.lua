--------------------------------------------------------------------------------
-- Action Test — Scene 01
-- Action preset test bed. One Paladin at the center controlled by the
-- player via WASD + left-click. Creeps spawn in waves from the edges to
-- give the hero something to fight. Hero has Holy Light (Q in positional
-- mode) for self-heal testing and Consecration as a target-point AoE.
--------------------------------------------------------------------------------

require("constants")
require("combat")

function main()
    Log("[Action] Scene setup starting")

    local player0 = Player(0)   -- hero
    local player1 = Player(1)   -- creeps

    register_armor_system()
    register_hit_vfx()
    register_death_vfx()
    register_damage_text()
    register_rune_system()

    -- Find the preplaced Paladin. The scene places exactly one unit for
    -- player 0; GetUnitsInRange is fine for such a small set.
    local paladin
    for _, unit in ipairs(GetUnitsInRange(0, 0, 2048)) do
        if GetUnitTypeId(unit) == "paladin" then paladin = unit end
    end
    if not paladin then
        Log("[Action] ERROR: paladin not placed")
        return
    end

    -- Engine-authored abilities (holy_light, consecration) are seeded
    -- onto the paladin from units.json — no Lua AddAbility needed.

    -- Holy Light is `instant` in this map: no target pick, heals the
    -- caster. The sim handles cooldown + mana cost via the ability
    -- def; the level's `heal` value is plumbed here so future level-ups
    -- scale naturally.
    local hl_trig = CreateTrigger()
    TriggerRegisterUnitEvent(hl_trig, paladin, EVENT_UNIT_ABILITY_EFFECT)
    TriggerAddCondition(hl_trig, function()
        return GetTriggerAbilityId() == "holy_light"
    end)
    TriggerAddAction(hl_trig, function()
        local caster = GetTriggerUnit()
        if caster and IsUnitAlive(caster) then
            PlayEffectOnUnit("heal_glow", caster, "overhead")
            HealUnit(caster, caster, 200)
        end
    end)

    -- Consecration is `target_point` + `shape: area` (300-radius AoE,
    -- 700-range cast). On effect, damage every enemy in the AoE and
    -- play the burst particle at the target point. Mobile drag-cast +
    -- desktop click-to-cast both produce the same Cast order; this
    -- handler closes the loop server-side.
    local cons_trig = CreateTrigger()
    TriggerRegisterUnitEvent(cons_trig, paladin, EVENT_UNIT_ABILITY_EFFECT)
    TriggerAddCondition(cons_trig, function()
        return GetTriggerAbilityId() == "consecration"
    end)
    TriggerAddAction(cons_trig, function()
        local caster = GetTriggerUnit()
        local tx = GetSpellTargetX()
        local ty = GetSpellTargetY()
        local radius = 300
        local damage = 80
        PlayEffect("consecration_burst", tx, ty, 0)
        -- "enemy_of = X" means "find units that are X's enemies", so we
        -- pass the caster's own player (the heroes) to get hostile
        -- units in the AoE — independent of map variable naming.
        local caster_owner = GetUnitOwner(caster)
        local nearby = GetUnitsInRange(tx, ty, radius,
            { enemy_of = caster_owner, alive_only = true })
        for _, u in ipairs(nearby) do
            DamageUnit(caster, u, damage, "ability")
        end
        Log(string.format("[Action] Consecration hit %d at (%.0f,%.0f)",
            #nearby, tx, ty))
    end)

    -- Lock the selection to the controlled hero. The Action preset never
    -- mutates selection; the HUD action bar reads it to fill the slot
    -- icons and abilities.
    SetControlledUnit(paladin)
    Log("[Action] Hero designated — paladin")

    -- Manual slot binding: the action bar uses `binding_mode: manual`
    -- so slots map to abilities the author picks explicitly. Only
    -- non-passive abilities make sense here (passive abilities bound
    -- to a slot draw the icon but ignore clicks/hotkeys).
    ActionBarSetSlot(1, "holy_light")
    ActionBarSetSlot(2, "consecration")

    -- One rune_book seeded next to the hero so the pickup path is testable
    -- without waiting on a creep drop. Creeps drop more via register_rune_system.
    local hero_x = GetUnitX(paladin)
    local hero_y = GetUnitY(paladin)
    CreateItem("rune_book", hero_x + 50, hero_y + 60)

    -- Periodic creep spawner so there's something to hit. Waves cap at
    -- a modest count to keep the scene readable for preset testing.
    local wave          = 0
    local TOTAL_WAVES   = 10
    local SPAWN_INTERVAL = 8.0

    -- Creeps spawn on a ring ~800–1000 units from the hero so they're
    -- visible on-screen and engaged within a few seconds. Full map is
    -- ±4096 but the edges are water; keep well inside.
    CreateTimer(SPAWN_INTERVAL, true, function()
        if wave >= TOTAL_WAVES then return end
        wave = wave + 1
        local count = 2 + math.floor(wave / 2)
        for i = 1, count do
            local edge = RandomInt(0, 3)
            local x, y
            if edge == 0 then     x, y = -1000, RandomFloat(-600, 600)
            elseif edge == 1 then x, y =  1000, RandomFloat(-600, 600)
            elseif edge == 2 then x, y = RandomFloat(-600, 600), -1000
            else                  x, y = RandomFloat(-600, 600),  1000
            end
            local creep = CreateUnit("creep", player1, x, y, 0)
            if creep then IssueOrder(creep, "attack", 0, 0) end
        end
        Log(string.format("[Action] Wave %d/%d — %d creeps", wave, TOTAL_WAVES, count))
    end)

    Log("[Action] Setup complete — use WASD to move, left-click to attack, Q for Holy Light")
end

-- Rune system: enemies have a 1-in-3 chance to drop a rune_book on death.
-- Picking one up rolls a random permanent stat bump and shows a matching
-- VFX pair (a burst where the book shatters + an aura over the hero). Each
-- rune keys a distinct colour so the player reads the reward at a glance.
--
-- These are permanent, same-source bumps, so they go through the base layer
-- directly (read-modify-write on GetUnitBaseAttribute → SetUnitBaseAttribute)
-- rather than stacking an ability instance per pickup. Eating 100 books is
-- then one number, not 100 AbilityInstances the tick loop walks forever.
-- (An ability would be the right call instead if these needed to expire or
-- be strippable — the base layer is for the sticky part.)
local RUNES = {
    { attr = "armor",      delta = 1,  burst = "rune_armor_burst", aura = "rune_armor_aura" },
    { attr = "move_speed", delta = 3, burst = "rune_swift_burst", aura = "rune_swift_aura" },
    { attr = "damage",     delta = 2,  burst = "rune_might_burst", aura = "rune_might_aura" },
}

function register_rune_system()
    -- Drop: 1-in-3 per creep death, spawned where the corpse fell.
    local drop_trig = CreateTrigger()
    TriggerRegisterEvent(drop_trig, EVENT_GLOBAL_DEATH)
    TriggerAddCondition(drop_trig, function()
        local unit = GetTriggerUnit()
        return unit and GetUnitTypeId(unit) == "creep" and RandomInt(1, 3) == 1
    end)
    TriggerAddAction(drop_trig, function()
        local unit = GetTriggerUnit()
        CreateItem("rune_book", GetUnitX(unit), GetUnitY(unit))
    end)

    -- Pickup: pick a random rune, bump its base stat, shatter + aura VFX.
    local pickup_trig = CreateTrigger()
    TriggerRegisterEvent(pickup_trig, EVENT_GLOBAL_ITEM_PICKED_UP)
    TriggerAddCondition(pickup_trig, function()
        local item = GetTriggerItem()
        return item and GetItemTypeId(item) == "rune_book"
    end)
    TriggerAddAction(pickup_trig, function()
        local hero = GetTriggerUnit()
        local item = GetTriggerItem()
        if not (hero and item) then return end

        local rune = RUNES[RandomInt(1, #RUNES)]
        SetUnitBaseAttribute(hero, rune.attr,
            GetUnitBaseAttribute(hero, rune.attr) + rune.delta)

        local ix, iy, iz = GetItemPosition(item)
        PlayEffect(rune.burst, ix, iy, iz)
        PlayEffectOnUnit(rune.aura, hero, "overhead")
    end)

    Log("[Action] Rune system active — creeps drop rune_book (1/3)")
end
