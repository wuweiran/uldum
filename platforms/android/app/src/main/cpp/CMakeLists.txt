# Android app's CMake. AGP invokes this with the NDK toolchain already configured.
#
# We add the repo root as a subdirectory: the root CMakeLists branches on
# ANDROID to produce one shared library target `uldum` (both dev and game
# flavors build the same target; they differ only in compile-time flags
# and APK-level config).
#
# Desktop-only bits (imgui, basisu.exe CLI, uldum_pack tool, uldum_editor,
# uldum_dev/game/server exes, shader compilation via glslc, pack_assets
# target) are guarded off in the relevant CMakeLists when ANDROID is true.

cmake_minimum_required(VERSION 3.28)

# games-activity AAR prefab — src/app/CMakeLists.txt's if(ANDROID) branch
# links `game-activity::game-activity_static`, so the package config must be
# imported before we add the engine subtree.
find_package(game-activity REQUIRED CONFIG)

set(ULDUM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../..")
if(NOT EXISTS "${ULDUM_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "ULDUM_ROOT resolved to '${ULDUM_ROOT}' but no CMakeLists.txt there.")
endif()

add_subdirectory("${ULDUM_ROOT}" "${CMAKE_BINARY_DIR}/uldum")

if(NOT TARGET uldum)
    message(FATAL_ERROR "Expected target 'uldum' from the engine tree was not created.")
endif()
