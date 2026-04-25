# UI System

Uldum has **two UI systems**, split along a load-bearing seam:

- **Shell UI** — everything *around* gameplay. Main menu, game room, settings, loading, results. Screen-space, tens of elements, authored as markup + stylesheet. **RmlUi-powered**, used directly (no engine-side wrapper).
- **HUD** — everything *during* gameplay. Screen-space UI (action bar, minimap, unit panel, dialogs) plus world-anchored overlays (unit healthbars, floating damage text). Authored per-map via `hud.json` and Lua. **Custom, built by us.**

```
App Launch → Shell UI (menu / game-room / loading) → Session (HUD) → Shell UI (results) → Menu
```

## Why split

| Aspect | Shell UI | HUD |
|---|---|---|
| Lifetime | Static-ish; between sessions | Every frame during gameplay |
| Anchoring | Screen-space | Screen-space + world-anchored overlays |
| Authoring | RML + RCSS | `hud.json` (layout + style) + Lua (content) |
| Count | Tens per screen | Hundreds per scene |
| Rendering | RmlUi's tessellated paths | Custom instanced batch |
| Data source | `game.json`, menu input | Live sim state |

Forcing RmlUi to draw per-unit health bars would be slow; forcing custom C++ to author a settings menu would be ugly. The two systems don't share rendering code.

## Dev tools

**ImGui** stays for the editor, debug overlays, and dev consoles. Immediate-mode is right for those. RmlUi and ImGui coexist in the same executable with separate contexts.

## HUD architecture

Retained-mode tree, not immediate-mode. Widgets are created once, persisted, and mutated — Lua addresses them by id, data bindings pull from sim state each frame. Available in both dev and game builds; does not depend on RmlUi.

**Rendering.** A 2D quad batcher feeds the render graph: textured quads for icons/panels, SDF-glyph quads for text. One draw call per batch, per material. World-anchored overlays (unit healthbars, floating damage text) use the same batcher with a world-to-screen projection from the scene camera.

**Font pipeline.** FreeType rasterizer + **MSDF** (multi-channel signed distance field) atlas, populated dynamically at runtime. Glyphs rasterized on demand, LRU-evicted if the atlas fills. MSDF preserves sharp corners (plain SDF rounds them), scales crisply from mobile to hero-title sizes, and gives outlines / glows / shadows for free in the shader. This is the modern standard for game UI text (Unity TextMeshPro, many indies).

**Text stack** layered so each box is replaceable:

```
string → [shaper] → glyph run → [line breaker] → lines → [atlas] → quads → [renderer]
```

- **Shaper** — identity today (codepoint == glyph). HarfBuzz drops in here later for Arabic / Thai / complex CJK.
- **Line breaker** — whitespace (Latin) + kinsoku (CJK forbidden-start/end punctuation). ICU later if needed.
- **Atlas / renderer** — full MSDF from day one.

Basic horizontal CJK works without HarfBuzz; Arabic / Thai / RTL / BiDi are deferred (see [design.md](design.md) §16 Deferred).

**Input.** Platform layer feeds pointer events (position + press/release) to the widget tree: mouse on desktop, touch on mobile. The widget tree doesn't know the difference. Hotkeys, virtual joysticks, and other platform-exclusive affordances live in the preset layer, not the foundation.

**Layout.** Logical dp (density-independent pixels), scaled to physical resolution. Safe-area rect excludes notch / nav bar on mobile, full window on desktop.

**Deps.** `freetype`, `msdfgen` (or `msdf-atlas-gen`). FetchContent-friendly.

## HUD composition model

### Two tiers — composites and atoms

HUD content is built from two categories of node. The split is load-bearing; everything else follows from it.

- **Engine composites** — fixed-behavior widgets the engine owns end-to-end (action bar with cooldown rings, minimap with fog, joystick with drag tracking). The map influences look-and-feel via `style`; behavior is not configurable. Used for things that are universal across games using a given preset.
- **Atom nodes** — small primitives (panel, label, image, bar, button) that map authors freely compose into custom UI. Layout declared in `hud.json`, content mutated from Lua at runtime.

Criterion: *"is this widget's behavior the same across every game that uses this preset?"* → yes → engine composite; no → build with atoms.

> We call them **nodes** (not "widgets") — `widget` is a WC3 term and we're not trying to re-adopt it. `Panel`, `Label`, `Button`, `Image`, `Bar` are all node types; "tree of nodes" is the HUD's data model.

