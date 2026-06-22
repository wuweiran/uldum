add_library(uldum_render STATIC
    renderer.cpp
    camera.cpp
    camera_controller.cpp
    gpu_mesh.cpp
    gpu_texture.cpp
    shadow.cpp
    terrain.cpp
    animation.cpp
    procedural_skeleton.cpp
    particles.cpp
    glow_system.cpp
    effect.cpp
    world_overlays.cpp
    hud/font.cpp
    hud/world.cpp
    hud/hud_renderer.cpp
)

target_include_directories(uldum_render PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_render PUBLIC uldum_core uldum_rhi uldum_asset uldum_simulation uldum_map uldum_hud glm::glm freetype)
# vma is pulled in transitively via uldum_rhi when ULDUM_BACKEND_VULKAN is on.
