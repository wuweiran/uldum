# Shared libraries (all platforms)
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(rhi)
add_subdirectory(asset)
add_subdirectory(render)
add_subdirectory(audio)
add_subdirectory(script)
add_subdirectory(simulation)
add_subdirectory(network)
add_subdirectory(input)
add_subdirectory(map)

# Shell UI framework — game-only. See ULDUM_SHELL_UI in root CMakeLists.
if(ULDUM_SHELL_UI)
    add_subdirectory(shell)
endif()

# HUD — custom retained-mode UI for in-game overlays (healthbars, command
# card, minimap, floating damage text). Available in all flavors (dev + game,
# desktop + Android). Separate from uldum_shell / RmlUi.
add_subdirectory(hud)

# Executables / shared-library entry point
add_subdirectory(app)

# Desktop-only. uldum_pack is needed by both engine and game builds (it produces
# .uldpak / .uldmap archives). The editor is engine-dev only; a per-project
# game build doesn't produce it.
if(NOT ANDROID)
    add_subdirectory(tools)
    if(NOT ULDUM_GAME_PROJECT_DIR)
        add_subdirectory(editor)
    endif()
endif()
