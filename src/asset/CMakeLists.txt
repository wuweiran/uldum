add_library(uldum_asset STATIC
    asset.cpp
    texture.cpp
    model.cpp
    config.cpp
)

target_include_directories(uldum_asset PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_asset PUBLIC uldum_core stb cgltf glm::glm nlohmann_json::nlohmann_json)
