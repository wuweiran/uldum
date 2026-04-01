#include "rhi/vulkan/vulkan_rhi.h"
#include "platform/platform.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace uldum::rhi {

static constexpr const char* TAG = "RHI";

// ---------------------------------------------------------------------------
// Debug messenger callback
// ---------------------------------------------------------------------------

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user_data*/)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        log::error(TAG, "Validation: {}", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        log::warn(TAG, "Validation: {}", data->pMessage);
    }
    return VK_FALSE;
}

// ---------------------------------------------------------------------------
// Helper to load extension functions
// ---------------------------------------------------------------------------

static VkResult create_debug_messenger(VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* info,
    VkDebugUtilsMessengerEXT* messenger)
{
    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    return func ? func(instance, info, nullptr, messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static void destroy_debug_messenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger) {
    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (func) func(instance, messenger, nullptr);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

VulkanRhi::~VulkanRhi() {
    shutdown();
}

bool VulkanRhi::init(const Config& config, platform::Platform& platform) {
    if (!create_instance(config))  return false;
    if (!create_surface(platform)) return false;
    if (!pick_physical_device())   return false;
    if (!create_device())          return false;
    if (!create_allocator())       return false;
    if (!create_swapchain(platform.width(), platform.height())) return false;
    if (!create_command_resources()) return false;
    if (!create_sync_objects())    return false;
    if (!create_triangle_resources()) return false;

    log::info(TAG, "Vulkan RHI initialized");
    return true;
}

void VulkanRhi::shutdown() {
    if (m_device) vkDeviceWaitIdle(m_device);

    destroy_triangle_resources();

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_in_flight[i])       vkDestroyFence(m_device, m_in_flight[i], nullptr);
        if (m_render_finished[i]) vkDestroySemaphore(m_device, m_render_finished[i], nullptr);
        if (m_image_available[i]) vkDestroySemaphore(m_device, m_image_available[i], nullptr);
        if (m_command_pools[i])   vkDestroyCommandPool(m_device, m_command_pools[i], nullptr);
    }

    destroy_swapchain();

    if (m_allocator) { vmaDestroyAllocator(m_allocator); m_allocator = VK_NULL_HANDLE; }
    if (m_device)    { vkDestroyDevice(m_device, nullptr); m_device = VK_NULL_HANDLE; }
    if (m_surface)   { vkDestroySurfaceKHR(m_instance, m_surface, nullptr); m_surface = VK_NULL_HANDLE; }

    if (m_debug_messenger) {
        destroy_debug_messenger(m_instance, m_debug_messenger);
        m_debug_messenger = VK_NULL_HANDLE;
    }

    if (m_instance) { vkDestroyInstance(m_instance, nullptr); m_instance = VK_NULL_HANDLE; }

    log::info(TAG, "Vulkan RHI shut down");
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

bool VulkanRhi::create_instance(const Config& config) {
    VkApplicationInfo app_info{};
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName   = "Uldum";
    app_info.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app_info.pEngineName        = "Uldum Engine";
    app_info.engineVersion      = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app_info.apiVersion         = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef ULDUM_PLATFORM_WINDOWS
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
    };

    std::vector<const char*> layers;

    VkDebugUtilsMessengerCreateInfoEXT debug_info{};

    if (config.enable_validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        debug_info.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug_info.pfnUserCallback = debug_callback;
    }

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app_info;
    ci.enabledExtensionCount   = static_cast<u32>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount       = static_cast<u32>(layers.size());
    ci.ppEnabledLayerNames     = layers.data();
    if (config.enable_validation) {
        ci.pNext = &debug_info;
    }

    if (vkCreateInstance(&ci, nullptr, &m_instance) != VK_SUCCESS) {
        log::error(TAG, "Failed to create Vulkan instance");
        return false;
    }

    if (config.enable_validation) {
        create_debug_messenger(m_instance, &debug_info, &m_debug_messenger);
    }

    log::info(TAG, "Vulkan instance created (API 1.3)");
    return true;
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------

bool VulkanRhi::create_surface(platform::Platform& platform) {
#ifdef ULDUM_PLATFORM_WINDOWS
    VkWin32SurfaceCreateInfoKHR ci{};
    ci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hinstance = static_cast<HINSTANCE>(platform.native_instance_handle());
    ci.hwnd      = static_cast<HWND>(platform.native_window_handle());

    if (vkCreateWin32SurfaceKHR(m_instance, &ci, nullptr, &m_surface) != VK_SUCCESS) {
        log::error(TAG, "Failed to create Win32 Vulkan surface");
        return false;
    }
#endif

    return true;
}

// ---------------------------------------------------------------------------
// Physical device
// ---------------------------------------------------------------------------

bool VulkanRhi::pick_physical_device() {
    u32 count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) {
        log::error(TAG, "No Vulkan-capable GPU found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    // Prefer discrete GPU
    for (auto dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            m_physical_device = dev;
            log::info(TAG, "Selected GPU: {}", props.deviceName);
            return true;
        }
    }

    // Fall back to first available
    m_physical_device = devices[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_physical_device, &props);
    log::info(TAG, "Selected GPU (fallback): {}", props.deviceName);
    return true;
}

// ---------------------------------------------------------------------------
// Logical device
// ---------------------------------------------------------------------------

bool VulkanRhi::create_device() {
    // Find queue families
    u32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physical_device, &family_count, families.data());

    bool found_graphics = false;
    bool found_present  = false;

    for (u32 i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_graphics_family = i;
            found_graphics = true;
        }

        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(m_physical_device, i, m_surface, &present_support);
        if (present_support) {
            m_present_family = i;
            found_present = true;
        }

        if (found_graphics && found_present) break;
    }

    if (!found_graphics || !found_present) {
        log::error(TAG, "Required queue families not found");
        return false;
    }

    // Create queues
    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_cis;

    VkDeviceQueueCreateInfo gfx_queue_ci{};
    gfx_queue_ci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    gfx_queue_ci.queueFamilyIndex = m_graphics_family;
    gfx_queue_ci.queueCount       = 1;
    gfx_queue_ci.pQueuePriorities = &priority;
    queue_cis.push_back(gfx_queue_ci);

    if (m_present_family != m_graphics_family) {
        VkDeviceQueueCreateInfo present_queue_ci = gfx_queue_ci;
        present_queue_ci.queueFamilyIndex = m_present_family;
        queue_cis.push_back(present_queue_ci);
    }

    // Enable Vulkan 1.3 features (dynamic rendering, synchronization2)
    VkPhysicalDeviceVulkan13Features features_13{};
    features_13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features_13.dynamicRendering = VK_TRUE;
    features_13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features_13;

    std::array device_extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext                   = &features2;
    ci.queueCreateInfoCount    = static_cast<u32>(queue_cis.size());
    ci.pQueueCreateInfos       = queue_cis.data();
    ci.enabledExtensionCount   = static_cast<u32>(device_extensions.size());
    ci.ppEnabledExtensionNames = device_extensions.data();

    if (vkCreateDevice(m_physical_device, &ci, nullptr, &m_device) != VK_SUCCESS) {
        log::error(TAG, "Failed to create logical device");
        return false;
    }

    vkGetDeviceQueue(m_device, m_graphics_family, 0, &m_graphics_queue);
    vkGetDeviceQueue(m_device, m_present_family, 0, &m_present_queue);

    log::info(TAG, "Logical device created");
    return true;
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------

