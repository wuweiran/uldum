add_library(uldum_core STATIC
    log.cpp
)

target_include_directories(uldum_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
