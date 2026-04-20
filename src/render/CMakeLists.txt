add_library(uldum_render STATIC
    renderer.cpp
    camera.cpp
    gpu_mesh.cpp
    gpu_texture.cpp
    shadow.cpp
    terrain.cpp
    animation.cpp
    procedural_skeleton.cpp
    particles.cpp
    effect.cpp
)

target_include_directories(uldum_render PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_render PUBLIC uldum_core uldum_rhi uldum_asset uldum_simulation uldum_map glm::glm vma)