bool VulkanRhi::create_swapchain(u32 width, u32 height) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical_device, m_surface, &caps);

    // Pick format
    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, formats.data());

    VkSurfaceFormatKHR chosen_format = formats[0];
    for (const auto& fmt : formats) {
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB && fmt.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen_format = fmt;
            break;
        }
    }

    // Pick extent
    VkExtent2D extent;
    if (caps.currentExtent.width != std::numeric_limits<u32>::max()) {
        extent = caps.currentExtent;
    } else {
        extent.width  = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    u32 image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
        image_count = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = m_surface;
    ci.minImageCount    = image_count;
    ci.imageFormat      = chosen_format.format;
    ci.imageColorSpace  = chosen_format.colorSpace;
    ci.imageExtent      = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = m_swapchain;

    u32 families[] = {m_graphics_family, m_present_family};
    if (m_graphics_family != m_present_family) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = families;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VkSwapchainKHR old_swapchain = m_swapchain;

    if (vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain) != VK_SUCCESS) {
        log::error(TAG, "Failed to create swapchain");
        return false;
    }

    // Destroy old swapchain after creating new one
    if (old_swapchain != VK_NULL_HANDLE) {
        for (auto view : m_swapchain_views) vkDestroyImageView(m_device, view, nullptr);
        vkDestroySwapchainKHR(m_device, old_swapchain, nullptr);
    }

    m_swapchain_format = chosen_format.format;
    m_swapchain_extent = extent;

    // Get images
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, nullptr);
    m_swapchain_images.resize(image_count);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, m_swapchain_images.data());

    // Create image views
    m_swapchain_views.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        VkImageViewCreateInfo view_ci{};
        view_ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image    = m_swapchain_images[i];
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format   = m_swapchain_format;
        view_ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        view_ci.subresourceRange.baseMipLevel   = 0;
        view_ci.subresourceRange.levelCount     = 1;
        view_ci.subresourceRange.baseArrayLayer = 0;
        view_ci.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(m_device, &view_ci, nullptr, &m_swapchain_views[i]) != VK_SUCCESS) {
            log::error(TAG, "Failed to create swapchain image view");
            return false;
        }
    }

    // Create depth buffer
    {
        VkImageCreateInfo depth_ci{};
        depth_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depth_ci.imageType     = VK_IMAGE_TYPE_2D;
        depth_ci.format        = m_depth_format;
        depth_ci.extent        = {extent.width, extent.height, 1};
        depth_ci.mipLevels     = 1;
        depth_ci.arrayLayers   = 1;
        depth_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        depth_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        depth_ci.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        depth_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        if (vmaCreateImage(m_allocator, &depth_ci, &alloc_ci, &m_depth_image, &m_depth_alloc, nullptr) != VK_SUCCESS) {
            log::error(TAG, "Failed to create depth image");
            return false;
        }

        VkImageViewCreateInfo view_ci{};
        view_ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image    = m_depth_image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format   = m_depth_format;
        view_ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_ci.subresourceRange.baseMipLevel   = 0;
        view_ci.subresourceRange.levelCount     = 1;
        view_ci.subresourceRange.baseArrayLayer = 0;
        view_ci.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(m_device, &view_ci, nullptr, &m_depth_view) != VK_SUCCESS) {
            log::error(TAG, "Failed to create depth image view");
            return false;
        }
    }

    // Reset per-image fence tracking
    m_image_in_flight.assign(image_count, VK_NULL_HANDLE);

    log::info(TAG, "Swapchain created: {}x{}, {} images", extent.width, extent.height, image_count);
    return true;
}

