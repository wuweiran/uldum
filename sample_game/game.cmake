# sample_game build manifest — included by src/app/CMakeLists.txt when
# ULDUM_GAME_PROJECT_DIR points here. Companion to game.json: JSON is
# runtime config, game.cmake is build config. Variables set:
#   ULDUM_GAME_SOURCES        .cpp files added to the engine target
#   ULDUM_GAME_INCLUDE_DIRS   include paths added to the engine target
#   ULDUM_APP_HEADER          relative include path the engine #includes
#   ULDUM_APP_CLASS           class engine.cpp instantiates as the App
#
# CMAKE_CURRENT_LIST_DIR resolves to this file's directory, so paths
# are project-relative without needing to know the checkout location.

set(ULDUM_GAME_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/sample_game_app.cpp
)
set(ULDUM_GAME_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/src
)
set(ULDUM_APP_HEADER "sample_game_app.h")
set(ULDUM_APP_CLASS SampleGameApp)
