# Editor

`uldum_editor` is the authoring tool for `.uldmap` packages. Today it edits terrain; over time it will grow to cover placements, types, scripts, and map metadata.

## Modes

The editor has two modes, auto-selected by what the user opens:

| Mode | Opened artifact | Save behavior |
|---|---|---|
| **Normal**        | packed `.uldmap` file  | rewrites the `.uldmap` archive |
| **Source-folder** | directory (`<name>.uldmap/`) | writes loose files to the directory |

There is no UI toggle — the mode is a consequence of the path the user picks. A file → normal. A directory → source-folder.

Both modes consume the same asset formats — KTX2 for textures, glTF for models, JSON for configs, Lua for scripts, custom binary for `terrain.bin`. The modes only differ in how changes are persisted (rewritten archive vs. loose files on disk).

Every other target (`uldum_dev`, `uldum_game`, `uldum_server`) is strict: packed maps only. Source-folder mode is the editor's sole exception, motivated purely by the save-to-loose-files workflow — not by format flexibility.

## What each mode edits

Most "editing" is metadata:

- Terrain sculpt (heightmap, cliffs, ramps) — `terrain.bin`
- Splatmap (tile layers, water, pathing) — part of `terrain.bin`
- Object placements, regions, cameras — JSON
- Types (units, abilities) — JSON
- Scripts — Lua text
- Manifest — JSON

Texture pixels are **not editable** from within the editor in either mode. Authors use their own image tools (Photoshop, GIMP, Substance, etc.), then convert to KTX2 with `toktx` before dropping the file into the map folder. The editor will display whatever KTX2 files are present but never modifies them.

## Authoring lifecycle

```
┌─────────────────┐     Export Map    ┌──────────────────┐
│ Source folder   │ ────────────────▶ │ Packed .uldmap    │
│ (KTX2, JSON,    │                   │ (KTX2, JSON,      │
│  Lua, terrain)  │ ◀─ (no reverse)   │  Lua, terrain)    │
└─────────────────┘                   └──────────────────┘
     ▲                                     ▲
     │ edit in source-folder mode          │ edit in normal mode
     │ (full authoring)                    │ (metadata + scripts)
```

- **Create:** start in source-folder mode. Author loose KTX2/JSON/Lua/terrain files in a directory.
- **Ship:** **Export Map** — packs the source folder into a `.uldmap` archive.
- **Iterate after ship:** open the packed `.uldmap` in normal mode to tweak metadata (balance, script logic, terrain sculpt, splatmap). Re-save updates the archive in place.
- **Re-texture:** convert the new PNG to KTX2 externally (see below), drop it into the source folder, re-Export Map.

No "reverse bake" is supported — `.uldmap` → source folder is not a round-trip. Treat the packed archive as a build artifact; keep the source folder under version control as the true source of truth.

## PNG → KTX2 conversion (author-side)

Map makers convert PNG textures to KTX2 outside the engine using [KTX-Software](https://github.com/KhronosGroup/KTX-Software)'s `toktx` tool, before placing files into a map folder.

A helper script `scripts/png_to_ktx2.bat` wraps the canonical flags:

```cmd
:: albedo / diffuse / sRGB textures
scripts\png_to_ktx2.bat tex.png                  :: → tex.ktx2, UASTC, sRGB, mipmapped

:: normal maps / data textures (linear)
scripts\png_to_ktx2.bat --linear normal.png      :: → normal.ktx2, UASTC, linear, mipmapped
```

Drop the resulting `.ktx2` into the map's asset folder. The engine never converts, bakes, or falls back — it only reads KTX2.

## Out of scope for the current phase

- **Import Asset:** not useful while terrain is the only editable surface. Reconsider after Phase 16 adds placements / types / scripts editing in the UI.
- **Reverse bake** (unpack a `.uldmap` back to a source folder): not supported. Ship your source folder alongside the `.uldmap` if you want others to remix it.
- **Live mode switching** within one session: opening a second map closes the first; each open-action re-selects the mode.
