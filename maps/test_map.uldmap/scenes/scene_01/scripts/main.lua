--------------------------------------------------------------------------------
-- Test Map — Scene 01: Unit Showcase
-- A few preplaced units with basic combat. Archer attacks the grunt.
--------------------------------------------------------------------------------

require("constants")
local combat    = require("combat")
local abilities = require("abilities")

function main()
    Log("[Scene01] main() called — unit showcase")

    local player1 = Player(0)

    -- Register standard combat systems (shared)
    combat.register_armor_system()
    combat.register_hit_vfx()
    combat.register_death_vfx()
    combat.register_damage_text()

    -- Global ability-effect handlers — fire whenever any unit casts
    -- the matching ability. Scene_01's preplaced paladin can cast
    -- consecration / holy_light if the player drives it.
    abilities.register_consecration()
    abilities.register_holy_light_effect()
    abilities.register_healing_potion()

    -- Find preplaced units (world origin = map center in centered coords)
    local units = GetUnitsInRange(-596, -396, 2000)
    local archer, grunt

    for _, unit in ipairs(units) do
        local t = GetUnitTypeId(unit)
        if t == "archer" then archer = unit end
        if t == "grunt"  then grunt = unit end
    end

    -- Archer attacks the grunt
    if archer and grunt then
        IssueOrder(archer, "attack", grunt)
        Log("[Scene01] Archer ordered to attack Grunt")
    end

    setup_barracks_market()

    Log("[Scene01] Setup complete")
end