### Style vs content — static vs dynamic

- **Style** (colors, icons, sizes, fonts, slot positions) is **static**. Declared in `hud.json`, frozen at map load. Lua cannot change style at runtime — not for composites, not for custom nodes.
- **Content** (text, bar fill fraction, bound ability id, actor reference, visibility) is **dynamic**. Lua mutates it freely via node handles.

This lets the engine bake style into GPU resources up-front (descriptor sets for icon textures, pre-sized atlases) and keeps the hot per-frame path to content updates only.

### `hud.json` shape

One `hud.json` per map, alongside `manifest.json` in the map folder. Top-level keys:

```json
{
  "preset":         "rts",
  "composites":     { ... },
  "world_overlays": { ... }
}
```

- **`preset`** (required) — `"rts"` or `"action_rpg"`. Picks the input bindings and default composite layout.
- **`composites`** — configuration for engine composites (position, slot layouts, inline style). Composites are singletons per map.
- **`world_overlays`** — configuration for screen-pixel-sized elements anchored to world positions / entities (unit health bars, name label style, etc.). See the World UI section below.

**All custom UI — panels, labels, buttons, custom bars — is Lua-authored, not JSON-declared.** The map's Lua script creates nodes at load time via `CreateNode{...}`, attaches them to the HUD root, and mutates content at runtime. Styles are set at creation and frozen; only content is Lua-mutable thereafter.

> **Transitional note (pre-16c-v):** until the Lua HUD API ships, `hud.json` accepts a `"nodes"` block that declares a static node tree. `test_map.uldmap/hud.json` uses this as a temporary bridge so we can smoke-test atoms + the parser without Lua. The block goes away when 16c-v lands; the smoke panel moves to a Lua setup script.

### Atom nodes

Five atoms. Every custom widget a map authors is a composition of these.

| Atom | Style | Content |
|---|---|---|
| `panel` | bg color, border, corner radius | visibility, children |
| `label` | font size, color, alignment | text |
| `image` | source (asset path), tint | visibility |
| `bar` | bg color, fill color, orientation | fill fraction [0..1] |
| `button` | bg / hover / press colors, border | enabled state, click callback |

Anchoring uses a 9-point enum for v1 (`tl tc tr ml mc mr bl bc br`) plus `(x, y, w, h)` offset in logical pixels. Extensible to percent-based / flex later without breaking existing maps.

### Engine composites

Four composites, all singletons. All have a Lua content API; none accept style changes at runtime.

| Composite | Role | Content API |
|---|---|---|
| `action_bar` | ability-button slot group. Each slot shows icon / cooldown ring / hotkey badge / disabled overlay for its bound ability. Click / hotkey issues the ability. | `ActionBarSetActor`, `ActionBarSetSlot`, ... |
| `command_bar` | engine-command slot group (Attack, Move, Stop, Hold, Patrol). Click / hotkey issues the order. | no Lua binding — purely map-authored |
| `minimap` | Scaled terrain + fog + entity dots + click-to-pan. | `MinimapSetVisible` |
| `joystick` | Virtual analog stick (mobile only; desktop skips it at load). | reads output vector; no Lua binding needed |

