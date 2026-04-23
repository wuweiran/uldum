# UI System

Uldum has **two UI systems**, split along a load-bearing seam:

- **Shell UI** — everything *around* gameplay. Main menu, game room, settings, loading, results. Screen-space, tens of elements, authored as markup + stylesheet. **RmlUi-powered**, used directly (no engine-side wrapper).
- **HUD** — everything *during* gameplay. Health bars above units, selection circles, toolbar, minimap, damage numbers. Often world-space, hundreds of instances per scene, authored from Lua. **Custom, built by us.**

```
App Launch → Shell UI (menu / game-room / loading) → Session (HUD) → Shell UI (results) → Menu
```

## Why split

| Aspect | Shell UI | HUD |
|---|---|---|
| Lifetime | Static-ish; between sessions | Every frame during gameplay |
| Anchoring | Screen-space | Often world-space (above units) |
| Authoring | RML + RCSS | Lua |
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

## Subphase design

Phase 16's subphase breakdown lives in [design.md](design.md). Concrete decisions (file layout, API shapes, primitive sets) get worked out before each subphase — not here.
