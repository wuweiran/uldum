add_library(uldum_map STATIC
    map.cpp
)

target_include_directories(uldum_map PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_map PUBLIC uldum_core uldum_asset)
