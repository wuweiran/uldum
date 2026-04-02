add_library(uldum_script STATIC
    script.cpp
)

target_include_directories(uldum_script PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_script PUBLIC uldum_core uldum_simulation uldum_map lua54 sol2 glm::glm)
