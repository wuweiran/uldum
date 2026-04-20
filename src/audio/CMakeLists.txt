add_library(uldum_audio STATIC
    audio.cpp
)

target_include_directories(uldum_audio PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_audio PUBLIC uldum_core uldum_asset miniaudio)
