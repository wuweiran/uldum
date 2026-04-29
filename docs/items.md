# Item System

Phase 17. Engine-side primitive that bundles ability lists into a pickup-droppable entity, with two free integer fields (`charges`, `level`) the engine renders but never interprets. Consumption logic, merge / stacking, drop-on-death, level-up rules all live in map Lua — typically a small per-map trigger or a future engine-shipped `stdlib.lua` (see *Future direction* below).

## Item kind — derived from `abilities[0]`

No `kind` field. Activeness is implicit:

- `abilities[0].form == passive` → **passive item**. Inventory slot is inert (no click / hotkey response).
- `abilities[0].form != passive` → **active item**. Slot click / hotkey fires `abilities[0]`.

Subsequent abilities in the list still apply their effects — passive modifiers, auras, on-attack triggers — but never *fire*. Authors who want a different fireable ability reorder the list.

This makes the engine's behavior a single rule: read the list, register every ability into the carrier's `ability_set`, mark the first as the slot's "use" target if it's non-passive.

## Schema — `item_types.json`

New file, sibling of `ability_types.json`. Loaded by the same type registry pass.

```json
{
  "healing_potion": {
    "name": "Healing Potion",
    "icon": "textures/icons/healing_potion.ktx2",
    "model": "models/potion.glb",
    "pickup_radius": 48,
    "abilities": ["use_healing_potion"],
    "initial_charges": 3,
    "initial_level": 1,
    "classifications": ["consumable"]
  },

  "boots_of_speed": {
    "name": "Boots of Speed",
    "icon": "textures/icons/boots.ktx2",
    "model": "models/boots.glb",
    "pickup_radius": 48,
    "abilities": ["boots_passive"]
    // initial_charges / initial_level default to 0
  }
}
```

| Field | Required | Default | Notes |
|---|---|---|---|
| `name` | yes | — | Display name (HUD tooltip later). |
| `icon` | yes | — | Slot icon (KTX2 path). |
| `model` | yes | — | Ground model (glTF). |
| `pickup_radius` | yes | 48 | World-units the carrier must be within to claim. |
| `abilities` | yes | — | Ordered list of ability ids. First = fireable if non-passive. |
| `initial_charges` | no | 0 | Initial value of the `charges` integer. |
| `initial_level` | no | 0 | Initial value of the `level` integer. |
| `classifications` | no | `[]` | String tags for `target_filter` and Lua queries. |

## Inventory

- Per-unit-type slot count via the existing `inventory_size` field in `unit_types.json`. **Default 0** — non-hero unit types have no inventory unless they declare one.
- Engine cap: **16 slots**. Loader clamps with a warning if a unit type asks for more.
- One item entity per slot. **No multi-entity stacking.** Same-type pickups create separate items in separate slots. Maps wanting WC3-style merge implement it in `on_item_picked_up`.
- All slots are "active" — every ability contributed by an item in any slot is live in the carrier's `ability_set`. No equip / hold distinction.

## Lifecycle

| State | Where | Visible | Renders |
|---|---|---|---|
| **Ground** | World entity at a position | Yes | Model + selection circle |
| **Carried** | Reference in a unit's inventory slot | No | Icon in HUD inventory composite |
| **Destroyed** | Removed | n/a | n/a |

Transitions:

