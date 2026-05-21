# ── uldum_input — key-name → InputState resolution ──────────────────────
# Tiny, render-free utility. Used by `uldum_hud` for hotkey checks and
# by `uldum_input_router` (presets + picking) for action binding lookup.
# Kept separate so consumers that only need binding resolution don't
# inherit the render/simulation chain that presets pull in.
add_library(uldum_input STATIC
    input_bindings.cpp
)
target_include_directories(uldum_input PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_input PUBLIC uldum_core uldum_platform nlohmann_json::nlohmann_json)

# ── uldum_input_router — input → game-action routing ────────────────────
# Picker (screen→world, unit/item pick) + input presets (RTS / action).
# Pulls render::Camera for screen-to-world raycasts and the various
# simulation systems for hit-testing. Consumed by App; never linked by
# `uldum_hud` (which uses callback hooks in `WorldContext` instead).
add_library(uldum_input_router STATIC
    picking.cpp
    rts_preset.cpp
    action_preset.cpp
    input_preset.cpp
)
target_include_directories(uldum_input_router PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_input_router PUBLIC uldum_core uldum_input uldum_simulation uldum_render uldum_map glm::glm)
