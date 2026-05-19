add_library(uldum_i18n STATIC
    locale.cpp
)

target_link_libraries(uldum_i18n PUBLIC
    uldum_core
    uldum_asset
    nlohmann_json::nlohmann_json
)

target_include_directories(uldum_i18n PUBLIC ${ULDUM_SRC})
