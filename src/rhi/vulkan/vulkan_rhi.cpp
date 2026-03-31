#include "rhi/vulkan/vulkan_rhi.h"
#include "platform/platform.h"
#include "core/log.h"

#include <algorithm>
#include <array>
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
    if (!create_swapchain(platform.width(), platform.height())) return false;
    if (!create_command_resources()) return false;
    if (!create_sync_objects())    return false;

    log::info(TAG, "Vulkan RHI initialized");
    return true;
}

void VulkanRhi::shutdown() {
    if (m_device) vkDeviceWaitIdle(m_device);

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_in_flight[i])       vkDestroyFence(m_device, m_in_flight[i], nullptr);
        if (m_render_finished[i]) vkDestroySemaphore(m_device, m_render_finished[i], nullptr);
        if (m_image_available[i]) vkDestroySemaphore(m_device, m_image_available[i], nullptr);
        if (m_command_pools[i])   vkDestroyCommandPool(m_device, m_command_pools[i], nullptr);
    }

    destroy_swapchain();

    if (m_device)  { vkDestroyDevice(m_device, nullptr);   m_device = VK_NULL_HANDLE; }
    if (m_surface) { vkDestroySurfaceKHR(m_instance, m_surface, nullptr); m_surface = VK_NULL_HANDLE; }

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

    log::info(TAG, "Swapchain created: {}x{}, {} images", extent.width, extent.height, image_count);
    return true;
}

void VulkanRhi::destroy_swapchain() {
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

bool VulkanRhi::begin_frame() {
    vkWaitForFences(m_device, 1, &m_in_flight[m_frame_index], VK_TRUE, UINT64_MAX);

    u32 image_index = 0;
    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
        m_image_available[m_frame_index], VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || m_swapchain_dirty) {
        m_swapchain_dirty = false;
        vkDeviceWaitIdle(m_device);
        create_swapchain(m_swapchain_extent.width, m_swapchain_extent.height);
        return false;
    }

    vkResetFences(m_device, 1, &m_in_flight[m_frame_index]);

    VkCommandBuffer cmd = m_command_buffers[m_frame_index];
    vkResetCommandBuffer(cmd, 0);

    record_command_buffer(cmd, image_index);

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
    present.pImageIndices      = &image_index;

    result = vkQueuePresentKHR(m_present_queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        m_swapchain_dirty = true;
    }

    m_frame_index = (m_frame_index + 1) % MAX_FRAMES_IN_FLIGHT;
    return true;
}

void VulkanRhi::end_frame() {
    // Currently a no-op — future: ImGui rendering, profiling end, etc.
}

// ---------------------------------------------------------------------------
// Command recording (Phase 1: just clear to a color)
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

    // Dynamic rendering — clear to cornflower blue
    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView   = m_swapchain_views[image_index];
    color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.clearValue  = {{{0.392f, 0.584f, 0.929f, 1.0f}}};

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea           = {{0, 0}, m_swapchain_extent};
    rendering.layerCount           = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments    = &color_attachment;

    vkCmdBeginRendering(cmd, &rendering);
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
