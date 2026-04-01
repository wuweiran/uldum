add_library(uldum_render STATIC
    renderer.cpp
    camera.cpp
    gpu_mesh.cpp
    terrain.cpp
)

target_include_directories(uldum_render PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_render PUBLIC uldum_core uldum_rhi uldum_asset uldum_simulation uldum_map glm::glm vma)