--------------------------------------------------------------------------------
-- Barracks Market — a paged shop UI stress test.
--
-- The barracks is the shop: when a unit that has an inventory (the preplaced
-- paladin) walks near it, a paged shop window opens for that unit's owner. It
-- sells Healing Potions with varying charge counts; gold is tracked in Lua
-- (no engine resource system) and deducted per purchase.
--------------------------------------------------------------------------------
function setup_barracks_market()
    local SHOP_OWNER  = Player(0)
    local SHOP_RADIUS = 400
    local PAGE_SIZE   = 3

    -- Gold, owned entirely in Lua (WC3-style; the engine has no gold resource).
    local gold = { [0] = 1000 }
    local function get_gold(pid) return gold[pid] or 0 end

    -- Six listings across two pages: one potion each, different charge counts.
    local listings = {
        { charges = 1,  price = 30  },
        { charges = 2,  price = 55  },
        { charges = 3,  price = 75  },
        { charges = 5,  price = 120 },
        { charges = 8,  price = 180 },
        { charges = 12, price = 260 },
    }
    local total_pages = math.ceil(#listings / PAGE_SIZE)
    local page = 1
    local shop_open = false
    local current_buyer = nil

    -- The shop sits at the barracks. Reuse a preplaced one if the map has it;
    -- otherwise spawn one near the first inventory-bearing unit so the feature
    -- is self-contained. Buildings spawn completed (construction is a build
    -- order, not a spawn state).
    local barracks
    for _, u in ipairs(GetUnitsInRange(0, 0, 8192)) do
        if GetUnitTypeId(u) == "barracks" then barracks = u break end
    end
    if not barracks then
        -- Anchor near a unit that can actually shop (has an inventory).
        local anchor_x, anchor_y = 0, 0
        for _, u in ipairs(GetUnitsInRange(0, 0, 8192)) do
            if UnitInventorySize(u) > 0 then
                anchor_x, anchor_y = GetUnitX(u), GetUnitY(u)
                break
            end
        end
        barracks = CreateUnit("barracks", SHOP_OWNER, anchor_x + 500, anchor_y, 0)
    end
    if not barracks then
        Log("[Scene01] ERROR: no barracks for the market")
        return
    end
    local bx, by = GetUnitX(barracks), GetUnitY(barracks)

    -- Gold panel (top-right) + shop window (center, hidden). Both go only to
    -- the shop owner's screen.
    CreateNode("gold_panel", { anchor = "tr", x = -10, y = 10, w = 170, h = 34, owner = SHOP_OWNER })
    CreateNode("shop_panel", { anchor = "mc", x = 0, y = 0, w = 360, h = 234, owner = SHOP_OWNER })

    local function refresh_gold()
        SetLabelText("gold_label", L("ui.shop.gold", { amount = get_gold(0) }))
    end

    -- Paint the current page's three rows from `listings`. Rows past the end of
    -- the list are hidden; a buy button greys out when the player can't afford
    -- it. Called on open, page change, and after every gold change.
    local function render_page()
        for row = 1, PAGE_SIZE do
            local idx = (page - 1) * PAGE_SIZE + row
            local item = listings[idx]
            if item then
                SetNodeVisible("shop_row_" .. row, true)
                SetLabelText("shop_row_" .. row .. "_name", L("ui.shop.item",  { charges = item.charges }))
                SetLabelText("shop_row_" .. row .. "_price", L("ui.shop.price", { price   = item.price }))
                SetButtonEnabled("shop_row_" .. row .. "_buy", get_gold(0) >= item.price)
            else
                SetNodeVisible("shop_row_" .. row, false)
            end
        end
        SetButtonEnabled("shop_prev", page > 1)
        SetButtonEnabled("shop_next", page < total_pages)
    end

    local function set_gold(pid, amount)
        gold[pid] = math.max(0, amount)
        refresh_gold()
        if shop_open then render_page() end
    end

    refresh_gold()

    -- Buy handler shared by all three rows: resolve the listing from the LIVE
    -- page + row (the panel is reused across pages), guard gold + a free
    -- inventory slot, then deduct and grant.
    local function buy(row)
        local idx = (page - 1) * PAGE_SIZE + row
        local item = listings[idx]
        if not item then return end
        local buyer = current_buyer
        if not buyer or not IsUnitAlive(buyer) then return end
        if get_gold(0) < item.price then return end
        if UnitInventorySize(buyer) - UnitItemCount(buyer) <= 0 then
            Log("[Scene01] Shop: inventory full")
            return
        end
        set_gold(0, get_gold(0) - item.price)
        local potion = CreateItem("potion_healing", bx, by)
        if potion then
            SetItemCharges(potion, item.charges)
            GiveItem(buyer, potion)
            Log("[Scene01] Bought potion x" .. item.charges .. " for " .. item.price .. "g")
        end
    end

    for row = 1, PAGE_SIZE do
        local trig = CreateTrigger()
        TriggerRegisterNodeEvent(trig, GetNode("shop_row_" .. row .. "_buy"), EVENT_BUTTON_PRESSED)
        TriggerAddAction(trig, function() buy(row) end)
    end

    local prev_trig = CreateTrigger()
    TriggerRegisterNodeEvent(prev_trig, GetNode("shop_prev"), EVENT_BUTTON_PRESSED)
    TriggerAddAction(prev_trig, function()
        if page > 1 then page = page - 1; render_page() end
    end)

    local next_trig = CreateTrigger()
    TriggerRegisterNodeEvent(next_trig, GetNode("shop_next"), EVENT_BUTTON_PRESSED)
    TriggerAddAction(next_trig, function()
        if page < total_pages then page = page + 1; render_page() end
    end)

    -- Proximity poll: the shop opens for the first alive, inventory-bearing unit
    -- near the barracks, and closes when none remain in range. No engine
    -- proximity event exists, so we poll (same pattern as scene_02's auto-cast).
    CreateTimer(0.4, true, function()
        local buyer
        for _, u in ipairs(GetUnitsInRange(bx, by, SHOP_RADIUS)) do
            if IsUnitAlive(u) and UnitInventorySize(u) > 0 then buyer = u break end
        end
        current_buyer = buyer
        if buyer and not shop_open then
            page = 1
            ShowNode("shop_panel")
            render_page()
            shop_open = true
        elseif not buyer and shop_open then
            HideNode("shop_panel")
            shop_open = false
        end
    end)

    Log("[Scene01] Barracks market ready at (" .. bx .. ", " .. by .. ")")
end

