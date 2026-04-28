cmake_minimum_required(VERSION 3.28)
project(uldum VERSION 0.1.0 LANGUAGES CXX C)

# `CMAKE_SOURCE_DIR` points at the top-level project passed to cmake -S, which
# on Android is the wrapper under platforms/android/app/src/main/cpp/ — not
# the repo root. Use ULDUM_ROOT / ULDUM_SRC everywhere the target needs the
# engine's source tree, regardless of who invoked CMake.
set(ULDUM_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(ULDUM_SRC  ${ULDUM_ROOT}/src)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output binaries to a common directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Vulkan: on desktop we need glslc from the Vulkan SDK to compile shaders at
# build time. On Android the NDK ships Vulkan headers + libvulkan.so but no
# glslc — shaders are pre-compiled on desktop and shipped inside engine.uldpak,
# so the Android build skips shader compilation entirely.
if(ANDROID)
    find_package(Vulkan REQUIRED)
else()
    find_package(Vulkan REQUIRED COMPONENTS glslc)
endif()

# Shader compilation: GLSL → SPIR-V. Desktop only — Android relies on
# pre-compiled .spv files bundled in the APK's engine.uldpak asset, produced
# by a prior desktop build.
function(compile_shaders TARGET SHADER_DIR OUTPUT_DIR)
    if(ANDROID)
        return()  # no-op on Android (no glslc available in the NDK toolchain)
    endif()
    file(GLOB SHADERS ${SHADER_DIR}/*.vert ${SHADER_DIR}/*.frag ${SHADER_DIR}/*.comp)
    foreach(SHADER ${SHADERS})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPV_OUTPUT ${OUTPUT_DIR}/${SHADER_NAME}.spv)
        add_custom_command(
            OUTPUT ${SPV_OUTPUT}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${OUTPUT_DIR}
            COMMAND Vulkan::glslc --target-env=vulkan1.2 ${SHADER} -o ${SPV_OUTPUT}
            DEPENDS ${SHADER}
            COMMENT "Compiling shader ${SHADER_NAME}"
        )
        list(APPEND SPV_OUTPUTS ${SPV_OUTPUT})
    endforeach()
    add_custom_target(${TARGET}_shaders ALL DEPENDS ${SPV_OUTPUTS})
    add_dependencies(${TARGET} ${TARGET}_shaders)
endfunction()

# Platform defines
if(WIN32)
    add_compile_definitions(ULDUM_PLATFORM_WINDOWS)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
elseif(ANDROID)
    add_compile_definitions(ULDUM_PLATFORM_ANDROID)
endif()

# Debug defines
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(ULDUM_DEBUG)
endif()

# MSVC settings
if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Shell UI (RmlUi + FreeType) is compiled into game builds only. Derived
# from ULDUM_GAME_PROJECT_DIR so third_party/ can gate its fetch up front —
# dev/editor/server builds avoid the ~minute-long FreeType+RmlUi build.
# Game flavor on Android sets ULDUM_GAME_PROJECT_DIR via Gradle's
# -DULDUM_GAME_PROJECT_DIR=... cmake argument, so this catches it too.
if(ULDUM_GAME_PROJECT_DIR)
    set(ULDUM_SHELL_UI TRUE)
endif()

# Dev UI (ImGui-based map picker / session controls) is compiled into the
# desktop `uldum_dev` target only. Engine-dev-only iteration tool — not in
# editor (has its own ImGui), not in game (Shell UI), not in server (headless),
# not on Android (dev flavor uses a different flow).
if(NOT ANDROID AND NOT ULDUM_GAME_PROJECT_DIR)
    set(ULDUM_DEV_UI TRUE)
endif()

add_subdirectory(third_party)
add_subdirectory(src)
