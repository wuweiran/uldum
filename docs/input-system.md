# Uldum Engine — Input System

Player input → game command → simulation. The engine provides built-in input
presets (control schemes); maps configure keybinds via manifest.json.

## 1. Architecture Overview

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│  Raw Input   │────>│ Input Preset │────>│   Command    │────> Simulation
│ (Platform)   │     │  (RTS/RPG)   │     │   System     │     (issue_order)
└──────────────┘     └──────────────┘     └──────────────┘
       │                    │                     │
       │                    ▼                     │
       │             ┌──────────────┐             │
       │             │  Selection   │             │
       │             │    State     │             │
       │             └──────────────┘             │
       │                                          ▼
       │                                   ┌──────────────┐
       └──────────────────────────────────>│  Lua Events  │
                                           │ (on_select,  │
                                           │  on_order)   │
                                           └──────────────┘
```

### Layers

1. **Platform** — raw mouse/keyboard/touch events (already exists)
2. **Input Preset** — translates raw input into game actions (select, order, cast)
3. **Selection State** — per-player selected units, subgroups, control groups
4. **Command System** — validates and submits `GameCommand`s to simulation
5. **Lua Events** — scripts can observe and intercept

## 2. Commands vs Abilities

Player actions are split into two categories:

### Commands

Built-in engine actions with fixed behavior. Defined per unit type in
`"commands"`:

```json
"paladin": {
    "commands": ["attack", "move", "stop", "hold_position"],
    "abilities": ["holy_light", "divine_shield", "devotion_aura", "resurrection"],
    ...
}
```

Commands are:
- **attack** — right-click enemy / A-click
- **move** — right-click ground
- **stop** — halt all actions
- **hold_position** — stop and don't auto-acquire
- **patrol** — move between waypoints, attack enemies in path

Input presets handle commands through keybindings (S = stop, H = hold, A =
attack-move). Commands are not slotted — they appear in a fixed area of the
UI (in WC3, the bottom row of the command card).

### Ability Slots

Everything else: active spells, passive auras, toggles, channels. These
go into numbered **slots** (0 through `MAX_ABILITY_SLOTS - 1`, currently 16).

Initial slot assignment comes from the `"abilities"` list order in the unit
type definition. Lua scripts can modify slots at runtime:

```lua
AddAbility(unit, "berserk")         -- auto-assigns to first empty slot
RemoveAbility(unit, "berserk")      -- removes ability, clears its slot
SetAbilitySlot(unit, "berserk", 3)  -- move existing ability to slot 3
ClearSlot(unit, 3)                  -- unslot (ability stays on unit, not in UI)
UnslotAbility(unit, "berserk")      -- same but by ability ID
SwapSlots(unit, 2, 4)               -- swap two slots (either can be empty)
GetAbilitySlot(unit, "berserk")     -- returns slot index, or -1
GetSlotAbility(unit, 3)             -- returns ability ID, or nil
```

**Slot rules:**
- Slots are a fixed-size array (16 entries). Empty slots are allowed.
- `RemoveAbility` clears the slot — no shifting. Other slots keep their indices.
- `AddAbility` auto-assigns to the first empty slot if the ability is not hidden.
- If all slots are full, the ability is still added (functional) but not slotted.
- `hidden` abilities (see ability-system.md) bypass slot assignment entirely.
- Passive abilities can be slotted — they show in the UI with icon/tooltip.

## 3. Game Commands

A `GameCommand` represents a player's intent. All player interaction with the
simulation goes through commands — input presets produce them, Lua can produce
them, and (later) the network layer serializes them.

```cpp
struct GameCommand {
    Player              player;      // who issued it
    std::vector<Unit>   units;       // selected units to receive the order
    OrderPayload        order;       // reuse existing order.h variant
    bool                queued;      // shift-queued
};
```

### Command flow

```
Input Preset                    Lua IssueOrder
     │                               │
     ▼                               ▼
GameCommand { player, units, order, queued }
     │
     ▼
CommandSystem::submit(cmd)
     │
     ├── validate ownership (units belong to player)
     ├── fire on_order Lua event (can cancel)
     └── for each unit: issue_order(world, unit, order)
