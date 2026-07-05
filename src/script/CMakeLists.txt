add_library(uldum_script STATIC
    script.cpp
)

target_include_directories(uldum_script PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_script PUBLIC uldum_core uldum_simulation uldum_map uldum_audio uldum_hud uldum_i18n lua54 sol2 glm::glm)

if(MSVC)
    target_compile_options(uldum_script PRIVATE /bigobj)
endif()

# Tier-1 Lua syntax checker — dev/editor tooling only. Deliberately a SEPARATE
# library from uldum_script: it depends on lua54 alone (no sol2, no engine
# libs), and is linked ONLY into uldum_dev and uldum_editor (see their
# CMakeLists), never uldum_game / the Android game flavor. Keeping it out of
# uldum_script guarantees the shipping game can't pull it in — "runtime free".
add_library(uldum_scriptcheck STATIC
    script_check.cpp
)
target_include_directories(uldum_scriptcheck PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_scriptcheck PUBLIC lua54)
