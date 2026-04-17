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

# Staging dir: source engine/ + compiled shaders are assembled here,
# then packed into bin/engine.uldpak.
set(ULDUM_ENGINE_STAGING ${CMAKE_BINARY_DIR}/staging/engine)

# ── uldum_dev — Developer testing runtime (current default) ───────────────

add_executable(uldum_dev
    dev_main.cpp
    app.cpp
)
target_include_directories(uldum_dev PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_dev PRIVATE ${ULDUM_ENGINE_LIBS})

compile_shaders(uldum_dev
    ${CMAKE_SOURCE_DIR}/engine/shaders
    ${ULDUM_ENGINE_STAGING}/shaders
)

# Pack engine + maps into .uldpak / .uldmap archives.
# Runs every build so that edits to source assets are reflected on disk.
add_custom_target(pack_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/engine
        ${ULDUM_ENGINE_STAGING}
    COMMAND $<TARGET_FILE:uldum_pack> pack
        ${ULDUM_ENGINE_STAGING}
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine.uldpak
    COMMAND ${CMAKE_COMMAND} -E make_directory
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps
    COMMAND $<TARGET_FILE:uldum_pack> pack
        ${CMAKE_SOURCE_DIR}/maps/test_map.uldmap
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps/test_map.uldmap
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/game.json
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
target_include_directories(uldum_game PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_game PRIVATE ${ULDUM_ENGINE_LIBS})
compile_shaders(uldum_game
    ${CMAKE_SOURCE_DIR}/engine/shaders
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
target_include_directories(uldum_server PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_server PRIVATE ${ULDUM_SERVER_LIBS})
add_dependencies(uldum_server pack_assets)
