# Shared engine libraries used by all game targets
set(ULDUM_ENGINE_LIBS
    uldum_core
    uldum_platform
    uldum_rhi
    uldum_asset
    uldum_i18n
    uldum_render
    uldum_audio
    uldum_script
    uldum_simulation
    uldum_network
    uldum_input
    uldum_input_router
    uldum_map
    uldum_hud
)

# Staging dir: source engine/ + compiled shaders are assembled here,
# then packed into engine.uldpak. Shared between engine and game builds.
set(ULDUM_ENGINE_STAGING ${CMAKE_BINARY_DIR}/staging/engine)


if(ANDROID)

    # ── Android: single shared library loaded by GameActivity ────────────────
    # Android apps don't ship executables — GameActivity loads libuldum.so
    # and calls android_main() inside it. The dev and game flavors produce
    # separate APKs with separate applicationIds, so they can't collide on
    # a device — we don't need the `.so` name to encode which flavor it is.
    # Flavor differences are compile-time:
    #   Dev flavor   → -DULDUM_ANDROID_DEV=1     (compiles in dev console)
    #   Game flavor  → -DULDUM_GAME_PROJECT_DIR=...  (compiles in Shell UI,
    #                                                 reads project's game.json)

    add_library(uldum SHARED
        android_main.cpp
        engine.cpp
    )
    target_include_directories(uldum PRIVATE ${ULDUM_SRC})
    target_link_libraries(uldum PRIVATE
        ${ULDUM_ENGINE_LIBS}
        -Wl,--whole-archive
        game-activity::game-activity_static
        -Wl,--no-whole-archive
        log
        android
    )
    # Dev flavor: bring in the ImGui dev console (map picker + session
    # controls) so engine work on a real device matches the desktop
    # uldum_dev experience. Game-flavor APKs skip this — they use the
    # game project's Shell UI / RmlUi for menus instead.
    #
    # ImGui's Vulkan backend is the only one wired into the engine, so
    # The dev console works on both backends: ImGui's Vulkan backend on
    # Vulkan builds, its OpenGL3 backend (GLES 3.x compatible) on GLES
    # builds. dev_console.cpp picks the right one via ULDUM_BACKEND_*.
    if(ULDUM_ANDROID_DEV)
        target_compile_definitions(uldum PRIVATE ULDUM_DEV_UI)
        target_sources(uldum PRIVATE dev_console.cpp dev_app.cpp)
        target_link_libraries(uldum PRIVATE imgui)
    endif()
    # Game flavor: bring in the project's Shell UI (RmlUi). Same
    # uldum_shell library + ULDUM_SHELL_UI macro the desktop game
    # build uses. Gated on ULDUM_SHELL_UI which is auto-set in the
    # root CMake when ULDUM_GAME_PROJECT_DIR is supplied (Gradle's
    # game flavor passes it via -DULDUM_GAME_PROJECT_DIR=...).
    if(ULDUM_SHELL_UI)
        target_compile_definitions(uldum PRIVATE ULDUM_SHELL_UI)
        target_link_libraries(uldum PRIVATE uldum_shell)
        # If the game project ships its own game.cmake, include it
        # and pick up the variables it sets (sources, include dirs, App
        # header/class). Without game.cmake, the App macros below
        # default to NullApp.
        set(_uldum_game_cmake "${ULDUM_GAME_PROJECT_DIR}/game.cmake")
        if(EXISTS "${_uldum_game_cmake}")
            set(ULDUM_GAME_SOURCES "")
            set(ULDUM_GAME_INCLUDE_DIRS "")
            include("${_uldum_game_cmake}")
            target_sources(uldum PRIVATE ${ULDUM_GAME_SOURCES})
            if(ULDUM_GAME_INCLUDE_DIRS)
                target_include_directories(uldum PRIVATE ${ULDUM_GAME_INCLUDE_DIRS})
            endif()
            message(STATUS "Uldum game project: ${ULDUM_GAME_PROJECT_DIR}")
            message(STATUS "  sources:        ${ULDUM_GAME_SOURCES}")
            message(STATUS "  include dirs:   ${ULDUM_GAME_INCLUDE_DIRS}")
            message(STATUS "  app:            ${ULDUM_APP_HEADER} / ${ULDUM_APP_CLASS}")
        endif()
    endif()

    # Which App class engine.cpp instantiates. Dev flavor → DevApp;
    # game flavor defaults to NullApp unless the project's game.cmake
    # already set both macros above.
    if(ULDUM_ANDROID_DEV)
        set(_uldum_app_header "app/dev_app.h")
        set(_uldum_app_class DevApp)
    else()
        if(NOT DEFINED ULDUM_APP_HEADER)
            set(ULDUM_APP_HEADER "app/app.h")
            set(ULDUM_APP_CLASS NullApp)
        endif()
        set(_uldum_app_header "${ULDUM_APP_HEADER}")
        set(_uldum_app_class ${ULDUM_APP_CLASS})
    endif()
    target_compile_definitions(uldum PRIVATE
        ULDUM_APP_HEADER="${_uldum_app_header}"
        ULDUM_APP_CLASS=${_uldum_app_class})


