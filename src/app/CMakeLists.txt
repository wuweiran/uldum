add_executable(uldum
    main.cpp
    engine.cpp
)

target_include_directories(uldum PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum PRIVATE
    uldum_core
    uldum_platform
    uldum_rhi
    uldum_asset
    uldum_render
    uldum_audio
    uldum_script
    uldum_simulation
    uldum_network
    uldum_map
    uldum_editor
)

# Console app in debug (to see logs), GUI app in release
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(WIN32)
        set_target_properties(uldum PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
endif()
