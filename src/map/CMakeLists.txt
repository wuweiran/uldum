add_library(uldum_map STATIC
    map.cpp
    placements_bin.cpp
    terrain_data.cpp
)

target_include_directories(uldum_map PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_map PUBLIC uldum_core uldum_asset uldum_simulation)
