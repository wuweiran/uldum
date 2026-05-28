include(FetchContent)

# Suppress deprecation warning for FetchContent_Populate on header-only libs
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# ── glm (math) ──────────────────────────────────────────────────────────────
message(STATUS "Fetching glm...")
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# ── nlohmann/json (JSON parser) ────────────────────────────────────────────
message(STATUS "Fetching nlohmann/json...")
FetchContent_Declare(json
    URL      https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
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
    URL      https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz
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
    URL      https://github.com/lua/lua/archive/refs/tags/v5.4.8.tar.gz
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
# v3.3.0's vendored optional has a reference-type specialization that trips
# newer Clang + C++23 ("no member named 'construct' in 'optional<T&>'"); the
# post-3.3.0 main branch has the fix merged.
message(STATUS "Fetching sol2...")
FetchContent_Declare(sol2
    URL      https://github.com/ThePhD/sol2/archive/refs/heads/main.tar.gz
)
FetchContent_MakeAvailable(sol2)

# ── Dear ImGui (dev console + editor) ─────────────────────────────────────
# Used by uldum_editor (desktop, Vulkan) and uldum_dev (desktop Vulkan +
# Android GLES). The renderer backend is picked at compile time:
#   • Vulkan target → imgui_impl_vulkan.cpp
#   • GLES target   → imgui_impl_opengl3.cpp (works on GLES 3.x via
#                     IMGUI_IMPL_OPENGL_ES3 — set by the dev console).
message(STATUS "Fetching Dear ImGui...")
FetchContent_Declare(imgui
    URL      https://github.com/ocornut/imgui/archive/refs/tags/v1.92.7.tar.gz
)
FetchContent_Populate(imgui)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)
if(ULDUM_BACKEND_VULKAN)
    list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
endif()
if(ULDUM_BACKEND_GLES)
    list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
endif()
if(WIN32)
    list(APPEND IMGUI_SOURCES ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)
endif()

add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
if(ULDUM_BACKEND_VULKAN)
    target_link_libraries(imgui PUBLIC Vulkan::Vulkan)
endif()
if(ULDUM_BACKEND_GLES)
    # GLES backend reads through GLAD2 / native GLES headers; imgui's
    # OpenGL3 backend autodetects via macros, but be explicit for GLES 3.x.
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES3)
    target_link_libraries(imgui PUBLIC GLESv3)
endif()
if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
endif()

# ── miniaudio (audio playback) — header-only ──────────────────────────────
message(STATUS "Fetching miniaudio...")
FetchContent_Declare(miniaudio
    URL      https://github.com/mackron/miniaudio/archive/refs/tags/0.11.25.tar.gz
)
FetchContent_Populate(miniaudio)

add_library(miniaudio STATIC miniaudio_impl.c)
target_include_directories(miniaudio SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})
target_include_directories(miniaudio PRIVATE ${stb_SOURCE_DIR})  # stb_vorbis header for OGG decoding
target_link_libraries(miniaudio PRIVATE stb)  # stb_vorbis implementation
if(MSVC)
    target_compile_options(miniaudio PRIVATE /W0)
endif()

# ── VMA (Vulkan Memory Allocator) — header-only ───────────────────────────
# Only consumed by the Vulkan RHI backend (rhi/vulkan/vulkan_rhi.cpp). Skip
# on builds that select a different backend (e.g. Android GLES).
if(ULDUM_BACKEND_VULKAN)
    message(STATUS "Fetching VMA...")
    FetchContent_Declare(vma
        URL      https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
    )
    FetchContent_Populate(vma)

    add_library(vma STATIC vma_impl.cpp)
    target_include_directories(vma SYSTEM PUBLIC ${vma_SOURCE_DIR}/include)
    target_link_libraries(vma PUBLIC Vulkan::Vulkan)
    if(MSVC)
        target_compile_options(vma PRIVATE /W0)
    endif()
endif()

