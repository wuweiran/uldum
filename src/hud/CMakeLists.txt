# ── uldum_hud — custom retained-mode HUD UI ──────────────────────────────
# Available in all flavors (dev + game, desktop + Android). Renders
# screen-space nodes and world-anchored overlays (healthbars, floating
# text). Separate from uldum_shell (RmlUi-based menus).

add_library(uldum_hud STATIC
    hud.cpp
    node.cpp
    font.cpp
    hud_loader.cpp
    world.cpp
)
target_include_directories(uldum_hud PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_hud
    PUBLIC
        uldum_core
        nlohmann_json::nlohmann_json
    PRIVATE
        uldum_rhi
        uldum_asset
        uldum_simulation
        uldum_render
        uldum_input
        uldum_map
        glm::glm
        vma
        freetype
        msdfgen::msdfgen
        msdfgen::msdfgen-ext
)