Deferred composites (may ship later; v1 doesn't include them):

- **Unit info panels** (selection portrait for RTS, hero frame / target frame for action). Portrait + name + HP / mana bars for a bound entity. Deferred because every game has idiosyncratic layouts here (portrait style, stat rows, attribute icons, XP bar, class colors); we'd rather see real use cases before picking a shape. Maps that need one before we ship it can compose `image` + `label` + `bar` atoms.
- **Status strip** (buff / debuff icons with timer ring + stack count). Conceptually part of unit-info display — ships together with unit_panel when we tackle that category.
- **Resource bars** (gold / lumber / custom resources). Resource types vary per map, no universal layout — maps compose from atoms.
- **Dialog** (modal popup for game-content prompts: tutorial callouts, in-world dialogue, mission briefings). A tutorial prompt is mostly a panel + label + button(s) — maps can build one from atoms. The only piece atoms don't cover is modal input capture (blocking world + HUD input while the dialog is up); that primitive ships when we decide we need it. System menus (pause / quit / settings) are not HUD — see below.

**System menus are not HUD.** Pause / Settings / Quit / disconnect dialogs are **Shell UI** concerns, not HUD composites. Shell already owns those screens for the main menu flow; the same documents are reused when the player presses Escape mid-game. App binds Escape → load `pause.rml` into the active Shell context, pause sim ticking, and release on close. Dev builds do the same via DevConsole's pause / disconnect overlays. HUD never invokes Shell directly; routing is App's job.

Slots inside `action_bar` (and any future slot-bearing composite) are declared as an array of full slot specs in `hud.json` — each with its own `(x, y, w, h)` and `style`. The composite gives no opinion about grid-vs-row layout; map authors arrange slots however they want. `ability_button` is **not** a standalone node type — slot entries only exist inside composites.

Behaviors the engine does **not** own (maps implement in Lua if they want them):

- Selection-following: when selection changes, re-bind `action_bar` slots to the new unit's abilities. → `on_select` event + `SetAbilitySlot`.
- Subgroup paging: cycle through subgroups of a mixed selection. → Lua tracks the active subgroup and re-binds.
- Multi-bar hotkey pages (WoW-style): one `action_bar` per map; maps wanting more use atoms + Lua.

**Observer events on composites are deferred.** Atoms (`button`) emit events for Lua to subscribe via the trigger system. Composite observer events (e.g., "a slot was clicked", "minimap was pinged") aren't needed for v1 — hardcoded behavior covers the common case. We can add observer events per-composite when a real use case appears; the trigger system is already the right shape.

### Composite JSON + Lua API

Composite config lives under `hud.json`'s `"composites"` block. Every composite has: an anchor + `(x, y, w, h)` for the composite itself, an optional inline `style` block, and composite-specific content (slots, etc.). All layout + style is frozen after load.

**Style is open and extensible.** The engine ships a default prototype for each composite (and each sub-element like an action_bar slot). Prototypes accept a known set of style fields; unknown fields are ignored. Maps override specific fields without having to re-declare the whole style. Later prototypes can add new fields or behaviors without breaking existing hud.json files.

**Paint values** — anywhere a style field accepts a color (backgrounds, fills, borders, ring tints, etc.), it also accepts an image. Three forms:

```json
"bg": "#80808080"                         // plain color, leading '#'
"bg": "ui/panel_bg.ktx2"                  // plain image, asset path
"bg": {                                   // compound form
  "image":      "ui/panel_bg.ktx2",
  "tint":       "#FFFFFFB0",              // optional tint multiplier
  "nine_slice": { "top": 8, "bottom": 8, "left": 8, "right": 8 }  // optional
}
```

This applies to every visual style field across every atom and composite. A few fields are image-only (e.g., `image.source` on the atom, icons in badges) and stay as plain paths — they don't accept colors.

```json
"composites": {
  "action_bar": {
    "anchor": "bc", "x": -192, "y": -64, "w": 384, "h": 48,
    "style": {
      "prototype":           "default",       // optional — selects engine prototype
      "cooldown_ring_color": "#000000B0",
      "disabled_tint":       "#80808080",
      "hotkey_badge": {
        "font_size": 11, "bg": "#000000C0", "color": "#FFFFFF", "corner": "tr"
      }
    },
    "slots": [
      {
        "x": 0, "y": 0, "w": 48, "h": 48,
        "hotkey": "Q",
        "style": {
          "prototype":    "default",          // different slots can use different prototypes
          "bg":           "#30303880",
          "hover_bg":     "#50505890",
          "press_bg":     "#6060708F",
          "border_color": "#00000080",
          "border_width": 1
        }
      }
    ]
  }
}
```

### Lua API conventions

HUD Lua follows the same flat WC3-style naming as the rest of the engine's scripting API. No OOP-style `Object:Method()` — every function is a global with a verb-noun-ish name, first argument is the subject when relevant.

- Node handles are values returned by `GetNode` / `CreateNode` / composite accessors; pass them as arguments.
- Composites are singletons; their APIs are named-prefixed (`ActionBarSetSlot`, `MinimapSetVisible`).
- Atoms' content setters take a node handle (`SetLabelText(node, text)`).

```lua
-- Node lookup
local node = GetNode("objective_label")       -- global lookup by id
local node = GetTriggerNode()                  -- the node that fired this trigger

-- Atom content
SetNodeVisible(node, true)
SetLabelText(node, "Destroy the Citadel")
SetImageSource(node, "icons/skull.ktx2")
SetBarFill(node, 0.73)
SetButtonEnabled(node, false)

-- Custom nodes: create / destroy
local popup = CreateNode{
    type = "panel",
    style = { bg = "#202028E0" },
    anchor = "mc", w = 320, h = 120,
    children = {
        { type = "label", id = "popup_text", text = "Tutorial: press Q to attack" }
    }
}
DestroyNode(popup)

-- Button events via trigger system
local trig = CreateTrigger()
TriggerRegisterNodeEvent(trig, GetNode("ready_button"), EVENT_BUTTON_PRESSED)
TriggerAddAction(trig, function() StartGame() end)

-- action_bar composite
ActionBarSetActor(player_hero)
ActionBarSetSlot(0, "fireball")
ActionBarClearSlot(0)
ActionBarSwapSlots(0, 2)
ActionBarSetSlotVisible(3, false)
ActionBarSetVisible(true)

-- minimap composite
MinimapSetVisible(true)

```

Hardcoded action_bar behavior (no Lua wiring required):

- Each slot shows its bound ability's icon, cooldown radial, hotkey badge, disabled overlay (out-of-range, no mana, no target, etc.).
- Click / hotkey press → issues the ability on the bound actor through CommandSystem.
- Empty slot binding → slot renders empty.
- No bound actor → entire bar renders at `disabled_tint`.

### `joystick`

Virtual analog stick for action-preset movement on mobile. Inert on desktop — the composite declaration is ignored if the build is desktop (WASD handles movement there). The action input preset reads the stick's output vector each frame and feeds it into the hero's movement; no Lua wiring needed.

```json
"composites": {
  "joystick": {
    "anchor": "bl", "x": 80, "y": -180, "w": 160, "h": 160,

    "style": {
      "prototype": "default",
      "mode":      "fixed",

      "base": {
        "fill":         "ui/joystick_base.ktx2",
        "radius":       80,
        "border_color": "#FFFFFF60",
        "border_width": 2
      },
      "knob": {
        "fill":   { "image": "ui/joystick_knob.ktx2", "tint": "#FFFFFFC0" },
        "radius": 32
      },

      "dead_zone":  0.15,
      "max_radius": 80
    }
  }
}
```

Two `style.mode` options:

- `"fixed"` — base is drawn at the anchor position; touch inside the base radius starts tracking. Muscle-memory friendly.
- `"floating"` — base is invisible until touch-down inside the composite's `(x, y, w, h)` region, at which point it spawns at the touch point and tracks from there. Popular in mobile action games where the left-thumb resting spot varies.

Hardcoded behavior:

- Touch-down in the activation region → knob follows finger, clamped to `max_radius` from base center.
- Output vector is `(knob_offset / max_radius)` with magnitude in `[0, 1]`; magnitudes below `dead_zone` report as zero.
- Release → knob returns to center, output zeros.

Lua API: `JoystickSetVisible(bool)`. No content API for reading the vector — the input preset handles that internally.

Deferred:

- 8-way / 4-way snap modes — add as a `style.mode` value when needed.
- A second right-hand stick for camera look — mobile action preset uses swipe-anywhere for camera.

### `minimap`

```json
"composites": {
  "minimap": {
    "anchor": "bl", "x": 10, "y": -210, "w": 200, "h": 200,
    "style": {
      "prototype":    "default",
      "bg":           "#00000080",
      "border_color": "#FFFFFF40",
      "border_width": 2,
      "fog_tint":     "#000000C0",
      "dot": {
        "self":      { "color": "#40FF40", "size": 3 },
        "ally":      { "color": "#4040FF", "size": 3 },
        "enemy":     { "color": "#FF4040", "size": 3 },
        "neutral":   { "color": "#FFD040", "size": 2 },
        "structure": { "size": 4 }
      }
    }
  }
}
```

Hardcoded behavior:

- Renders scaled terrain + local-player fog + per-unit dots, rebuilt each frame.
- Left-click (or tap on mobile) → pans camera to that world point.
- Dot color picked by alliance relationship (self / ally / enemy / neutral). `structure` size overrides for buildings regardless of alliance; color still follows relationship.

Lua API: `MinimapSetVisible(bool)`. No per-entity marker API, no programmatic ping — additions land when a real use case appears.

## Subphase design

Phase 16's subphase breakdown lives in [design.md](design.md). Concrete decisions (file layout, API shapes, primitive sets) get worked out before each subphase — not here.
