# Audio System Design

## Overview

The audio system provides 3D positional sound, background music streaming, and ambient loops. Maps define all sound content — the engine provides the playback system. **OGG Vorbis is the recommended format** (it's what the sample content ships and what "one format per asset type" prescribes), but the miniaudio decoder is compiled with all its built-in codecs enabled, so **WAV, MP3, and FLAC also load and play** — see [Sound Assets](#sound-assets).

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

- **Format**: **OGG Vorbis (.ogg) recommended.** The engine also decodes **WAV, MP3, and FLAC** — miniaudio is built with no `MA_NO_*` decoder flags, so all its built-in codecs (dr_wav, dr_mp3, dr_flac, stb_vorbis) are active. Format is detected from the file's contents, not its extension: `resolve_path` passes the author's path through untouched and hands the bytes to miniaudio, which sniffs the codec. So a `.wav`/`.mp3`/`.flac` referenced from JSON or Lua plays with no code change.
  - **Why OGG is still recommended**: good compression at high quality (WAV is uncompressed → large; FLAC is lossless → large), consistent with the "one format per asset type" convention, and matches the sample content. Reach for the others only when you already have assets in that format or need lossless.
  - **No transcode step**: unlike textures (PNG → KTX2), audio is read as-is at runtime — there is no author-side conversion tool. Whatever format you drop in is what ships.
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