elseif(ULDUM_GAME_PROJECT_DIR)

    # ── Desktop GAME build ───────────────────────────────────────────────────
    # Invoked by build_game.ps1 with -DULDUM_GAME_PROJECT_DIR=<abs-path>.
    # Produces one shippable uldum_game.exe:
    #   - exe name from game.json.app_id ("uldum-sample" → "uldum-sample.exe")
    #   - icon from <project>/branding/icon.ico
    #   - only maps listed in game.json.maps[]
    #   - game.json + engine.uldpak bundled alongside
    # Per-project build tree under build/game-<name>/ keeps artifacts isolated.

    # Shell UI (RmlUi) is a game-only dependency; set in the root CMakeLists
    # before third_party/ is processed. See ULDUM_SHELL_UI there.

    if(NOT IS_DIRECTORY ${ULDUM_GAME_PROJECT_DIR})
        message(FATAL_ERROR "ULDUM_GAME_PROJECT_DIR='${ULDUM_GAME_PROJECT_DIR}' is not a directory")
    endif()
    set(ULDUM_GAME_JSON_PATH ${ULDUM_GAME_PROJECT_DIR}/game.json)
    if(NOT EXISTS ${ULDUM_GAME_JSON_PATH})
        message(FATAL_ERROR "${ULDUM_GAME_JSON_PATH} not found")
    endif()

    file(READ ${ULDUM_GAME_JSON_PATH} ULDUM_GAME_JSON)
    string(JSON ULDUM_GAME_NAME GET ${ULDUM_GAME_JSON} name)
    # Exe name is the app_id slug (e.g. "uldum-sample"). Must match build_game.ps1.
    string(JSON ULDUM_GAME_EXE_NAME GET ${ULDUM_GAME_JSON} app_id)
    message(STATUS "Uldum game: '${ULDUM_GAME_NAME}' -> ${ULDUM_GAME_EXE_NAME}.exe")

    # Extract maps list from game.json. Each entry is a map id; resolves to
    # <project>/maps/<id>.uldmap/.
    string(JSON ULDUM_GAME_MAPS_COUNT LENGTH ${ULDUM_GAME_JSON} maps)
    if(ULDUM_GAME_MAPS_COUNT EQUAL 0)
        message(FATAL_ERROR "${ULDUM_GAME_JSON_PATH}: 'maps' array is empty")
    endif()
    math(EXPR ULDUM_GAME_MAPS_LAST "${ULDUM_GAME_MAPS_COUNT}-1")
    set(PACK_GAME_MAP_COMMANDS "")
    foreach(i RANGE ${ULDUM_GAME_MAPS_LAST})
        string(JSON MAP_ID GET ${ULDUM_GAME_JSON} maps ${i})
        set(MAP_SRC ${ULDUM_GAME_PROJECT_DIR}/maps/${MAP_ID}.uldmap)
        if(NOT IS_DIRECTORY ${MAP_SRC})
            message(FATAL_ERROR "game.json references '${MAP_ID}' but ${MAP_SRC} does not exist")
        endif()
        list(APPEND PACK_GAME_MAP_COMMANDS
            COMMAND $<TARGET_FILE:uldum_pack> pack
                ${MAP_SRC}
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps/${MAP_ID}.uldmap)
    endforeach()

    # If the game project ships a shell/ folder (RML + RCSS for main menu,
    # settings, etc.), copy it as loose files into the dist. Later we may
    # pack it into a .uldpak — today the Shell's FileInterface reads loose
    # files directly from the working directory.
    set(COPY_SHELL_COMMAND "")
    if(IS_DIRECTORY ${ULDUM_GAME_PROJECT_DIR}/shell)
        set(COPY_SHELL_COMMAND
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${ULDUM_GAME_PROJECT_DIR}/shell
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shell)
    endif()

    # Pack engine.uldpak + listed maps + game.json + shell/ into the dist folder.
    add_custom_target(pack_game_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${ULDUM_ROOT}/engine
            ${ULDUM_ENGINE_STAGING}
        COMMAND $<TARGET_FILE:uldum_pack> pack
            ${ULDUM_ENGINE_STAGING}
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine.uldpak
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps
        ${PACK_GAME_MAP_COMMANDS}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ULDUM_GAME_JSON_PATH}
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/game.json
        ${COPY_SHELL_COMMAND}
        COMMENT "Packing '${ULDUM_GAME_NAME}' assets"
        VERBATIM
    )

    # Windows icon: committed .ico embedded via a generated .rc.
    set(GAME_ICON_ICO ${ULDUM_GAME_PROJECT_DIR}/branding/icon.ico)
    if(WIN32 AND EXISTS ${GAME_ICON_ICO})
        set(GAME_ICON_RC ${CMAKE_BINARY_DIR}/staging/uldum_game_icon.rc)
        file(WRITE ${GAME_ICON_RC} "IDI_ICON1 ICON \"${GAME_ICON_ICO}\"\n")
        set_source_files_properties(${GAME_ICON_RC}
            PROPERTIES OBJECT_DEPENDS ${GAME_ICON_ICO})
        set(ULDUM_GAME_ICON_SRC ${GAME_ICON_RC})
    else()
        set(ULDUM_GAME_ICON_SRC "")
    endif()

    add_executable(uldum_game
        game_main.cpp
        engine.cpp
        ${ULDUM_GAME_ICON_SRC}
    )
    set_target_properties(uldum_game PROPERTIES OUTPUT_NAME ${ULDUM_GAME_EXE_NAME})
    target_include_directories(uldum_game PRIVATE ${ULDUM_SRC})
    target_link_libraries(uldum_game PRIVATE ${ULDUM_ENGINE_LIBS})
    if(ULDUM_SHELL_UI)
        target_link_libraries(uldum_game PRIVATE uldum_shell)
        target_compile_definitions(uldum_game PRIVATE ULDUM_SHELL_UI)
    endif()

    # Pull in the project's App + macro overrides (if any). With no
    # game.cmake, fall back to NullApp.
    set(_uldum_game_cmake "${ULDUM_GAME_PROJECT_DIR}/game.cmake")
    if(EXISTS "${_uldum_game_cmake}")
        set(ULDUM_GAME_SOURCES "")
        set(ULDUM_GAME_INCLUDE_DIRS "")
        include("${_uldum_game_cmake}")
        target_sources(uldum_game PRIVATE ${ULDUM_GAME_SOURCES})
        if(ULDUM_GAME_INCLUDE_DIRS)
            target_include_directories(uldum_game PRIVATE ${ULDUM_GAME_INCLUDE_DIRS})
        endif()
        message(STATUS "Uldum game project: ${ULDUM_GAME_PROJECT_DIR}")
        message(STATUS "  sources:        ${ULDUM_GAME_SOURCES}")
        message(STATUS "  include dirs:   ${ULDUM_GAME_INCLUDE_DIRS}")
        message(STATUS "  app:            ${ULDUM_APP_HEADER} / ${ULDUM_APP_CLASS}")
    endif()
    if(NOT DEFINED ULDUM_APP_HEADER)
        set(ULDUM_APP_HEADER "app/app.h")
        set(ULDUM_APP_CLASS NullApp)
    endif()
    target_compile_definitions(uldum_game PRIVATE
        ULDUM_APP_HEADER="${ULDUM_APP_HEADER}"
        ULDUM_APP_CLASS=${ULDUM_APP_CLASS})
    # Identity compile-defs for engine.cpp, from game.json: ULDUM_APP_ID keys
    # the data folder, ULDUM_APP_NAME is the window title. Non-game builds omit
    # them and use engine.cpp's "Uldum" fallback.
    string(JSON ULDUM_GAME_APP_ID ERROR_VARIABLE _app_id_err GET ${ULDUM_GAME_JSON} app_id)
    if(NOT _app_id_err)
        target_compile_definitions(uldum_game PRIVATE ULDUM_APP_ID="${ULDUM_GAME_APP_ID}")
    endif()
    target_compile_definitions(uldum_game PRIVATE ULDUM_APP_NAME="${ULDUM_GAME_NAME}")
    compile_shaders(uldum_game
        ${ULDUM_SRC}/render/shaders/vulkan
        ${ULDUM_ENGINE_STAGING}/shaders
    )
    add_dependencies(pack_game_assets uldum_pack uldum_game_shaders)
    add_dependencies(uldum_game pack_game_assets)


