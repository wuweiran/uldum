# Shared libraries
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(rhi)
add_subdirectory(asset)
add_subdirectory(render)
add_subdirectory(audio)
add_subdirectory(script)
add_subdirectory(simulation)
add_subdirectory(network)
add_subdirectory(map)
add_subdirectory(editor)

# Executables
add_subdirectory(app)
# editor/ builds uldum_editor.exe directly