void VulkanRhi::destroy_swapchain() {
    if (m_depth_view)  { vkDestroyImageView(m_device, m_depth_view, nullptr); m_depth_view = VK_NULL_HANDLE; }
    if (m_depth_image) { vmaDestroyImage(m_allocator, m_depth_image, m_depth_alloc); m_depth_image = VK_NULL_HANDLE; m_depth_alloc = VK_NULL_HANDLE; }

    for (auto view : m_swapchain_views) {
        vkDestroyImageView(m_device, view, nullptr);
    }
    m_swapchain_views.clear();
    m_swapchain_images.clear();

    if (m_swapchain) {
        vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
}

// ---------------------------------------------------------------------------
// Command & sync resources
// ---------------------------------------------------------------------------

bool VulkanRhi::create_command_resources() {
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkCommandPoolCreateInfo pool_ci{};
        pool_ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_ci.queueFamilyIndex = m_graphics_family;

        if (vkCreateCommandPool(m_device, &pool_ci, nullptr, &m_command_pools[i]) != VK_SUCCESS) {
            log::error(TAG, "Failed to create command pool");
            return false;
        }

        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool        = m_command_pools[i];
        alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(m_device, &alloc_info, &m_command_buffers[i]) != VK_SUCCESS) {
            log::error(TAG, "Failed to allocate command buffer");
            return false;
        }
    }
    return true;
}

