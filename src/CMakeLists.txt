# Shared libraries (all platforms)
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(asset)
add_subdirectory(i18n)
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
if(ULDUM_BUILD_GRAPHICS)
    add_subdirectory(rhi)
    add_subdirectory(render)
    add_subdirectory(app)
endif()

# Desktop-only. uldum_pack is needed by both engine and game builds (it produces
# .uldpak / .uldmap archives). The editor is engine-dev only; a per-project
# game build doesn't produce it.
if(NOT ANDROID)
    add_subdirectory(tools)

    # Engine data and dev maps are also needed by headless workers.
    if(NOT ULDUM_GAME_PROJECT_DIR OR NOT ULDUM_BUILD_GRAPHICS)
        set(ULDUM_ENGINE_STAGING ${CMAKE_BINARY_DIR}/staging/engine)
        # Glob every .uldmap folder in maps/ so new dev maps land automatically.
        file(GLOB ULDUM_ENGINE_MAP_DIRS LIST_DIRECTORIES true
            RELATIVE ${ULDUM_ROOT}/maps
            ${ULDUM_ROOT}/maps/*.uldmap)
        set(PACK_ENGINE_MAP_COMMANDS "")
        foreach(MAP ${ULDUM_ENGINE_MAP_DIRS})
            list(APPEND PACK_ENGINE_MAP_COMMANDS
                COMMAND $<TARGET_FILE:uldum_pack> pack
                    ${ULDUM_ROOT}/maps/${MAP}
                    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps/${MAP})
        endforeach()

        add_custom_target(pack_engine_maps ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${ULDUM_ROOT}/engine
                ${ULDUM_ENGINE_STAGING}
            COMMAND $<TARGET_FILE:uldum_pack> pack
                ${ULDUM_ENGINE_STAGING}
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine.uldpak
            COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps
            ${PACK_ENGINE_MAP_COMMANDS}
            COMMENT "Packing engine assets + dev maps"
            VERBATIM
        )
        add_dependencies(pack_engine_maps uldum_pack)
        if(TARGET uldum_dev_shaders)
            add_dependencies(pack_engine_maps uldum_dev_shaders)
        endif()
    endif()

    if(ULDUM_BUILD_GRAPHICS AND NOT ULDUM_GAME_PROJECT_DIR)
        add_subdirectory(editor)
    endif()
endif()

# Headless server binaries depend on the shared engine asset packaging above.
if(NOT ANDROID)
    add_subdirectory(server)
endif()
