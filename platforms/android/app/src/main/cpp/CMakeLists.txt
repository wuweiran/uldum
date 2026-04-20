# Android app's CMake. AGP invokes this with the NDK toolchain already configured.
#
# We add the repo root as a subdirectory: the root CMakeLists branches on
# ANDROID to produce a single shared library target `uldum_game` (src/app/
# CMakeLists.txt's if(ANDROID) branch). All the engine's static libs, the
# portable third-party deps (Basis transcoder, miniaudio, ENet, Lua, sol2,
# VMA, stb, cgltf, json, glm), and the Android platform layer compile here.
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

# `uldum_game` is the SHARED library target defined in src/app/CMakeLists.txt's
# if(ANDROID) branch. GameActivity loads it by name (see the
# android.app.lib_name metadata in AndroidManifest.xml → "uldum_game").
if(NOT TARGET uldum_game)
    message(FATAL_ERROR "Expected target 'uldum_game' from the engine tree was not created.")
endif()
