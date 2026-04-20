add_library(uldum_rhi STATIC
    vulkan/vulkan_rhi.cpp
)

target_include_directories(uldum_rhi PUBLIC ${ULDUM_SRC})
target_link_libraries(uldum_rhi PUBLIC uldum_core uldum_platform Vulkan::Vulkan vma)
