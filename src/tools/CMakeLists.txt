# ── uldum_pack — Package tool (pack/unpack .uldpak / .uldmap archives) ───

add_executable(uldum_pack pack_main.cpp)
target_include_directories(uldum_pack PRIVATE ${ULDUM_SRC})
target_link_libraries(uldum_pack PRIVATE uldum_asset uldum_core)
