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

# Compile shaders (GLSL → SPIR-V) into the output engine/shaders directory
compile_shaders(uldum
    ${CMAKE_SOURCE_DIR}/engine/shaders
    ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine/shaders
)

# Always copy engine assets and maps to output directory.
# Uses a custom target (not POST_BUILD) so it runs even when no C++ changed.
add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/engine
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/maps
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps
    COMMENT "Syncing engine assets and maps to output directory"
)
add_dependencies(uldum copy_assets)
