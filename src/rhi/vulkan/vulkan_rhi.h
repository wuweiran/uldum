#pragma once

#include "core/types.h"

#ifdef ULDUM_PLATFORM_WINDOWS
#include <Windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#ifdef ULDUM_PLATFORM_ANDROID
#define VK_USE_PLATFORM_ANDROID_KHR
#endif

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
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

    // begin_frame acquires swapchain image, returns command buffer to record into.
    // Returns null if swapchain needs recreate (caller should skip the frame).
    // Call begin_rendering() after any pre-render passes (e.g. shadow) to start the main pass.
    VkCommandBuffer begin_frame();
    void begin_rendering();  // starts main color+depth rendering scope
    void end_frame();

    void handle_resize(u32 width, u32 height);

    // Android lifecycle plumbing: called when the ANativeWindow the
    // surface was bound to has been replaced (foreground → background →
    // foreground, or rotation). Destroys the old VkSurfaceKHR +
    // swapchain and builds new ones against the platform's current
    // native window handle. Safe to call when the RHI has never been
    // initialized (early return). Caller must only invoke after the
    // platform reports a non-null native_window_handle().
    void recreate_surface(platform::Platform& platform);

    // Opaque pointer to the native window (ANativeWindow* / HWND) the
    // current VkSurfaceKHR is bound to. Used by the main loop to detect
    // a window handoff and trigger `recreate_surface` exactly when the
    // platform gives us a new window, not every frame.
    void* native_window_handle() const { return m_native_window; }

    // Accessors for renderer
    VkInstance       instance()  const { return m_instance; }
    VkDevice         device()    const { return m_device; }
    VkPhysicalDevice physical_device() const { return m_physical_device; }
    VmaAllocator     allocator() const { return m_allocator; }
    VkExtent2D       extent()    const { return m_swapchain_extent; }
    VkFormat         swapchain_format() const { return m_swapchain_format; }
    VkFormat         depth_format()     const { return m_depth_format; }
    VkSampleCountFlagBits msaa_samples() const { return m_msaa_samples; }
    u32              current_image_index() const { return m_current_image_index; }
    // Ring-buffer index for resources that are written per-frame but read by
    // the GPU for up to MAX_FRAMES_IN_FLIGHT frames. Stable inside one frame;
    // advances in end_frame(). Callers can allocate MAX_FRAMES_IN_FLIGHT
    // copies of a resource and index by this value to avoid CPU/GPU races.
    u32              frame_index()         const { return m_frame_index; }
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

    // Instance
    VkInstance       m_instance        = VK_NULL_HANDLE;
    VkSurfaceKHR     m_surface         = VK_NULL_HANDLE;
    // Cached native window pointer the current surface was created
    // against. Opaque (HWND on Win32, ANativeWindow* on Android).
    // Compared against the platform's latest handle to detect
    // Android's post-background surface handoff.
    void*            m_native_window   = nullptr;
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

    // MSAA color target (4x, transient — resolves to swapchain image)
    VkImage       m_msaa_color_image = VK_NULL_HANDLE;
    VmaAllocation m_msaa_color_alloc = VK_NULL_HANDLE;
    VkImageView   m_msaa_color_view  = VK_NULL_HANDLE;
    VkSampleCountFlagBits m_msaa_samples = VK_SAMPLE_COUNT_4_BIT;

    // Depth buffer (matches MSAA sample count)
    VkImage       m_depth_image  = VK_NULL_HANDLE;
    VmaAllocation m_depth_alloc  = VK_NULL_HANDLE;
    VkImageView   m_depth_view   = VK_NULL_HANDLE;
    VkFormat      m_depth_format = VK_FORMAT_D32_SFLOAT;

    // Per-frame resources
    VkCommandPool   m_command_pools[MAX_FRAMES_IN_FLIGHT]   = {};
    VkCommandBuffer m_command_buffers[MAX_FRAMES_IN_FLIGHT] = {};
    VkSemaphore     m_image_available[MAX_FRAMES_IN_FLIGHT] = {};
    VkFence         m_in_flight[MAX_FRAMES_IN_FLIGHT]       = {};

    // Per-swapchain-image semaphore for present — avoids reuse while present engine holds it
    std::vector<VkSemaphore> m_render_finished;

    u32  m_frame_index = 0;
    u32  m_current_image_index = 0;
    bool m_swapchain_dirty = false;
    bool m_frame_active = false;

    // Per-swapchain-image: which in-flight fence was last submitted with this image.
    // Used to avoid writing to an image that a previous frame is still rendering to.
    std::vector<VkFence> m_image_in_flight;

    // Debug
    VkDebugUtilsMessengerEXT m_debug_messenger = VK_NULL_HANDLE;
};

} // namespace uldum::rhi