bool VulkanRhi::create_sync_objects() {
    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_image_available[i]) != VK_SUCCESS ||
            vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_render_finished[i]) != VK_SUCCESS ||
            vkCreateFence(m_device, &fence_ci, nullptr, &m_in_flight[i]) != VK_SUCCESS)
        {
            log::error(TAG, "Failed to create sync objects");
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

void VulkanRhi::handle_resize(u32 width, u32 height) {
    if (width == 0 || height == 0) return;
    m_swapchain_dirty = true;
}

VkCommandBuffer VulkanRhi::begin_oneshot() {
    VkCommandPool pool = m_command_pools[0];

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool        = pool;
    alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    return cmd;
}

void VulkanRhi::end_oneshot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;

    vkQueueSubmit(m_graphics_queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_graphics_queue);

    vkFreeCommandBuffers(m_device, m_command_pools[0], 1, &cmd);
}

VkCommandBuffer VulkanRhi::begin_frame() {
    vkWaitForFences(m_device, 1, &m_in_flight[m_frame_index], VK_TRUE, UINT64_MAX);

    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
        m_image_available[m_frame_index], VK_NULL_HANDLE, &m_current_image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || m_swapchain_dirty) {
        m_swapchain_dirty = false;
        vkDeviceWaitIdle(m_device);
        create_swapchain(m_swapchain_extent.width, m_swapchain_extent.height);
        return VK_NULL_HANDLE;
    }

    // Wait if a different frame-in-flight is still using this swapchain image
    if (m_image_in_flight[m_current_image_index] != VK_NULL_HANDLE &&
        m_image_in_flight[m_current_image_index] != m_in_flight[m_frame_index]) {
        vkWaitForFences(m_device, 1, &m_image_in_flight[m_current_image_index], VK_TRUE, UINT64_MAX);
    }
    m_image_in_flight[m_current_image_index] = m_in_flight[m_frame_index];

    vkResetFences(m_device, 1, &m_in_flight[m_frame_index]);

    VkCommandBuffer cmd = m_command_buffers[m_frame_index];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    // Transition: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL (color)
    VkImageMemoryBarrier2 barriers[2]{};
    barriers[0].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    barriers[0].srcAccessMask = VK_ACCESS_2_NONE;
    barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].image         = m_swapchain_images[m_current_image_index];
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Transition: UNDEFINED → DEPTH_ATTACHMENT_OPTIMAL (depth)
    barriers[1].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    barriers[1].srcAccessMask = VK_ACCESS_2_NONE;
    barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barriers[1].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barriers[1].image         = m_depth_image;
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount  = 2;
    dep.pImageMemoryBarriers     = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);

    m_frame_active = true;
    return cmd;
}

void VulkanRhi::begin_rendering() {
    if (!m_frame_active) return;
    VkCommandBuffer cmd = m_command_buffers[m_frame_index];

    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView   = m_swapchain_views[m_current_image_index];
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue  = {{{0.1f, 0.1f, 0.1f, 1.0f}}};

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView   = m_depth_view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea           = {{0, 0}, m_swapchain_extent};
    rendering.layerCount           = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments    = &color_attachment;
    rendering.pDepthAttachment     = &depth_attachment;

    vkCmdBeginRendering(cmd, &rendering);
}

