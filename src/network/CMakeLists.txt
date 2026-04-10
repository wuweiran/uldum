add_library(uldum_network STATIC
    network.cpp
    game_server.cpp
)

target_include_directories(uldum_network PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_network PUBLIC uldum_core uldum_simulation uldum_script uldum_map)
