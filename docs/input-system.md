# Uldum Engine — Input System

Player input → game command → simulation. The engine provides built-in input
presets (control schemes); maps configure keybinds and UI layout.

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

## 2. Game Commands

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

This wraps the existing `Order` + `OrderPayload` (from `order.h`) with the
player and unit list. The command system iterates `units` and calls
`issue_order()` for each.

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

## 3. Selection State

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
- **Box drag**: select all own units in box. Enemy units only if no own units.
- **Shift+click/box**: toggle add/remove from selection.
- **Ctrl+click**: select all visible units of same type.
- **Double click**: select all visible units of same type (same as ctrl+click).
- **Tab**: cycle subgroup (when multiple unit types selected).
- **Ctrl+N**: assign control group N (0-9).
- **N**: recall control group N.
- **Selection limit**: 24 units max (like WC3's 12, but doubled).

### Picking

The engine provides picking helpers used by input presets:

```cpp
// Pick the best unit under screen coordinates (closest to cursor, highest priority)
Unit pick_unit(f32 screen_x, f32 screen_y) const;

// Collect all units within a screen-space rectangle
std::vector<Unit> pick_units_in_box(f32 x0, f32 y0, f32 x1, f32 y1) const;

// Convert screen position to world position on terrain
bool screen_to_world(f32 screen_x, f32 screen_y, glm::vec3& world_pos) const;
```

These use the camera's view-projection to unproject screen coords, then test
against `Selectable` components using `selection_radius`.

## 4. Input Presets

An input preset is a class that receives raw input each frame and produces
game actions (selections, commands, camera movement). The engine ships two
presets; maps choose which one to use via `manifest.json`.

### Interface

```cpp
class InputPreset {
public:
    virtual ~InputPreset() = default;

    // Called each frame with raw input, dt, and access to picking/selection.
    virtual void update(const InputContext& ctx, f32 dt) = 0;

    // Called when a key/button binding fires (map-configurable).
    virtual void on_action(std::string_view action, const InputContext& ctx) = 0;
};
```

`InputContext` bundles everything the preset needs:

```cpp
struct InputContext {
    const platform::InputState& input;      // raw mouse/keyboard
    SelectionState&             selection;
    CommandSystem&              commands;
    const render::Camera&       camera;

    // Picking helpers
    Unit pick_unit(f32 sx, f32 sy) const;
    std::vector<Unit> pick_units_in_box(f32 x0, f32 y0, f32 x1, f32 y1) const;
    bool screen_to_world(f32 sx, f32 sy, glm::vec3& pos) const;
};
```

### RTS Preset

Standard WC3-style controls for desktop:

| Input | Action |
|---|---|
| Left click | Select unit under cursor (or deselect if empty ground) |
| Left drag | Box select |
| Right click ground | Smart order: Move (or AttackMove if in attack-move mode) |
| Right click enemy | Attack order |
| Right click ally | Follow order (or right-click ability if applicable) |
| Shift + right click | Queue order |
| S | Stop |
| H | Hold position |
| A + left click | Attack-move to point |
| P | Patrol (click waypoints) |
| Ability hotkeys | Cast ability (may need target click) |
| Ctrl+0-9 | Assign control group |
| 0-9 | Recall control group |
| F1-F3 | Select hero 1-3 |
| Edge pan / arrow keys | Camera pan |
| Mouse wheel | Camera zoom |

### Action/RPG Preset (Mobile, deferred)

| Input | Action |
|---|---|
| Virtual joystick | Direct movement of controlled unit |
| Tap unit | Select / target |
| Ability buttons | Cast (immediate or tap-target) |
| Drag ability button | Directional cast |

## 5. Map Configuration

Maps configure input via `manifest.json` and an optional `input.json`.

### manifest.json additions

```json
{
    "input_preset": "rts",
    "max_selection": 24
}
```

### input.json (optional, per map)

Overrides default keybinds and defines the ability bar layout.

```json
{
    "keybinds": {
        "stop": "S",
        "hold": "H",
        "attack_move": "A",
        "patrol": "P",
        "ability_1": "Q",
        "ability_2": "W",
        "ability_3": "E",
        "ability_4": "R"
    },
    "ability_bar": {
        "rows": 3,
        "cols": 4,
        "position": "bottom_right"
    }
}
```

## 6. Lua Events

The engine fires events at key moments. Scripts observe but do not drive input.

```lua
-- Fires when selection changes. units = array of Unit handles.
OnSelect(function(player, units)
    -- e.g. update custom UI
end)

-- Fires before a command is executed. Return false to cancel.
OnOrder(function(player, units, order_type, target)
    -- e.g. prevent attacking allied units in a custom game mode
    return true  -- allow
end)

-- Fires when a unit is right-clicked (smart order context)
OnSmartOrder(function(player, selected_units, target_unit, target_pos)
    -- e.g. custom right-click behavior (repair, harvest, enter vehicle)
    return false -- false = let engine handle default behavior
end)
```

## 7. Interaction with Server (Phase 13)

In single player (Phase 12), commands execute immediately:

```
InputPreset → GameCommand → CommandSystem → issue_order()
```

In multiplayer (Phase 13), the flow becomes:

```
InputPreset → GameCommand → NetworkClient.send(cmd) → Server → issue_order()
```

The `CommandSystem` interface stays the same. In multiplayer mode, `submit()`
sends over the network instead of executing locally. The input preset and
selection state are completely unaware of networking.

## 8. Implementation Plan (Phase 12)

**12a — Command System & Selection**
- `GameCommand` struct
- `CommandSystem` class (validate, submit, fire Lua events)
- `SelectionState` class (selection, control groups)
- Picking helpers (screen_to_world, pick_unit, pick_units_in_box)
- Wire into engine main loop

**12b — RTS Input Preset**
- Left click select, box drag select
- Right click smart orders (move, attack, follow)
- Ability hotkeys
- Control groups
- Camera edge pan
- Shift-queue

**12c — Map Input Configuration**
- `input.json` loading (keybinds, ability bar)
- `input_preset` field in manifest
- Lua event hooks (OnSelect, OnOrder, OnSmartOrder)

**12d — Action/RPG Preset (deferred)**
- Virtual joystick widget
- Ability button UI
- Touch input handling