void VulkanRhi::end_frame() {
    if (!m_frame_active) return;
    m_frame_active = false;

    VkCommandBuffer cmd = m_command_buffers[m_frame_index];

    vkCmdEndRendering(cmd);

    // Transition: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC
    VkImageMemoryBarrier2 to_present{};
    to_present.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_present.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.dstStageMask  = VK_PIPELINE_STAGE_2_NONE;
    to_present.dstAccessMask = VK_ACCESS_2_NONE;
    to_present.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.image         = m_swapchain_images[m_current_image_index];
    to_present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &to_present;
    vkCmdPipelineBarrier2(cmd, &dep);

    vkEndCommandBuffer(cmd);

    // Submit
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit{};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = &m_image_available[m_frame_index];
    submit.pWaitDstStageMask    = &wait_stage;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = &m_render_finished[m_frame_index];

    vkQueueSubmit(m_graphics_queue, 1, &submit, m_in_flight[m_frame_index]);

    // Present
    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &m_render_finished[m_frame_index];
    present.swapchainCount     = 1;
    present.pSwapchains        = &m_swapchain;
    present.pImageIndices      = &m_current_image_index;

    VkResult result = vkQueuePresentKHR(m_present_queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        m_swapchain_dirty = true;
    }

    m_frame_index = (m_frame_index + 1) % MAX_FRAMES_IN_FLIGHT;
}

// ---------------------------------------------------------------------------
// VMA allocator
// ---------------------------------------------------------------------------

bool VulkanRhi::create_allocator() {
    VmaAllocatorCreateInfo ci{};
    ci.instance       = m_instance;
    ci.physicalDevice  = m_physical_device;
    ci.device          = m_device;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;

    if (vmaCreateAllocator(&ci, &m_allocator) != VK_SUCCESS) {
        log::error(TAG, "Failed to create VMA allocator");
        return false;
    }

    log::info(TAG, "VMA allocator created");
    return true;
}

// ---------------------------------------------------------------------------
// Shader loading
// ---------------------------------------------------------------------------

VkShaderModule VulkanRhi::load_shader(std::string_view path) {
    std::string path_str(path);
    std::ifstream file(path_str, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        log::error(TAG, "Failed to open shader file '{}'", path);
        return VK_NULL_HANDLE;
    }

    auto size = file.tellg();
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), size);

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const u32*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &ci, nullptr, &module) != VK_SUCCESS) {
        log::error(TAG, "Failed to create shader module from '{}'", path);
        return VK_NULL_HANDLE;
    }

    log::trace(TAG, "Loaded shader '{}'", path);
    return module;
}

// ---------------------------------------------------------------------------
// Triangle resources (Phase 5a)
// ---------------------------------------------------------------------------

bool VulkanRhi::create_triangle_resources() {
    // Load shaders
    VkShaderModule vert = load_shader("engine/shaders/triangle.vert.spv");
    VkShaderModule frag = load_shader("engine/shaders/triangle.frag.spv");
    if (!vert || !frag) {
        log::error(TAG, "Failed to load triangle shaders");
        if (vert) vkDestroyShaderModule(m_device, vert, nullptr);
        if (frag) vkDestroyShaderModule(m_device, frag, nullptr);
        return false;
    }

    // Shader stages
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // Vertex input: position (vec3) + color (vec3)
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0].location = 0;
    attrs[0].binding  = 0;
    attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset   = offsetof(Vertex, position);
    attrs[1].location = 1;
    attrs[1].binding  = 0;
    attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset   = offsetof(Vertex, color);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions    = attrs;

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport + scissor (dynamic)
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace   = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;

    // Multisampling (off)
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Color blend (no blending)
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments    = &blend_attachment;

    // Dynamic state
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates    = dynamic_states;

    // Pipeline layout (empty — no push constants or descriptors yet)
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(m_device, &layout_ci, nullptr, &m_pipeline_layout) != VK_SUCCESS) {
        log::error(TAG, "Failed to create pipeline layout");
        vkDestroyShaderModule(m_device, vert, nullptr);
        vkDestroyShaderModule(m_device, frag, nullptr);
        return false;
    }

    // Dynamic rendering format
    VkPipelineRenderingCreateInfo rendering_ci{};
    rendering_ci.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.colorAttachmentCount    = 1;
    rendering_ci.pColorAttachmentFormats = &m_swapchain_format;

    // Graphics pipeline
    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext               = &rendering_ci;
    pipeline_ci.stageCount          = 2;
    pipeline_ci.pStages             = stages;
    pipeline_ci.pVertexInputState   = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState      = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState   = &multisample;
    pipeline_ci.pColorBlendState    = &color_blend;
    pipeline_ci.pDynamicState       = &dynamic_state;
    pipeline_ci.layout              = m_pipeline_layout;

    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &m_pipeline) != VK_SUCCESS) {
        log::error(TAG, "Failed to create graphics pipeline");
        vkDestroyShaderModule(m_device, vert, nullptr);
        vkDestroyShaderModule(m_device, frag, nullptr);
        return false;
    }

    // Shader modules no longer needed after pipeline creation
    vkDestroyShaderModule(m_device, vert, nullptr);
    vkDestroyShaderModule(m_device, frag, nullptr);

    // Create vertex buffer with a colored triangle
    Vertex vertices[] = {
        {{ 0.0f, -0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},  // top — red
        {{ 0.5f,  0.5f, 0.0f}, {0.0f, 1.0f, 0.0f}},  // bottom right — green
        {{-0.5f,  0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}},  // bottom left — blue
    };

    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size  = sizeof(vertices);
    buf_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    if (vmaCreateBuffer(m_allocator, &buf_ci, &alloc_ci, &m_vertex_buffer, &m_vertex_alloc, nullptr) != VK_SUCCESS) {
        log::error(TAG, "Failed to create vertex buffer");
        return false;
    }

    // Upload vertex data
    void* mapped = nullptr;
    vmaMapMemory(m_allocator, m_vertex_alloc, &mapped);
    memcpy(mapped, vertices, sizeof(vertices));
    vmaUnmapMemory(m_allocator, m_vertex_alloc);

    log::info(TAG, "Triangle pipeline and vertex buffer created");
    return true;
}

