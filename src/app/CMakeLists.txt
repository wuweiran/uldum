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

# Copy engine assets to output directory after build
add_custom_command(TARGET uldum POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/engine
        $<TARGET_FILE_DIR:uldum>/engine
    COMMENT "Copying engine assets to output directory"
)

# Copy maps to output directory after build
add_custom_command(TARGET uldum POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/maps
        $<TARGET_FILE_DIR:uldum>/maps
    COMMENT "Copying maps to output directory"
)
