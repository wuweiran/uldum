# ── uldum_shell — Shell UI framework (RmlUi bridge) ─────────────────────
# Only compiled into game builds. See ULDUM_SHELL_UI in the root CMakeLists.
#
# Provides:
#   - `shell::Shell` — engine-facing API for loading RML documents and driving
#     RmlUi per-frame (update, render, input).
#   - Bridge implementations for RmlUi's three extension points:
#       - `Rml::RenderInterface` → Vulkan RHI (see render_interface.cpp)
#       - `Rml::SystemInterface`  → core::log, std::chrono (system_interface.cpp)
#       - `Rml::FileInterface`    → asset::AssetManager (file_interface.cpp)

add_library(uldum_shell STATIC
    shell.cpp
    render_interface.cpp
    system_interface.cpp
    file_interface.cpp
)
target_include_directories(uldum_shell PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_shell
    PUBLIC
        rmlui_core
    PRIVATE
        uldum_core
        uldum_asset
        uldum_rhi
)
