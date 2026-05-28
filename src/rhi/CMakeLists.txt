add_library(uldum_rhi STATIC)

target_include_directories(uldum_rhi PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_rhi PUBLIC uldum_core uldum_platform)

if(ULDUM_BACKEND_VULKAN)
    target_sources(uldum_rhi PRIVATE
        vulkan/vulkan_rhi.cpp
        vulkan/command_list.cpp
    )
    target_link_libraries(uldum_rhi PUBLIC Vulkan::Vulkan vma)
endif()

if(ULDUM_BACKEND_GLES)
    target_sources(uldum_rhi PRIVATE
        gles/gles_rhi.cpp
        gles/command_list.cpp
    )
    # Android NDK ships libGLESv3 (and the v2-named superset) + libEGL.
    target_link_libraries(uldum_rhi PUBLIC GLESv3 EGL)
endif()
