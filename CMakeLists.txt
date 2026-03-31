cmake_minimum_required(VERSION 3.28)
project(uldum VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output binaries to a common directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Vulkan SDK
find_package(Vulkan REQUIRED)

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

add_subdirectory(src)
