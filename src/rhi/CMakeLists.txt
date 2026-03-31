add_library(uldum_rhi STATIC
    vulkan/vulkan_rhi.cpp
)

target_include_directories(uldum_rhi PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(uldum_rhi PUBLIC uldum_core uldum_platform Vulkan::Vulkan)
