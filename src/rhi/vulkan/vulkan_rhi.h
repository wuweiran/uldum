#pragma once

#include "core/types.h"

#ifdef ULDUM_PLATFORM_WINDOWS
#include <Windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan.h>
#include <vector>

namespace uldum::platform { class Platform; }

namespace uldum::rhi {

static constexpr u32 MAX_FRAMES_IN_FLIGHT = 2;

struct Config {
    bool enable_validation = true;
};

class VulkanRhi {
public:
    VulkanRhi() = default;
    ~VulkanRhi();

    bool init(const Config& config, platform::Platform& platform);
    void shutdown();

    // Returns false if swapchain is out of date (needs recreate)
    bool begin_frame();
    void end_frame();

    void handle_resize(u32 width, u32 height);

private:
    bool create_instance(const Config& config);
    bool create_surface(platform::Platform& platform);
    bool pick_physical_device();
    bool create_device();
    bool create_swapchain(u32 width, u32 height);
    void destroy_swapchain();
    bool create_command_resources();
    bool create_sync_objects();

    void record_command_buffer(VkCommandBuffer cmd, u32 image_index);

    // Instance
    VkInstance       m_instance        = VK_NULL_HANDLE;
    VkSurfaceKHR     m_surface         = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice         m_device          = VK_NULL_HANDLE;

    // Queues
    VkQueue m_graphics_queue = VK_NULL_HANDLE;
    VkQueue m_present_queue  = VK_NULL_HANDLE;
    u32     m_graphics_family = 0;
    u32     m_present_family  = 0;

    // Swapchain
    VkSwapchainKHR           m_swapchain     = VK_NULL_HANDLE;
    VkFormat                 m_swapchain_format = VK_FORMAT_UNDEFINED;
    VkExtent2D               m_swapchain_extent = {};
    std::vector<VkImage>     m_swapchain_images;
    std::vector<VkImageView> m_swapchain_views;

    // Per-frame resources
    VkCommandPool   m_command_pools[MAX_FRAMES_IN_FLIGHT]   = {};
    VkCommandBuffer m_command_buffers[MAX_FRAMES_IN_FLIGHT] = {};
    VkSemaphore     m_image_available[MAX_FRAMES_IN_FLIGHT] = {};
    VkSemaphore     m_render_finished[MAX_FRAMES_IN_FLIGHT] = {};
    VkFence         m_in_flight[MAX_FRAMES_IN_FLIGHT]       = {};

    u32  m_frame_index = 0;
    bool m_swapchain_dirty = false;

    // Debug
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
};

} // namespace uldum::rhi
