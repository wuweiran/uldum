# Server-side binaries: the orchestrator (`uldum_server`) and the
# per-session worker (`uldum_worker`). Both are headless — no renderer,
# no audio, no window — so they only link the subset of engine libs that
# don't pull in Vulkan / rhi / vma / freetype.

if(ANDROID)
    # Server-side binaries are desktop-only. Android ships clients.
    return()
endif()

set(ULDUM_WORKER_LIBS
    uldum_core
    uldum_asset
    uldum_i18n
    uldum_simulation
    uldum_script
    uldum_network
    uldum_map
)

# uldum_worker — one process per active game session.
add_executable(uldum_worker worker_main.cpp)
target_include_directories(uldum_worker PRIVATE ${ULDUM_SRC})
target_link_libraries(uldum_worker PRIVATE ${ULDUM_WORKER_LIBS} nlohmann_json::nlohmann_json)
# Workers need engine assets. pack_engine_maps lives in src/app/ under
# the desktop ENGINE build branch; only depend on it there.
if(NOT ULDUM_GAME_PROJECT_DIR AND TARGET pack_engine_maps)
    add_dependencies(uldum_worker pack_engine_maps)
endif()

# uldum_server — orchestrator. HTTP API for game backends, fork+execs
# workers on demand, dispatches game-end webhooks. Small dep surface —
# core for log/types, asset for reading the .uldmap manifest to learn
# how many tokens to mint per session, httplib for the HTTP server,
# nlohmann_json for request / response parsing. No engine-side
# simulation / network code here; the worker owns all of that.
add_executable(uldum_server server_main.cpp)
target_include_directories(uldum_server PRIVATE ${ULDUM_SRC})
target_link_libraries(uldum_server PRIVATE
    uldum_core uldum_asset
    httplib nlohmann_json::nlohmann_json)
if(WIN32)
    # cpp-httplib needs winsock on Windows.
    target_link_libraries(uldum_server PRIVATE ws2_32)
endif()
