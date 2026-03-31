add_library(uldum_simulation STATIC
    simulation.cpp
)

target_include_directories(uldum_simulation PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_simulation PUBLIC uldum_core uldum_script)
