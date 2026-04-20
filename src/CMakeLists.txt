# Shared libraries (all platforms)
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(rhi)
add_subdirectory(asset)
add_subdirectory(render)
add_subdirectory(audio)
add_subdirectory(script)
add_subdirectory(simulation)
add_subdirectory(network)
add_subdirectory(input)
add_subdirectory(map)

# Executables / shared-library entry point
add_subdirectory(app)

# Desktop-only: editor (ImGui, Win32 file dialogs) and the uldum_pack host tool.
if(NOT ANDROID)
    add_subdirectory(editor)
    add_subdirectory(tools)
endif()
