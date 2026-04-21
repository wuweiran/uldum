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

## Subphase design

Phase 16's subphase breakdown lives in [design.md](design.md). Concrete decisions (file layout, API shapes, primitive sets) get worked out before each subphase — not here.
