include(FetchContent)

# Suppress deprecation warning for FetchContent_Populate on header-only libs
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# ── glm (math) ──────────────────────────────────────────────────────────────
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# ── nlohmann/json (JSON parser) ────────────────────────────────────────────
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "")
FetchContent_MakeAvailable(json)

# ── stb (stb_image, stb_vorbis) — header-only, no CMakeLists ──────────────
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_Populate(stb)

add_library(stb STATIC stb_impl.cpp)
target_include_directories(stb SYSTEM PUBLIC ${stb_SOURCE_DIR})
if(MSVC)
    target_compile_options(stb PRIVATE /W0)
endif()

# ── cgltf (glTF 2.0 loader) — header-only ─────────────────────────────────
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.14
    GIT_SHALLOW    TRUE
)
FetchContent_Populate(cgltf)

add_library(cgltf STATIC cgltf_impl.cpp)
target_include_directories(cgltf SYSTEM PUBLIC ${cgltf_SOURCE_DIR})
if(MSVC)
    target_compile_options(cgltf PRIVATE /W0)
endif()

# ── Lua 5.4 (scripting runtime) ────────────────────────────────────────────
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        v5.4.7
    GIT_SHALLOW    TRUE
)
FetchContent_Populate(lua)

# Build Lua as a static library from its source files
file(GLOB LUA_SOURCES ${lua_SOURCE_DIR}/*.c)
list(REMOVE_ITEM LUA_SOURCES ${lua_SOURCE_DIR}/lua.c ${lua_SOURCE_DIR}/luac.c ${lua_SOURCE_DIR}/onelua.c)
add_library(lua54 STATIC ${LUA_SOURCES})
target_include_directories(lua54 SYSTEM PUBLIC ${lua_SOURCE_DIR})
if(MSVC)
    target_compile_options(lua54 PRIVATE /W0)
endif()

# ── sol2 (C++ ↔ Lua binding, header-only) ─────────────────────────────────
FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG        v3.3.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(sol2)

# ── VMA (Vulkan Memory Allocator) — header-only ───────────────────────────
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.1.0
    GIT_SHALLOW    TRUE
)
FetchContent_Populate(vma)

add_library(vma STATIC vma_impl.cpp)
target_include_directories(vma SYSTEM PUBLIC ${vma_SOURCE_DIR}/include)
target_link_libraries(vma PUBLIC Vulkan::Vulkan)
if(MSVC)
    target_compile_options(vma PRIVATE /W0)
endif()
