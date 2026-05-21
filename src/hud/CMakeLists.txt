# ── uldum_hud — custom retained-mode HUD data layer ──────────────────────
# Pure data side after Phase 23's hud-split: node tree, composite configs
# + runtimes, text-tag pool, focus target, input routing, hud.json
# loader. The Vulkan-bound renderer lives next door in uldum_render
# (src/render/hud/hud_renderer.{cpp,h}); this lib has no rhi / render /
# vma / freetype / msdfgen dependency.
#
# node.cpp moved to src/render/hud/node.cpp because Node::draw consumes
# the renderer; only the headers (POD data) live here.

add_library(uldum_hud STATIC
    hud.cpp
    hud_network.cpp
    hud_loader.cpp
    node.cpp
)
target_include_directories(uldum_hud PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_hud
    PUBLIC
        uldum_core
        uldum_i18n
        uldum_simulation
        nlohmann_json::nlohmann_json
    PRIVATE
        uldum_asset
        uldum_input
        uldum_map
        glm::glm
)
