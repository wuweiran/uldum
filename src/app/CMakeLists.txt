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
    COMMENT "Syncing engine assets and maps to output directory"
)
add_dependencies(uldum_dev copy_assets)

# uldum_game (Phase 13) and uldum_server (Phase 12) entry points exist in
# game_main.cpp and server_main.cpp but are not built yet. They will be
# added as targets when their respective phases are implemented.
