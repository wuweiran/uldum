# ── uldum_pack — Package tool (pack/unpack .uldpak / .uldmap archives) ───

add_executable(uldum_pack pack_main.cpp)
target_include_directories(uldum_pack PRIVATE ${ULDUM_SRC})
target_link_libraries(uldum_pack PRIVATE uldum_asset uldum_core)

# ── uldum_gen_overlays — Procedural source generator for the overlay
# decal textures (engine/textures/overlays/*.png). Run after build to
# regenerate; pipe through scripts/png_to_ktx2.ps1 -Linear to produce
# the runtime KTX2 assets.
add_executable(uldum_gen_overlays gen_overlay_textures.cpp)
target_link_libraries(uldum_gen_overlays PRIVATE stb)
