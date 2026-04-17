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

# ── uldum_dev — Developer testing runtime (current default) ───────────────

add_executable(uldum_dev
    dev_main.cpp
    app.cpp
)
target_include_directories(uldum_dev PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_dev PRIVATE ${ULDUM_ENGINE_LIBS})

compile_shaders(uldum_dev
    ${CMAKE_SOURCE_DIR}/engine/shaders
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine/shaders
)

# Always copy engine assets and maps to output directory.
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/engine
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/maps
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/game.json
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/game.json
    COMMENT "Syncing engine assets and maps to output directory"
)
add_dependencies(uldum_dev copy_assets)

# ── uldum_game — Shipped product (reads game.json, no debug) ─────────────

add_executable(uldum_game
    game_main.cpp
    app.cpp
)
target_include_directories(uldum_game PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_game PRIVATE ${ULDUM_ENGINE_LIBS})
compile_shaders(uldum_game
    ${CMAKE_SOURCE_DIR}/engine/shaders
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine/shaders
)
add_dependencies(uldum_game copy_assets)

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
add_dependencies(uldum_server copy_assets)
