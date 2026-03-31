add_library(uldum_network STATIC
    network.cpp
)

target_include_directories(uldum_network PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_network PUBLIC uldum_core uldum_simulation)
