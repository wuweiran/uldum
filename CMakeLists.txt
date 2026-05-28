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

# Vulkan SDK is required:
#   • on Windows for the Vulkan backend implementation + glslc shader compiler;
#   • on every desktop build (even GLES-target packaging) for glslc, since
#     shaders are authored as GLSL and compiled to SPIR-V at desktop build
#     time, then optionally cross-compiled to GLSL ES for the Android bundle.
# Android-only builds (compiled with the NDK) never link Vulkan; the GLES
# runtime comes from libGLESv3 / libEGL.
if(NOT ANDROID)
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
    # GLSL ES translation for the Android bundle is handled by the Android
    # Gradle build (see platforms/android/app/build.gradle.kts), not by
    # desktop CMake. Desktop builds only target Vulkan and don't need .glsl.
endfunction()

# Platform defines
if(WIN32)
    add_compile_definitions(ULDUM_PLATFORM_WINDOWS)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
elseif(ANDROID)
    add_compile_definitions(ULDUM_PLATFORM_ANDROID)
endif()

# RHI backend selection. Each backend ships its own implementation of the
# `rhi::Rhi` class with an identical public interface. Consumer code is
# backend-agnostic — only the build system picks one.
#
# Platform mapping:
#   • Android  → OpenGL ES 3.1 (lets us hit lower minSdk than Vulkan would)
#   • Windows / Linux / macOS → Vulkan 1.3
#
# To force a non-default backend for testing, override on the command line
# (e.g. `cmake -DULDUM_BACKEND_VULKAN=ON -DULDUM_BACKEND_GLES=OFF` for a
# Vulkan-on-Android experiment).
if(ANDROID)
    set(ULDUM_BACKEND_VULKAN OFF CACHE BOOL "Enable Vulkan RHI backend")
    set(ULDUM_BACKEND_GLES   ON  CACHE BOOL "Enable OpenGL ES 3.1 RHI backend")
else()
    set(ULDUM_BACKEND_VULKAN ON  CACHE BOOL "Enable Vulkan RHI backend")
    set(ULDUM_BACKEND_GLES   OFF CACHE BOOL "Enable OpenGL ES 3.1 RHI backend")
endif()

if(ULDUM_BACKEND_GLES AND NOT ANDROID)
    message(WARNING "ULDUM_BACKEND_GLES is Android-only; ignored on this platform")
    set(ULDUM_BACKEND_GLES OFF)
endif()
if(ULDUM_BACKEND_GLES AND ULDUM_BACKEND_VULKAN)
    message(FATAL_ERROR "ULDUM_BACKEND_GLES and ULDUM_BACKEND_VULKAN are mutually exclusive")
endif()
if(ULDUM_BACKEND_GLES)
    add_compile_definitions(ULDUM_BACKEND_GLES)
elseif(ULDUM_BACKEND_VULKAN)
    add_compile_definitions(ULDUM_BACKEND_VULKAN)
else()
    message(FATAL_ERROR "No RHI backend selected. Enable ULDUM_BACKEND_VULKAN or ULDUM_BACKEND_GLES.")
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
