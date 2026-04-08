include(FetchContent)

# Suppress deprecation warning for FetchContent_Populate on header-only libs
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# ── glm (math) ──────────────────────────────────────────────────────────────
message(STATUS "Fetching glm...")
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# ── nlohmann/json (JSON parser) ────────────────────────────────────────────
message(STATUS "Fetching nlohmann/json...")
FetchContent_Declare(json
    URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH MD5=c23a33f04786d85c29fda8d16b5f0efd
)
set(JSON_BuildTests OFF CACHE BOOL "")
FetchContent_MakeAvailable(json)

# ── stb (stb_image, stb_vorbis) — header-only, no CMakeLists ──────────────
message(STATUS "Fetching stb...")
FetchContent_Declare(stb
    URL      https://github.com/nothings/stb/archive/refs/heads/master.tar.gz
)
FetchContent_Populate(stb)

add_library(stb STATIC stb_impl.cpp)
target_include_directories(stb SYSTEM PUBLIC ${stb_SOURCE_DIR})
if(MSVC)
    target_compile_options(stb PRIVATE /W0)
endif()

# ── cgltf (glTF 2.0 loader) — header-only ─────────────────────────────────
message(STATUS "Fetching cgltf...")
FetchContent_Declare(cgltf
    URL      https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.14.tar.gz
)
FetchContent_Populate(cgltf)

add_library(cgltf STATIC cgltf_impl.cpp)
target_include_directories(cgltf SYSTEM PUBLIC ${cgltf_SOURCE_DIR})
if(MSVC)
    target_compile_options(cgltf PRIVATE /W0)
endif()

# ── Lua 5.4 (scripting runtime) ────────────────────────────────────────────
message(STATUS "Fetching Lua 5.4...")
FetchContent_Declare(lua
    URL      https://github.com/lua/lua/archive/refs/tags/v5.4.7.tar.gz
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
message(STATUS "Fetching sol2...")
FetchContent_Declare(sol2
    URL      https://github.com/ThePhD/sol2/archive/refs/tags/v3.3.0.tar.gz
)
FetchContent_MakeAvailable(sol2)

# ── Dear ImGui (editor UI) ────────────────────────────────────────────────
message(STATUS "Fetching Dear ImGui...")
FetchContent_Declare(imgui
    URL      https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8.tar.gz
)
FetchContent_Populate(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan)
if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
endif()

# ── miniaudio (audio playback) — header-only ──────────────────────────────
message(STATUS "Fetching miniaudio...")
FetchContent_Declare(miniaudio
    URL      https://github.com/mackron/miniaudio/archive/refs/tags/0.11.21.tar.gz
)
FetchContent_Populate(miniaudio)

add_library(miniaudio STATIC miniaudio_impl.c)
target_include_directories(miniaudio SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})
if(MSVC)
    target_compile_options(miniaudio PRIVATE /W0)
endif()

# ── VMA (Vulkan Memory Allocator) — header-only ───────────────────────────
message(STATUS "Fetching VMA...")
FetchContent_Declare(vma
    URL      https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.1.0.tar.gz
)
FetchContent_Populate(vma)

add_library(vma STATIC vma_impl.cpp)
target_include_directories(vma SYSTEM PUBLIC ${vma_SOURCE_DIR}/include)
target_link_libraries(vma PUBLIC Vulkan::Vulkan)
if(MSVC)
    target_compile_options(vma PRIVATE /W0)
endif()
