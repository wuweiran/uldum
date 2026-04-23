add_library(uldum_network STATIC
    network.cpp
    game_server.cpp
    enet_transport.cpp
)

target_include_directories(uldum_network PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_network PUBLIC uldum_core uldum_simulation uldum_script uldum_map uldum_input uldum_hud enet)
