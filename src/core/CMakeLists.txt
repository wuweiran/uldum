add_library(uldum_core STATIC
    log.cpp
    hash.cpp
    settings.cpp
)

target_include_directories(uldum_core PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_core PUBLIC nlohmann_json::nlohmann_json)
