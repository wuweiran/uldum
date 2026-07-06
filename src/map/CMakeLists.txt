add_library(uldum_map STATIC
    map.cpp
    map_format.cpp
    placements_bin.cpp
    terrain_data.cpp
)

target_include_directories(uldum_map PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_map PUBLIC uldum_core uldum_asset uldum_simulation)

# Map-format policy (see map_format.cpp). The game build tree is configured
# with ULDUM_GAME_PROJECT_DIR (build_game.ps1 / Android game flavor); there the
# shipping binary accepts only the current format and refuses stale maps loudly.
# The engine tree (dev / editor / worker) omits ULDUM_GAME_BUILD, so those tools
# accept the wider retained-reader set used for testing old maps and migrating them.
if(ULDUM_GAME_PROJECT_DIR)
    target_compile_definitions(uldum_map PRIVATE ULDUM_GAME_BUILD)
endif()