void VulkanRhi::destroy_triangle_resources() {
    if (m_vertex_buffer) { vmaDestroyBuffer(m_allocator, m_vertex_buffer, m_vertex_alloc); m_vertex_buffer = VK_NULL_HANDLE; }
    if (m_pipeline)      { vkDestroyPipeline(m_device, m_pipeline, nullptr); m_pipeline = VK_NULL_HANDLE; }
    if (m_pipeline_layout) { vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr); m_pipeline_layout = VK_NULL_HANDLE; }
}

// ---------------------------------------------------------------------------
// Command recording
// ---------------------------------------------------------------------------

void VulkanRhi::record_command_buffer(VkCommandBuffer cmd, u32 image_index) {
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    // Transition: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
    VkImageMemoryBarrier2 to_render{};
    to_render.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_render.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    to_render.srcAccessMask = VK_ACCESS_2_NONE;
    to_render.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_render.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_render.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    to_render.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_render.image         = m_swapchain_images[image_index];
    to_render.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount  = 1;
    dep.pImageMemoryBarriers     = &to_render;
    vkCmdPipelineBarrier2(cmd, &dep);

    // Dynamic rendering — clear to dark gray
    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView   = m_swapchain_views[image_index];
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue  = {{{0.1f, 0.1f, 0.1f, 1.0f}}};

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea           = {{0, 0}, m_swapchain_extent};
    rendering.layerCount           = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments    = &color_attachment;

    vkCmdBeginRendering(cmd, &rendering);

    // Draw triangle
    if (m_pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

        VkViewport viewport{};
        viewport.width    = static_cast<float>(m_swapchain_extent.width);
        viewport.height   = static_cast<float>(m_swapchain_extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{{0, 0}, m_swapchain_extent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_vertex_buffer, &offset);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    vkCmdEndRendering(cmd);

    // Transition: COLOR_ATTACHMENT_OPTIMAL → PRESENT_SRC
    VkImageMemoryBarrier2 to_present{};
    to_present.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_present.srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.dstStageMask  = VK_PIPELINE_STAGE_2_NONE;
    to_present.dstAccessMask = VK_ACCESS_2_NONE;
    to_present.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.image         = m_swapchain_images[image_index];
    to_present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep2{};
    dep2.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep2.imageMemoryBarrierCount = 1;
    dep2.pImageMemoryBarriers    = &to_present;
    vkCmdPipelineBarrier2(cmd, &dep2);

    vkEndCommandBuffer(cmd);
}

} // namespace uldum::rhi
