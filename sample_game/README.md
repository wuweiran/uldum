# Sample Game

A first-party product built on the Uldum engine. Two jobs:

1. **Validate the game pipeline.** Every change to the game build / packaging flow is tested against this folder.
2. **Copy-me-to-start reference.** To create a new game, copy this folder out somewhere and edit it.

## Layout

```
sample_game/
  game.json                 product config — name, window, maps, Android metadata
  branding/
    icon.ico                Windows executable icon
    android/                Android launcher icons
  maps/
    simple_map.uldmap/      the map this product ships
  shell/                    Shell UI markup, styles, and fonts
  src/                      App-specific Engine + App integration
  game.cmake                App target sources and include paths
  keystore.properties       Android release signing (gitignored per project)
  keystore.properties.example
```

Per-platform settings (exe name, Android `applicationId` / `app_name`) live as fields in `game.json`. `sample_game_app.cpp` owns the menu/lobby/loading/results flow and starts `simple_map` from the Play action.

## Not a playground

Maps here only contain what would ship with this product. Engine feature tests go in the engine repo's `../maps/` folder (consumed by `uldum_dev`).
