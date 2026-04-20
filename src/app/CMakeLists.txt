# Shared engine libraries used by all game targets
set(ULDUM_ENGINE_LIBS
    uldum_core
    uldum_platform
    uldum_rhi
    uldum_asset
    uldum_render
    uldum_audio
    uldum_script
    uldum_simulation
    uldum_network
    uldum_input
    uldum_map
)

if(ANDROID)

    # ── Android: single shared library loaded by GameActivity ────────────────
    # Android apps don't ship executables — GameActivity loads libuldum_game.so
    # and calls android_main() inside it. No uldum_dev/uldum_editor/uldum_server
    # on Android; one build artifact, one entry point.
    #
    # The full engine port wiring (GameActivity callbacks → AndroidPlatform,
    # AAssetManager → AssetManager, shader pre-compile integration) is still
    # ahead. For now this target establishes the CMake structure so the Android
    # CMake wrapper (platforms/android/app/src/main/cpp/CMakeLists.txt) can
    # switch from `add_library(uldum_game SHARED stub_main.cpp)` to
    # `add_subdirectory(<repo_root>)` and pick up this target.

    add_library(uldum_game SHARED
        android_main.cpp
        app.cpp
    )
    target_include_directories(uldum_game PRIVATE ${ULDUM_SRC})
    target_link_libraries(uldum_game PRIVATE
        ${ULDUM_ENGINE_LIBS}
        -Wl,--whole-archive
        game-activity::game-activity_static
        -Wl,--no-whole-archive
        log
        android
    )

else()

    # ── Desktop: four executables + pack_assets ──────────────────────────────

    # Staging dir: source engine/ + compiled shaders are assembled here,
    # then packed into bin/engine.uldpak.
    set(ULDUM_ENGINE_STAGING ${CMAKE_BINARY_DIR}/staging/engine)

    # ── uldum_dev — Developer testing runtime (current default) ───────────────

    add_executable(uldum_dev
        dev_main.cpp
        app.cpp
    )
    target_include_directories(uldum_dev PRIVATE ${ULDUM_SRC})
    target_link_libraries(uldum_dev PRIVATE ${ULDUM_ENGINE_LIBS})

    compile_shaders(uldum_dev
        ${ULDUM_ROOT}/engine/shaders
        ${ULDUM_ENGINE_STAGING}/shaders
    )

    # Pack engine + maps into .uldpak / .uldmap archives.
    # Runs every build so that edits to source assets are reflected on disk.
    add_custom_target(pack_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${ULDUM_ROOT}/engine
            ${ULDUM_ENGINE_STAGING}
        COMMAND $<TARGET_FILE:uldum_pack> pack
            ${ULDUM_ENGINE_STAGING}
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine.uldpak
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps
        COMMAND $<TARGET_FILE:uldum_pack> pack
            ${ULDUM_ROOT}/maps/test_map.uldmap
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps/test_map.uldmap
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ULDUM_ROOT}/game.json
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/game.json
        COMMENT "Packing engine and map assets"
        VERBATIM
    )
    add_dependencies(pack_assets uldum_pack uldum_dev_shaders)
    add_dependencies(uldum_dev pack_assets)

    # ── uldum_game — Shipped product (reads game.json, no debug) ─────────────

    add_executable(uldum_game
        game_main.cpp
        app.cpp
    )
    target_include_directories(uldum_game PRIVATE ${ULDUM_SRC})
    target_link_libraries(uldum_game PRIVATE ${ULDUM_ENGINE_LIBS})
    compile_shaders(uldum_game
        ${ULDUM_ROOT}/engine/shaders
        ${ULDUM_ENGINE_STAGING}/shaders
    )
    add_dependencies(uldum_game pack_assets)

    # ── uldum_server — Headless dedicated server (no renderer/audio/window) ──

    set(ULDUM_SERVER_LIBS
        uldum_core
        uldum_asset
        uldum_simulation
        uldum_script
        uldum_network
        uldum_map
    )

    add_executable(uldum_server
        server_main.cpp
    )
    target_include_directories(uldum_server PRIVATE ${ULDUM_SRC})
    target_link_libraries(uldum_server PRIVATE ${ULDUM_SERVER_LIBS})
    add_dependencies(uldum_server pack_assets)

endif()
