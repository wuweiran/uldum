# Shared engine libraries used by all game targets
set(ULDUM_ENGINE_LIBS
    uldum_core
    uldum_platform
    uldum_rhi
    uldum_asset
    uldum_render
    uldum_audio
    uldum_script
    uldum_simulation
    uldum_network
    uldum_input
    uldum_map
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
    # Flavor differences are compile-time flags:
    #   Dev flavor   → -DULDUM_ANDROID_DEV=1     (auto-start on test_map)
    #   Game flavor  → -DULDUM_GAME_PROJECT_DIR=...  (read game.json)

    add_library(uldum SHARED
        android_main.cpp
        app.cpp
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
    # Gradle's `arguments += "-DULDUM_ANDROID_DEV=1"` sets a CMake cache
    # variable, not a preprocessor macro. Propagate it explicitly so the
    # `#ifdef ULDUM_ANDROID_DEV` branch in android_main.cpp actually fires.
    if(ULDUM_ANDROID_DEV)
        target_compile_definitions(uldum PRIVATE ULDUM_ANDROID_DEV)
    endif()


elseif(ULDUM_GAME_PROJECT_DIR)

    # ── Desktop GAME build ───────────────────────────────────────────────────
    # Invoked by build_game.ps1 with -DULDUM_GAME_PROJECT_DIR=<abs-path>.
    # Produces one shippable uldum_game.exe:
    #   - exe name from game.json.name ("Uldum Sample" → "UldumSample.exe")
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
    # PascalCase exe name: strip spaces from the display name.
    string(REPLACE " " "" ULDUM_GAME_EXE_NAME ${ULDUM_GAME_NAME})
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
        app.cpp
        ${ULDUM_GAME_ICON_SRC}
    )
    set_target_properties(uldum_game PROPERTIES OUTPUT_NAME ${ULDUM_GAME_EXE_NAME})
    target_include_directories(uldum_game PRIVATE ${ULDUM_SRC})
    target_link_libraries(uldum_game PRIVATE ${ULDUM_ENGINE_LIBS})
    if(ULDUM_SHELL_UI)
        target_link_libraries(uldum_game PRIVATE uldum_ui)
        target_compile_definitions(uldum_game PRIVATE ULDUM_SHELL_UI)
    endif()
    compile_shaders(uldum_game
        ${ULDUM_ROOT}/engine/shaders
        ${ULDUM_ENGINE_STAGING}/shaders
    )
    add_dependencies(pack_game_assets uldum_pack uldum_game_shaders)
    add_dependencies(uldum_game pack_game_assets)


else()

    # ── Desktop ENGINE build ─────────────────────────────────────────────────
    # No game project supplied. Builds engine developer tools:
    #   uldum_dev, uldum_editor (in src/editor/), uldum_server, uldum_pack.
    # Packs engine assets + every .uldmap folder under maps/ for dev runtime.
    # Does NOT produce uldum_game — that's build_game.ps1's job.

    add_executable(uldum_dev
        dev_main.cpp
        app.cpp
    )
    target_include_directories(uldum_dev PRIVATE ${ULDUM_SRC})
    target_link_libraries(uldum_dev PRIVATE ${ULDUM_ENGINE_LIBS})
    if(ULDUM_DEV_UI)
        target_sources(uldum_dev PRIVATE dev_console.cpp)
        target_link_libraries(uldum_dev PRIVATE imgui)
        target_compile_definitions(uldum_dev PRIVATE ULDUM_DEV_UI)
    endif()
    compile_shaders(uldum_dev
        ${ULDUM_ROOT}/engine/shaders
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

    # ── uldum_server — Headless dedicated server ──────────────────────────
    set(ULDUM_SERVER_LIBS
        uldum_core
        uldum_asset
        uldum_simulation
        uldum_script
        uldum_network
        uldum_map
    )
    add_executable(uldum_server server_main.cpp)
    target_include_directories(uldum_server PRIVATE ${ULDUM_SRC})
    target_link_libraries(uldum_server PRIVATE ${ULDUM_SERVER_LIBS})
    add_dependencies(uldum_server pack_engine_maps)

endif()
