add_library(uldum_core STATIC
    log.cpp
    allocator.cpp
)

target_include_directories(uldum_core PUBLIC ${ULDUM_SRC})
