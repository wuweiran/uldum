add_library(uldum_script STATIC
    script.cpp
)

target_include_directories(uldum_script PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_script PUBLIC uldum_core uldum_simulation uldum_map uldum_audio uldum_hud uldum_i18n lua54 sol2 glm::glm)

if(MSVC)
    target_compile_options(uldum_script PRIVATE /bigobj)
endif()

# Lua syntax and semantic checker — dev/editor tooling only. Deliberately a
# separate library from uldum_script and never linked into shipping targets.
add_library(uldum_scriptcheck STATIC
    script_check.cpp
)
target_include_directories(uldum_scriptcheck PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_scriptcheck PUBLIC lua54)
