#pragma once

#include "core/types.h"

#ifdef ULDUM_PLATFORM_WINDOWS
#include <Windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <string_view>
#include <vector>

namespace uldum::platform { class Platform; }

namespace uldum::rhi {

static constexpr u32 MAX_FRAMES_IN_FLIGHT = 2;

struct Config {
    bool enable_validation = true;
};

// Simple vertex format for Phase 5a
struct Vertex {
    float position[3];
    float color[3];
};

class VulkanRhi {
public:
    VulkanRhi() = default;
    ~VulkanRhi();

    bool init(const Config& config, platform::Platform& platform);
    void shutdown();

    // begin_frame acquires swapchain image, returns command buffer to record into.
    // Returns null if swapchain needs recreate (caller should skip the frame).
    VkCommandBuffer begin_frame();
    void end_frame();

    void handle_resize(u32 width, u32 height);

    // Accessors for renderer
    VkDevice         device()    const { return m_device; }
    VkPhysicalDevice physical_device() const { return m_physical_device; }
    VmaAllocator     allocator() const { return m_allocator; }
    VkExtent2D       extent()    const { return m_swapchain_extent; }
    VkFormat         swapchain_format() const { return m_swapchain_format; }
    VkFormat         depth_format()     const { return m_depth_format; }
    u32              current_image_index() const { return m_current_image_index; }
    VkImageView      current_image_view() const { return m_swapchain_views[m_current_image_index]; }
    VkImage          current_image()      const { return m_swapchain_images[m_current_image_index]; }
    VkQueue          graphics_queue() const { return m_graphics_queue; }
    u32              graphics_family() const { return m_graphics_family; }

    // One-shot command buffer for immediate GPU work (texture uploads, etc.)
    // Call begin, record commands, then end (submits and waits).
    VkCommandBuffer begin_oneshot();
    void            end_oneshot(VkCommandBuffer cmd);

private:
    bool create_instance(const Config& config);
    bool create_surface(platform::Platform& platform);
    bool pick_physical_device();
    bool create_device();
    bool create_allocator();
    bool create_swapchain(u32 width, u32 height);
    void destroy_swapchain();
    bool create_command_resources();
    bool create_sync_objects();
    bool create_triangle_resources();
    void destroy_triangle_resources();

    VkShaderModule load_shader(std::string_view path);

    void record_command_buffer(VkCommandBuffer cmd, u32 image_index);

    // Instance
    VkInstance       m_instance        = VK_NULL_HANDLE;
    VkSurfaceKHR     m_surface         = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
    VkDevice         m_device          = VK_NULL_HANDLE;
    VmaAllocator     m_allocator       = VK_NULL_HANDLE;

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

    // Depth buffer
    VkImage       m_depth_image  = VK_NULL_HANDLE;
    VmaAllocation m_depth_alloc  = VK_NULL_HANDLE;
    VkImageView   m_depth_view   = VK_NULL_HANDLE;
    VkFormat      m_depth_format = VK_FORMAT_D32_SFLOAT;

    // Per-frame resources
    VkCommandPool   m_command_pools[MAX_FRAMES_IN_FLIGHT]   = {};
    VkCommandBuffer m_command_buffers[MAX_FRAMES_IN_FLIGHT] = {};
    VkSemaphore     m_image_available[MAX_FRAMES_IN_FLIGHT] = {};
    VkSemaphore     m_render_finished[MAX_FRAMES_IN_FLIGHT] = {};
    VkFence         m_in_flight[MAX_FRAMES_IN_FLIGHT]       = {};

    u32  m_frame_index = 0;
    u32  m_current_image_index = 0;
    bool m_swapchain_dirty = false;
    bool m_frame_active = false;

    // Per-swapchain-image: which in-flight fence was last submitted with this image.
    // Used to avoid writing to an image that a previous frame is still rendering to.
    std::vector<VkFence> m_image_in_flight;

    // Triangle resources (Phase 5a)
    VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline        = VK_NULL_HANDLE;
    VkBuffer         m_vertex_buffer   = VK_NULL_HANDLE;
    VmaAllocation    m_vertex_alloc    = VK_NULL_HANDLE;

    // Debug
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
};

} // namespace uldum::rhi
