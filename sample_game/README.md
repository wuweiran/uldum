# Sample Game

A first-party product built on the Uldum engine. Two jobs:

1. **Validate the game pipeline.** Every change to the game build / packaging flow is tested against this folder.
2. **Copy-me-to-start reference.** To create a new game, copy this folder out somewhere and edit it.

## Layout

```
sample_game/
  game.json                 product config — name, window, maps, Android metadata
  branding/
    icon.png                1024×1024, becomes Windows .ico (embedded in exe) and Android launcher icons
  maps/
    simple_map.uldmap/      the map this product ships
  keystore.properties       Android release signing (gitignored per project)
  keystore.properties.example
```

Per-platform settings (exe name, Android `applicationId` / `app_name`) live as fields in `game.json`.

Future addition: `lobby/` — main menu / map select Lua scripts. Lands with Phase 16 (UI system). Until then, the game auto-loads `default_map` on launch.

## Not a playground

Maps here only contain what would ship with this product. Engine feature tests go in the engine repo's `../maps/` folder (consumed by `uldum_dev`).