# ── ENet (UDP networking) ─────────────────────────────────────────────────
message(STATUS "Fetching ENet...")
FetchContent_Declare(enet
    URL      https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz
)
FetchContent_Populate(enet)

file(GLOB ENET_SOURCES ${enet_SOURCE_DIR}/*.c)
add_library(enet STATIC ${ENET_SOURCES})
target_include_directories(enet SYSTEM PUBLIC ${enet_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()
if(MSVC)
    target_compile_options(enet PRIVATE /W0)
endif()
if(ANDROID)
    # Bionic already provides socklen_t, fcntl, gethostbyname_r, etc.;
    # without these defines ENet's unix.c retypedefs socklen_t and emits
    # declarations that conflict with the NDK headers.
    target_compile_definitions(enet PRIVATE
        HAS_SOCKLEN_T=1
        HAS_FCNTL=1
        HAS_POLL=1
        HAS_MSGHDR_FLAGS=1
        HAS_INET_PTON=1
        HAS_INET_NTOP=1
        HAS_GETADDRINFO=1
        HAS_GETNAMEINFO=1
    )
endif()

# ── cpp-httplib (HTTP server for uldum_server orchestrator — Phase 24) ────
# Single-header C++ HTTP server. We use it in plain-HTTP mode; TLS
# termination is the reverse proxy's job in production. Desktop-only —
# the orchestrator is a backend daemon, never built for Android.
if(NOT ANDROID)
    message(STATUS "Fetching cpp-httplib...")
    FetchContent_Declare(httplib
        URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.7.tar.gz
    )
    FetchContent_Populate(httplib)
    add_library(httplib INTERFACE)
    target_include_directories(httplib SYSTEM INTERFACE ${httplib_SOURCE_DIR})
endif()

# ── Basis Universal (KTX2 container + UASTC encoder/transcoder) ───────────
# BasisU is the codec behind KTX2 UASTC/ETC1S. It ships both an authoring
# CLI (basisu.exe — we invoke it from scripts/png_to_ktx2.ps1) and a
# self-contained transcoder that can parse KTX2 files end-to-end.
#
# We build two things from the same source tree:
#   - `basisu_transcoder` — a static lib made from just transcoder/*.cpp, linked
#     into uldum_asset for runtime decoding
#   - `basisu` — their full build (encoder + CLI), used only at author time
#
# Replaces KTX-Software, which bundles more than we need (fmt, cxxopts,
# astc-encoder, five extra CLI tools) and hit MSVC/fmt consteval issues.
message(STATUS "Fetching Basis Universal...")
FetchContent_Declare(basisu
    URL https://github.com/BinomialLLC/basis_universal/archive/refs/heads/master.tar.gz
)
FetchContent_Populate(basisu)

# Runtime transcoder library — minimal, no encoder, no CLI.
add_library(basisu_transcoder STATIC
    ${basisu_SOURCE_DIR}/transcoder/basisu_transcoder.cpp
)
target_include_directories(basisu_transcoder SYSTEM PUBLIC
    ${basisu_SOURCE_DIR}/transcoder
)
# KTX2 container support on; zstd-compressed KTX2 off (we use plain UASTC).
target_compile_definitions(basisu_transcoder PUBLIC
    BASISD_SUPPORT_KTX2=1
    BASISD_SUPPORT_KTX2_ZSTD=0
)
if(MSVC)
    target_compile_options(basisu_transcoder PRIVATE /W0)
endif()

# ── FreeType (font rasterization) ────────────────────────────────────────
# Fetched unconditionally: needed by uldum_render's MSDF glyph pipeline
# (font.cpp) and by uldum_shell / RmlUi (game builds only). Minimal —
# strip brotli, zlib, harfbuzz, png, bzip2 backends we don't use.
#
# Save caller's BUILD_SHARED_LIBS; force static for just these deps to keep
# the dist folder self-contained (no DLL copying needed), then restore.
set(_ULDUM_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

message(STATUS "Fetching FreeType...")
set(FT_DISABLE_BROTLI   TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB     TRUE CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG        VER-2-13-3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(freetype)
if(MSVC AND TARGET freetype)
    target_compile_options(freetype PRIVATE /W0)
endif()
# RmlUi calls find_package(Freetype) which expects the namespaced target
# `Freetype::Freetype`. FreeType's own CMake only exports the unadorned
# `freetype` target, so alias across.
if(TARGET freetype AND NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# The engine ships no fonts. `Font::init_from_system` discovers the
# platform's curated chain at runtime (Windows: Segoe UI + msyh + emoji;
# macOS: Helvetica + PingFang + Apple Color Emoji; Android: Roboto +
# NotoSansCJK + NotoColorEmoji; Linux: DejaVu / Noto via common paths).

# Restore the caller's BUILD_SHARED_LIBS setting.
set(BUILD_SHARED_LIBS ${_ULDUM_SAVED_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)
unset(_ULDUM_SAVED_BUILD_SHARED_LIBS)

# ── RmlUi (Shell UI framework) ───────────────────────────────────────────
# Used only by uldum_game (desktop) and the Android `game` flavor — never
# by uldum_dev / editor / server / android dev flavor. We implement our
# own RenderInterface + SystemInterface + FileInterface on top of our
# Vulkan RHI and asset manager; RmlUi's bundled backends aren't used.
if(ULDUM_SHELL_UI)
    set(_ULDUM_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

    message(STATUS "Fetching RmlUi...")
    # BACKEND must be a valid value (RmlUi rejects "none"), but with
    # RMLUI_SAMPLES=OFF the backend code isn't built. "auto" is the safe
    # sentinel — the samples directory is skipped so the backend is never
    # actually compiled or linked.
    set(RMLUI_BACKEND       "auto"     CACHE STRING "" FORCE)
    set(RMLUI_SAMPLES       OFF        CACHE BOOL   "" FORCE)
    set(RMLUI_LUA_BINDINGS  OFF        CACHE BOOL   "" FORCE)
    set(RMLUI_SVG_PLUGIN    OFF        CACHE BOOL   "" FORCE)
    set(RMLUI_LOTTIE_PLUGIN OFF        CACHE BOOL   "" FORCE)
    set(RMLUI_FONT_ENGINE   "freetype" CACHE STRING "" FORCE)
    FetchContent_Declare(rmlui
        GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
        GIT_TAG        6.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(rmlui)
    if(MSVC)
        foreach(T rmlui_core rmlui_debugger)
            if(TARGET ${T})
                target_compile_options(${T} PRIVATE /W0)
            endif()
        endforeach()
    endif()

    set(BUILD_SHARED_LIBS ${_ULDUM_SAVED_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)
    unset(_ULDUM_SAVED_BUILD_SHARED_LIBS)
endif()

# Their full build — gives us the `basisu` CLI executable for authoring.
# Desktop-only: the CLI is a host tool (PNG → KTX2 encoding offline); it's
# never invoked on device. Android builds skip this to avoid cross-compiling
# an encoder that would never run.
#
# BasisU's own CMakeLists hard-sets CMAKE_RUNTIME_OUTPUT_DIRECTORY to a
# subdirectory under their source tree, and target-scoped overrides don't
# dislodge it. Copy the binary into our build/bin/ with a POST_BUILD step so
# png_to_ktx2.ps1 can find it alongside the engine executables.
if(NOT ANDROID)
    add_subdirectory(${basisu_SOURCE_DIR} ${basisu_BINARY_DIR})
    if(TARGET basisu)
        if(MSVC)
            target_compile_options(basisu PRIVATE /W0)
        endif()
        add_custom_target(basisu_copy_to_bin ALL
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:basisu>
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<TARGET_FILE_NAME:basisu>
            COMMENT "Copying basisu.exe to build/bin/")
        add_dependencies(basisu_copy_to_bin basisu)
    endif()
endif()
