--------------------------------------------------------------------------------
-- Test Map — Scene 02: Ability Test
-- Creeps spawn from edges and attack toward two heroes at the center.
-- Footman has Cleave (30% AoE on attack), Paladin has Consecration (periodic
-- AoE damage) and Holy Light (auto-heals Footman when HP is low).
-- Coordinate scale: WC3-style (1 tile = 128 game units, map = 8192x8192,
-- world origin at map center so the 64×64 grid extends (-4096..+4096) on each axis).
--------------------------------------------------------------------------------

require("constants")
local combat    = require("combat")
local abilities = require("abilities")

function main()
    Log("[Scene02] main() called — setting up ability test")

    local player1 = Player(0)   -- heroes
    local player2 = Player(1)   -- creeps

    -- Wave panel: top-center, visible only to player 0.
    -- `owner = Player(0)` marks the node as player-0-owned — the HUD
    -- render filter hides it from anyone else's screen, and once MP sync
    -- lands, the host only sends these updates to player 0's client.
    CreateNode("wave_panel", {
        anchor = "tc", x = 0, y = 20, w = 280, h = 48,
        owner  = Player(0),
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
    -- Wind Walk on the footman is the proof-of-life for the
    -- invisibility primitive (UNIT_STATUS_INVISIBLE + SetUnitAlpha
    -- composed in scripts/abilities.lua). Scene-specific because the
    -- footman's gameplay role differs per scene — scene_01's footman
    -- doesn't need it.
    AddAbility(footman, "wind_walk")
    -- Chain Frost on the paladin — proof-of-life for the projectile
    -- chain pattern (each jump is a fresh CreateProjectile bouncing
    -- to the nearest unhit enemy via PROJECTILE_HIT).
    AddAbility(paladin, "chain_frost")

    -- Register standard combat systems (shared)
    combat.register_armor_system()
    combat.register_hit_vfx()
    combat.register_death_vfx()

    -- Ability handlers (shared in scripts/abilities.lua). Global ones
    -- (consecration, holy_light, wind_walk) only need a single
    -- registration per scene; per-caster ones (cleave, devotion_aura)
    -- bind to a unit.
    abilities.register_cleave(footman, player1)
    abilities.register_consecration()
    abilities.register_devotion_aura(paladin, player1)
    abilities.register_holy_light_effect()
    abilities.register_wind_walk()
    abilities.register_chain_frost()
    Log("[Scene02] Hero abilities registered (cleave, consecration, aura, holy light, wind walk, chain frost)")

    -- Auto-cast Holy Light: paladin heals footman whenever footman drops
    -- below 80% HP. Scene-specific because the caster→target binding
    -- only makes sense for this scene's hero pair.
    CreateTimer(1.0, true, function()
        if not IsUnitAlive(paladin) then return end
        if not IsUnitAlive(footman) then return end
        if GetUnitHealth(footman) < GetUnitMaxHealth(footman) * 0.80 then
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

    local spawn_timer
    spawn_timer = CreateTimer(SPAWN_INTERVAL, true, function()
        wave = wave + 1
        -- Update the wave panel each wave — label shows "Wave N / 20"
        -- and the bar fills proportionally.
        SetLabelText("wave_label", string.format("Wave %d / %d", wave, TOTAL_WAVES))
        SetBarFill("wave_progress", wave / TOTAL_WAVES)

        -- Stop after the final wave; the panel says "/20", the spawner
        -- has to honor that contract.
        if wave >= TOTAL_WAVES then
            DestroyTimer(spawn_timer)
        end

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

    abilities.register_healing_potion()
    Log("[Scene02] Healing-potion charge consumption registered")

    -- Portal region. Authored in scene_02/objects.json as the
    -- "portal" region. Walk a hero onto it to swap to scene_01.
    -- The script derives its center and radius from the authored
    -- bounds so moving the region in the editor moves the visuals
    -- and repel math with it — no constants to keep in sync.
    local portal = GetRegion("portal")
    if not portal then
        Log("[Scene02] ERROR: portal region missing from objects.json")
        return
    end
    local px0, py0, px1, py1 = GetRegionBounds(portal)
    local portal_x = (px0 + px1) * 0.5
    local portal_y = (py0 + py1) * 0.5
    local portal_r = math.max(px1 - portal_x, py1 - portal_y)

    CreateTextTag({
        text = "[Portal: scene_01]",
        size = 28,
        pos  = { portal_x, portal_y, 200 },  -- raised above ground for legibility
        color = "#FFD600FF",
    })

    -- Portal rim VFX. Continuous emitters (emit_rate > 0) ringed around
    -- the region perimeter make the trigger volume legible without a
    -- dedicated region-overlay primitive. CreateEffect (not PlayEffect)
    -- because we want them to persist for the lifetime of the scene —
    -- the scene swap clears the EffectManager, so no manual cleanup.
    -- portal_rim's def lives in types/effects.json so both host and
    -- client have it at load time.
    local PORTAL_RIM_SEGMENTS = 16
    for i = 0, PORTAL_RIM_SEGMENTS - 1 do
        local theta = (i / PORTAL_RIM_SEGMENTS) * math.pi * 2
        CreateEffect("portal_rim",
            portal_x + math.cos(theta) * portal_r,
            portal_y + math.sin(theta) * portal_r,
            0)
    end

    -- Portal dialog. Composed from the `portal_dialog` template in
    -- hud.json (panel + two buttons + labels). Centered, hidden at
    -- spawn; ShowNode pops it on portal entry. The dialog only goes
    -- to player 0 (the unit's owner) so other peers don't see it.
    --
    -- `dialog_open` guards re-entry: in MP we don't pause the sim,
    -- so units keep moving while the prompt is up. Without the flag,
    -- a second hero (or the same one if it walks out and back in)
    -- would re-fire region_enter and we'd ShowNode on top of an
    -- already-open dialog — the player would see no change and the
    -- pending decision would silently switch which entry triggered
    -- which subsequent button press. The flag also prevents the
    -- region's leave→enter cycle (caused by the repel teleport
    -- below) from re-opening the same dialog mid-cancel.
    local dialog_open = false

    CreateNode("portal_dialog", {
        anchor = "mc", x = 0, y = 0, w = 340, h = 150,
        owner  = Player(0),
    })

    -- Push every player-0 unit currently inside the portal region out
    -- past its rim. Used after Cancel so the dialog doesn't fire on
    -- the next tick from a unit standing on top of the portal.
    -- Stop first so any in-flight move / attack-move order (or the
    -- joystick-fed move-direction the player is still holding) doesn't
    -- immediately walk the unit back into the region.
    local PORTAL_RADIUS  = 200
    local REPEL_DISTANCE = PORTAL_RADIUS + 40
    local function repel_from_portal()
        for _, u in ipairs(GetUnitsInRegion(portal)) do
            local owner = GetUnitOwner(u)
            if owner and owner.id == 0 then
                IssueOrder(u, "stop")
                local dx = GetUnitX(u) - portal_x
                local dy = GetUnitY(u) - portal_y
                local d  = math.sqrt(dx * dx + dy * dy)
                if d < 1 then dx, dy, d = -1, -1, math.sqrt(2) end
                SetUnitPosition(u,
                    portal_x + dx / d * REPEL_DISTANCE,
                    portal_y + dy / d * REPEL_DISTANCE)
            end
        end
    end

    local yes_trig = CreateTrigger()
    TriggerRegisterNodeEvent(yes_trig, GetNode("portal_dialog_yes"), EVENT_BUTTON_PRESSED)
    TriggerAddAction(yes_trig, function()
        Log("[Scene02] Dialog: Yes — loading scene_01")
        HideNode("portal_dialog")
        dialog_open = false
        if IsSinglePlayer() then UnpauseGame() end
        LoadScene("scene_01")
    end)

    local cancel_trig = CreateTrigger()
    TriggerRegisterNodeEvent(cancel_trig, GetNode("portal_dialog_cancel"), EVENT_BUTTON_PRESSED)
    TriggerAddAction(cancel_trig, function()
        Log("[Scene02] Dialog: Cancel — repelling unit, staying in scene_02")
        HideNode("portal_dialog")
        dialog_open = false
        if IsSinglePlayer() then UnpauseGame() end
        repel_from_portal()
    end)

    -- Camera pan-in for the portal-entry beat. Pan runs on real time
    -- (CameraController updates each frame, not each tick) so it would
    -- happily complete while paused — but timers don't advance during
    -- pause, so we schedule the ShowNode + PauseGame on a timer that
    -- fires once the pan finishes, *then* freeze.
    local PORTAL_PAN_DURATION = 0.5

    local portal_trig = CreateTrigger()
    TriggerRegisterEnterRegion(portal_trig, portal)
    TriggerAddAction(portal_trig, function()
        if dialog_open then return end

        local u = GetTriggerUnit()
        if not u then return end
        local owner = GetUnitOwner(u)
        if not (owner and owner.id == 0) then return end

        Log("[Scene02] Portal entered — panning camera")
        dialog_open = true   -- block re-entries during the pan window
        PanCamera(Player(0), portal_x, portal_y, PORTAL_PAN_DURATION)

        CreateTimer(PORTAL_PAN_DURATION, false, function()
            Log("[Scene02] Pan complete — opening dialog")
            ShowNode("portal_dialog")
            if IsSinglePlayer() then PauseGame() end
        end)
    end)

    Log("[Scene02] Setup complete — heroes defending center against creep waves")
end