```

In multiplayer (Phase 13), the client sends `GameCommand` to the server instead
of executing locally. The server validates and executes. The flow is identical.

## 4. Selection State

Selection is client-side — the server never needs to know what a player has
selected. It only receives commands that reference specific units.

```cpp
struct SelectionState {
    Player                  player;
    std::vector<Unit>       selected;       // current selection (ordered by priority)
    std::array<std::vector<Unit>, 10> control_groups; // ctrl+0..9
};
```

### Selection rules (WC3-style)

- **Click**: select single unit. Prefer highest priority within click radius.
- **Box drag**: select all own units in box.
- **Shift+click/box**: toggle add/remove from selection.
- **Ctrl+click**: select all visible units of same type.
- **Double click**: select all visible units of same type (same as ctrl+click).
- **Tab**: cycle subgroup (when multiple unit types selected).
- **Ctrl+N**: assign control group N (0-9).
- **N**: recall control group N.
- **Selection limit**: 24 units max.

### Picking

The engine provides picking helpers used by input presets:

```cpp
Unit pick_unit(f32 screen_x, f32 screen_y) const;
std::vector<Unit> pick_units_in_box(f32 x0, f32 y0, f32 x1, f32 y1) const;
bool screen_to_world(f32 screen_x, f32 screen_y, glm::vec3& world_pos) const;
```

## 5. Input Presets

An input preset translates raw input into selections, commands, and camera
movement. Maps choose which preset to use via `manifest.json`.

### Interface

```cpp
class InputPreset {
public:
    virtual ~InputPreset() = default;
    virtual void update(const InputContext& ctx, f32 dt) = 0;
};
```

`InputContext` bundles everything the preset needs:

```cpp
struct InputContext {
    const platform::InputState& input;
    SelectionState&             selection;
    CommandSystem&              commands;
    Picker&                     picker;
    render::Camera&             camera;
    const InputBindings&        bindings;    // action-to-key mapping
    const simulation::Simulation& simulation;
    u32 screen_w;
    u32 screen_h;
};
```

### InputBindings

A flat map from action ID strings to key name strings. Handles rising-edge
detection internally.

```cpp
struct InputBindings {
    bool action_pressed(const std::string& action,
                        const platform::InputState& input) const;
    void load(const nlohmann::json& j);
    void apply_defaults(const std::unordered_map<std::string, std::string>& defaults);
};
```

Key names match `InputState` field names without the `key_` prefix:
`"S"`, `"H"`, `"A"`, `"F1"`, `"Escape"`, etc.

### Preset Factory

```cpp
std::unique_ptr<InputPreset> create_preset(std::string_view name);
// "rts" → RtsPreset, "action_rpg" → (Phase 12d)
```

### RTS Preset

Standard WC3-style controls. Preset `"rts"` in `manifest.json`.

| Aspect | Behavior |
|---|---|
| Selection | Multi-select. Click, shift-click, box-drag. Ctrl-click / double-click selects all of same type on screen. Selection persists until changed. Max 24 units. |
| Control groups | `Ctrl+0..9` to bind, `0..9` to recall. |
| Movement | Right-click ground → selected units pathfind there. Shift queues. |
| Targeting | No persistent target slot. Right-click enemy → selected units attack. Ability buttons that need a target enter reticle mode → click picks. |
| Camera | Free pan (edge-scroll + arrow keys on desktop, two-finger pan on mobile), mouse wheel / pinch zoom. No auto-follow. |
| Hotkeys | Each ability slot has an optional `"hotkey"` field; pressing it issues the ability on selection. Commands (`stop`, `hold`, `attack_move`, `patrol`) bind per-map. |
| Mobile | Same semantics. Tap replaces click; tap-drag replaces box-drag; HUD `action_bar` slot buttons replace hotkeys. |

Default command keybindings (overridable per map):

| Action ID | Default Key |
|---|---|
| `stop` | S |
| `hold` | H |
| `attack_move` | A |
| `patrol` | P |
| `select_hero_1` | F1 |
| `select_hero_2` | F2 |
| `select_hero_3` | F3 |

**Ability activation (RTS):** The RTS preset reads the selected unit's ability
slots. When a key is pressed, it checks if any slotted ability has a matching
`"hotkey"` value. If found, the preset enters targeting mode (for target_unit /
target_point forms) or instant-casts (for no-target / toggle forms).

### Action Preset

Single-hero, direct-control. Preset `"action_rpg"` in `manifest.json`. Targeting
is **explicit and persistent** for v1 — soft-target (Diablo) and lock-on
(Souls-like) are out of scope.

| Aspect | Behavior |
|---|---|
| Selection | **None.** The player is the hero; there is no "selected units" concept. |
| Movement | WASD on desktop; virtual `joystick` composite on mobile. No click-to-move. |
| Targeting | Explicit, persistent. `Tab` cycles nearest enemies; click picks; tap picks on mobile. Target slot holds one entity until it changes or dies. All slotted abilities that need a target use the target slot unless ground-placed. |
| Camera | Chase cam: position tracks the hero; player controls rotation (mouse look on desktop, swipe on mobile) and zoom (wheel / pinch). No free pan. |
| Hotkeys (desktop) | Ability slots are bound per-slot (`1..9`, `Q W E R F`). Slot 0 defaults to `Q`, slot 1 `W`, etc. The ability's `"hotkey"` field is ignored. |
| Hotkeys (mobile) | No keys. Every occupied slot becomes a touch button on the `action_bar` composite. |
| Control groups | N/A — one actor. |

**Ability activation (Action desktop):** Slot-based. Keys are bound per slot in
input bindings. Targeting uses the current target slot; ground-placed abilities
enter reticle mode (click picks ground).

**Ability activation (Action mobile):** Each occupied slot becomes a touch
button. Targeting interaction is derived from the ability's `form`:

- `instant` / `toggle` → tap button
- `target_unit` → tap button, uses current target slot (no extra tap needed)
- `target_point` → tap button, then tap ground (or drag from button)
- `channel` → tap button to start, tap again to cancel

## 6. Map Configuration

Maps configure input via the `"input"` section in `manifest.json`:

```json
{
    "input": {
        "preset": "rts",
        "bindings": {
            "stop": "S",
            "hold": "H",
            "attack_move": "A",
            "patrol": "P"
        }
    }
}
```

- `"preset"`: selects the input preset class. Default: `"rts"`.
- `"bindings"`: overrides default keybindings. Omitted actions use preset defaults.

The entire `"input"` section is optional. If absent, defaults to RTS preset
with built-in bindings.

## 7. Lua Events

### on_order

Fired AFTER a command has been issued to its target units. Pure observer
— handlers can read the command context and trigger side effects (logs,
SFX, score updates, follow-up `IssueOrder` calls), but cannot cancel
the order. Cast validity is decided upstream by the ability's
`target_filter` (which both client and server evaluate against the
same synced state); this hook is for "an order *was* issued"
reactions, not for last-mile validation.

```lua
local trig = CreateTrigger()
TriggerRegisterEvent(trig, "on_order")
TriggerAddAction(trig, function()
    local order_type = GetOrderType()      -- "move", "attack", "stop", etc.
    local player     = GetOrderPlayer()
    local tx, ty     = GetOrderTargetX(), GetOrderTargetY()
    local target     = GetOrderTargetUnit()
    local units      = GetOrderUnits()

    -- Reactions, not gating: count orders, play SFX, log telemetry,
    -- chain a follow-up IssueOrder, etc.
    if order_type == "cast" then
        increment_cast_counter(player)
    end
end)
```

### on_select

Fired after selection changes. Not cancellable.

```lua
local trig = CreateTrigger()
TriggerRegisterEvent(trig, "on_select")
TriggerAddAction(trig, function()
    local units = GetSelectedUnits()
    local count = GetSelectedUnitCount()
    -- e.g. update custom UI
end)
```

### Lua Selection API

```lua
GetSelectedUnits()          -- returns table of Unit handles
GetSelectedUnitCount()      -- returns integer
SelectUnit(unit)            -- replace selection with one unit
SelectUnits(table)          -- replace selection with multiple
ClearSelection()            -- clear
IsUnitSelected(unit)        -- boolean
```

## 8. Interaction with Server (Phase 13)

In single player, commands execute immediately:

```
InputPreset → GameCommand → CommandSystem → issue_order()
```

In multiplayer, the flow becomes:

```
InputPreset → GameCommand → NetworkClient.send(cmd) → Server → issue_order()
```

The `CommandSystem` interface stays the same. In multiplayer mode, `submit()`
sends over the network instead of executing locally. The input preset and
selection state are completely unaware of networking.

## 9. UI System Interaction

The HUD consumes the slot system — it never drives input. Both presets use the
same `action_bar` composite (see [ui.md](ui.md)): a slot group where each slot
shows icon / cooldown ring / hotkey badge / disabled overlay for its bound
ability. Click or hotkey fires a Lua callback that issues the command.

The differences between presets are in **how slots get bound**, not in which
composite is drawn:

- **RTS**: Lua hooks `on_select` and rebinds `action_bar` slots to the primary
  selected unit's slot array. Empty selection → slots show empty.
- **Action**: Lua binds `action_bar` slots once from the hero's abilities; the
  binding stays until Lua updates it (level up, ability swap).

Input presets and HUD composites are independent readers of the same underlying
data — commands list, ability slots, ability definitions. The HUD never bypasses
the command system; its click / hotkey callbacks issue `GameCommand`s exactly
like raw input does.
