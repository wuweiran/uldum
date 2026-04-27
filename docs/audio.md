# Audio System Design

## Overview

The audio system provides 3D positional sound, background music streaming, and ambient loops. Maps define all sound content — the engine provides the playback system. Sound files use OGG Vorbis format, decoded by miniaudio.

## Sound Categories

| Category    | Behavior                        | Positioning | Examples                                    |
|-------------|---------------------------------|-------------|---------------------------------------------|
| **SFX**     | Short one-shot, fire-and-forget | 3D or 2D    | Sword hit, spell cast, footstep, UI click   |
| **Music**   | Streamed, one track at a time   | 2D          | Map theme, combat music, victory fanfare    |
| **Ambient** | Looping environmental           | 3D or 2D    | Wind, river, campfire crackle, cave echo    |
| **Voice**   | Unit response, queued           | 2D          | "Yes my lord", "Ready", "What is it?"       |

## Volume Channels

Five independent volume channels, each 0.0 to 1.0:

- **Master** — scales all output
- **SFX** — combat and ability sounds
- **Music** — background tracks
- **Ambient** — environmental loops
- **Voice** — unit responses

## Listener

The listener position follows the camera. Updated each frame. All 3D sounds attenuate based on distance from the listener.

## Lua API

### SFX

```lua
-- Play at a unit's position (follows the unit)
PlaySound("sounds/sword_hit.ogg", unit)

-- Play at a world position
PlaySoundAtPoint("sounds/explosion.ogg", x, y)

-- Play as 2D (UI sounds, not positioned)
PlaySound2D("sounds/ui_click.ogg")
```

### Music

```lua
-- Play with crossfade (seconds)
PlayMusic("music/theme.ogg", 2.0)

-- Stop with fade out (seconds)
StopMusic(1.0)
```

### Ambient

```lua
-- Start a looping ambient sound at a position
local handle = PlayAmbientLoop("sounds/river.ogg", x, y)

-- Stop with fade out
StopAmbientLoop(handle, 0.5)
```

### Volume

```lua
SetVolume("master", 0.8)
SetVolume("sfx", 1.0)
SetVolume("music", 0.5)
SetVolume("ambient", 0.7)
SetVolume("voice", 1.0)
```

## Sound Assets

- **Format**: OGG Vorbis (.ogg)
- **SFX / Ambient / Voice**: short files, loaded into memory
- **Music**: streamed from disk
- **Path resolution**: passed verbatim to the AssetManager. Mount prefixes (active map's pak, then `engine.uldpak`) decide which package answers.

## Map Integration

The engine provides the playback system. All audio content is map-defined.

- **Unit animation sounds** — defined per unit type in JSON (e.g., attack sound, death sound, footstep). The engine triggers them automatically from animation events — no Lua needed.
- **Ability sounds** — defined per ability in JSON. Triggered by the ability system on cast/impact.
- **Ambient sounds** — placed by map scripts at world positions via Lua.
- **Music** — started in map's `main.lua` via Lua API.
- **UI / custom sounds** — triggered by Lua scripts directly.