else()

    # ── Desktop ENGINE build ─────────────────────────────────────────────────
    # No game project supplied. Builds engine developer tools:
    #   uldum_dev (here), uldum_editor (src/editor/), uldum_worker + uldum_server
    #   (src/server/), uldum_pack (src/tools/).
    # Packs engine assets + every .uldmap folder under maps/ for dev runtime.
    # Does NOT produce uldum_game — that's build_game.ps1's job.

    add_executable(uldum_dev
        dev_main.cpp
        engine.cpp
    )
    target_include_directories(uldum_dev PRIVATE ${ULDUM_SRC})
    target_link_libraries(uldum_dev PRIVATE ${ULDUM_ENGINE_LIBS})
    # Tier-1 Lua syntax checker — dev-only tooling, NOT added to
    # ULDUM_ENGINE_LIBS so it can never reach uldum_game (which shares that
    # var). Keeps the shipping game runtime-free of validation code.
    target_link_libraries(uldum_dev PRIVATE uldum_scriptcheck)
    # cpp-httplib for the orchestrator-via-HTTP flow (`--server`).
    # uldum_dev acts briefly as a game-backend stand-in: POSTs `/sessions`
    # to `uldum_server`, takes one token, then becomes the first player.
    # Header-only, no SSL needed (local dev uses plain http://).
    target_link_libraries(uldum_dev PRIVATE httplib)
    if(ULDUM_DEV_UI)
        target_sources(uldum_dev PRIVATE dev_console.cpp dev_app.cpp)
        target_link_libraries(uldum_dev PRIVATE imgui)
        target_compile_definitions(uldum_dev PRIVATE ULDUM_DEV_UI)
        target_compile_definitions(uldum_dev PRIVATE
            ULDUM_APP_HEADER="app/dev_app.h"
            ULDUM_APP_CLASS=DevApp)
    else()
        # No dev console → use NullApp so the macro-driven instantiation
        # in engine.cpp resolves to something benign.
        target_compile_definitions(uldum_dev PRIVATE
            ULDUM_APP_HEADER="app/app.h"
            ULDUM_APP_CLASS=NullApp)
    endif()
    compile_shaders(uldum_dev
        ${ULDUM_SRC}/render/shaders/vulkan
        ${ULDUM_ENGINE_STAGING}/shaders
    )

    # Glob every .uldmap folder in maps/ so new dev maps land automatically.
    file(GLOB ULDUM_ENGINE_MAP_DIRS LIST_DIRECTORIES true
        RELATIVE ${ULDUM_ROOT}/maps
        ${ULDUM_ROOT}/maps/*.uldmap)
    set(PACK_ENGINE_MAP_COMMANDS "")
    foreach(MAP ${ULDUM_ENGINE_MAP_DIRS})
        list(APPEND PACK_ENGINE_MAP_COMMANDS
            COMMAND $<TARGET_FILE:uldum_pack> pack
                ${ULDUM_ROOT}/maps/${MAP}
                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps/${MAP})
    endforeach()

    add_custom_target(pack_engine_maps ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${ULDUM_ROOT}/engine
            ${ULDUM_ENGINE_STAGING}
        COMMAND $<TARGET_FILE:uldum_pack> pack
            ${ULDUM_ENGINE_STAGING}
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/engine.uldpak
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/maps
        ${PACK_ENGINE_MAP_COMMANDS}
        COMMENT "Packing engine assets + dev maps"
        VERBATIM
    )
    add_dependencies(pack_engine_maps uldum_pack uldum_dev_shaders)
    add_dependencies(uldum_dev pack_engine_maps)

endif()
