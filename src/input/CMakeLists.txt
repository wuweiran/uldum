add_library(uldum_input STATIC
    command_system.cpp
    selection.cpp
    picking.cpp
    rts_preset.cpp
)

target_include_directories(uldum_input PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_input PUBLIC uldum_core uldum_simulation uldum_render uldum_map glm::glm)
