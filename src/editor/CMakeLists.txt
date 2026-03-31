add_library(uldum_editor STATIC
    editor.cpp
)

target_include_directories(uldum_editor PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_editor PUBLIC uldum_core uldum_render uldum_simulation uldum_map)
