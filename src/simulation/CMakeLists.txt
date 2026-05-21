add_library(uldum_simulation STATIC
    simulation.cpp
    world.cpp
    type_registry.cpp
    ability_def.cpp
    systems.cpp
    pathfinding.cpp
    spatial_query.cpp
    vision.cpp
    command_system.cpp
    selection.cpp
)

target_include_directories(uldum_simulation PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_simulation PUBLIC uldum_core uldum_asset uldum_map glm::glm)