- **PickUp**: smart right-click on a ground item issues `Order::PickUp(item_handle)`. Unit walks to within `pickup_radius` of the item, on arrival moves into the first free slot. Fires `EVENT_ITEM_PICKED_UP`. Failure modes: target-filter rejection (e.g. another player's hero), inventory full → drop the order with a log line.
- **Drop**: `UnitDropItemFromSlot(unit, slot)` from Lua or HUD drag-out. Item appears at unit's position. Fires `EVENT_ITEM_DROPPED`.
- **Use** (active items): same path as `Order::Cast`. Cast pipeline tracks "this cast originated from item X" so the cast event payload carries `source_item`, surfaced via `GetTriggerItem()` inside trigger actions. **The engine never decrements `charges` or destroys the item** — map Lua does.
- **Death**: no engine policy. Maps wanting drop-on-death write a single trigger.

## The two free integer fields

`charges` and `level`. Both are component data the engine stores per-item and exposes via Lua. **No engine semantics** beyond rendering them.

- Engine stores → can serialize for save/load when that lands.
- Engine renders → as corner badges (see HUD section).
- Lua reads / writes via getter / setter pairs.

That's it. No auto-decrement on cast, no destroy-at-zero, no level-up formula. Authors who want WC3-style consumables write ~10 lines of Lua:

```lua
local trig = CreateTrigger()
TriggerRegisterAnyUnitEvent(trig, EVENT_ABILITY_CAST_FINISHED)
TriggerAddAction(trig, function()
    local item = GetTriggerItem()         -- nil if cast wasn't from an item
    if not item then return end
    local c = GetItemCharges(item) - 1
    SetItemCharges(item, c)
    if c <= 0 then RemoveItem(item) end
end)
```

The engine has zero opinion on this. A map that wants charges to *recharge over time*, or to *not consume*, or to *consume two per cast*, or to *level up after N uses* — all are Lua, all using the same primitives.

## HUD — `inventory` composite

New composite alongside `action_bar` / `command_bar` / `minimap` / `joystick`. Authored under `composites.inventory` in `hud.json`. Slot row similar to `action_bar`, with these per-slot affordances:

- **Item icon** — from item type (never from any of its abilities).
- **Charges badge** — bottom-right corner. Rendered only when `charges > 0`.
- **Level badge** — top-left corner. Rendered only when `level > 0`.
- **Cooldown overlay** — from the slot's bound ability (`abilities[0]`).
- **Disabled tint** — when out of charges, out of range, no target, etc. Same logic the action_bar already uses.
- **Hotkey badge** — slot's positional hotkey (e.g. 1–6 for slots 0–5).

Items that don't care about a badge leave the underlying field at 0 and the badge stays hidden — the symmetry with charges keeps the rule trivial: "render if value > 0."

Click / tap behavior:
- **Active item** (passive abilities[0] is non-passive): fires that ability through the cast pump.
- **Passive item**: no-op (slot reads as disabled-style).

## Lua API

```lua
-- Construction
local potion = CreateItem("healing_potion", 1024.0, 768.0)
                                              -- ground at (x, y); z auto-sampled
RemoveItem(potion)                            -- destroy

-- Inventory ops
GiveItem(hero, potion)                        -- bypass walk-and-pickup; fails if full
UnitDropItemFromSlot(hero, 0)
UnitGetItemFromSlot(hero, 2)                  -- Item handle, or nil
UnitHasItemOfType(hero, "healing_potion")     -- bool
UnitItemCount(hero)                           -- 0..inventory_size

-- State
GetItemTypeId(item)                           -- "healing_potion"
GetItemCharges(item)
SetItemCharges(item, n)
GetItemLevel(item)
SetItemLevel(item, n)
GetItemOwner(item)                            -- carrying Unit, or nil if on ground
GetItemPosition(item)                         -- world position (carrier's pos if carried)

-- Events
TriggerRegisterUnitEvent(t, hero, EVENT_ITEM_PICKED_UP)
TriggerRegisterUnitEvent(t, hero, EVENT_ITEM_DROPPED)
TriggerRegisterAnyUnitEvent(t, EVENT_ABILITY_CAST_FINISHED)
GetTriggerItem()                              -- nil if event isn't item-related
```

## Network sync

Items are entities in the existing `Item` handle category (already declared in the simulation alongside `Unit` / `Destructable`). Component fields (`type_id`, `charges`, `level`, owner / slot, transform) ride the existing entity-delta sync. No new message kinds.

## Deferred

These belong to later phases or to map-side Lua, not Phase 17:

- **Shops / vendors / gold cost** — needs a generic resource system (Phase 18+ if a sample asks).
- **Item placement via editor** — Phase 19's editor expansion. v1: items in the scene's `objects.json`.
- **Auto-pickup-on-walk-over** — manual right-click only for v1.
- **Item recipes / combine** — Lua-implementable on top of triggers once items ship.
- **Drop-all-on-death**, **stack merge**, **per-class restrictions** — all map-Lua patterns, not engine policy.

## Future direction — engine-shipped Lua stdlib

WC3 shipped `blizzard.j` — a Lua-equivalent script ahead of every map's own code, providing default behavior for armor, orb effects, item charges, default tooltips, and other "expected gameplay" patterns. Maps could rely on the defaults or override them per trigger.

Once 17 ships and we have ~3 patterns recurring across sample maps (item-charge consumption, drop-on-death, same-type merge, etc.), we extract them into a single optional `engine/scripts/stdlib.lua` that loads ahead of any map's `main.lua`. Maps that want the defaults get them for free; maps that want different behavior either don't `require()` the stdlib or override specific functions.

Concretely the current Phase 17 ships none of this — the engine keeps zero policy. The stdlib is a v2 concern, and *only* gets the patterns we've actually seen multiple maps duplicate. No speculative inclusion.

## Demo coverage

A small `item_test` map (or addition to `action_test`) ships:

1. A `healing_potion` item type — active, 3 charges. Hero picks up via right-click, slot icon appears, badge shows `3`, hotkey fires the cast (heals self), a Lua trigger decrements charges and `RemoveItem`s at 0.
2. A `sword_basic` item type — passive, `abilities: ["sword_damage_passive"]` whose ability is `form: passive` with `modifiers: { damage_bonus_flat: 10 }`. Slot icon visible, no badge, hero's damage stat goes up while held.

Together those exercise: item-type loading, ground entity creation, smart-order pickup, walk-and-claim, slot rendering with both badges, cast routing with `source_item`, Lua charge consumption, passive modifier integration. If any of those paths break, one of the two items goes wrong.
