add_executable(uldum_editor
    main.cpp
    editor.cpp
)

target_include_directories(uldum_editor PRIVATE ${ULDUM_SRC})
target_compile_definitions(uldum_editor PRIVATE ULDUM_SOURCE_DIR="${ULDUM_ROOT}")
target_link_libraries(uldum_editor PRIVATE
    uldum_core
    uldum_platform
    uldum_rhi
    uldum_asset
    uldum_render
    uldum_simulation
    uldum_map
    imgui
    ole32
)

# Reuse shader + asset targets from app/ (engine-build path, where these exist).
add_dependencies(uldum_editor uldum_dev_shaders pack_engine_maps)
