#include "rhi/vulkan/vulkan_rhi.h"
#include "rhi/detail/slot_table.h"
#include "platform/platform.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
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

Rhi::~Rhi() {
    shutdown();
}

bool Rhi::init(const Config& config, platform::Platform& platform) {
    if (!create_instance(config))  return false;
    if (!create_surface(platform)) return false;
    if (!pick_physical_device())   return false;
    if (!create_device())          return false;
    if (!create_allocator())       return false;
    if (!create_swapchain(platform.width(), platform.height())) return false;
    if (!create_command_resources()) return false;
    if (!create_sync_objects())    return false;

    log::info(TAG, "Vulkan RHI initialized");
    return true;
}

void Rhi::shutdown() {
    if (m_device) vkDeviceWaitIdle(m_device);

    for (auto& sem : m_render_finished) {
        if (sem) vkDestroySemaphore(m_device, sem, nullptr);
    }
    m_render_finished.clear();

    // Idempotent: zero each handle after destroying so a second shutdown
    // (App::shutdown() runs explicitly, then ~App → ~Rhi calls
    // shutdown() again) doesn't re-feed stale handles into vkDestroy*
    // with a NULL device.
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_in_flight[i])       { vkDestroyFence(m_device, m_in_flight[i], nullptr);            m_in_flight[i] = VK_NULL_HANDLE; }
        if (m_image_available[i]) { vkDestroySemaphore(m_device, m_image_available[i], nullptr);  m_image_available[i] = VK_NULL_HANDLE; }
        if (m_command_pools[i])   { vkDestroyCommandPool(m_device, m_command_pools[i], nullptr);  m_command_pools[i] = VK_NULL_HANDLE; }
    }
    if (m_oneshot_fence) { vkDestroyFence(m_device, m_oneshot_fence, nullptr); m_oneshot_fence = VK_NULL_HANDLE; }
    if (m_oneshot_pool)  { vkDestroyCommandPool(m_device, m_oneshot_pool, nullptr); m_oneshot_pool = VK_NULL_HANDLE; }

    destroy_swapchain();

    // Safety-net teardown for any handles a caller leaked. Destroy in
    // dependency order: pipelines / layouts / descriptors first, then
    // samplers and textures (which free `vk*`), before VMA destruction.
    for (auto& rec : m_pipeline_records) {
        if (rec.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(m_device, rec.pipeline, nullptr);
            rec.pipeline = VK_NULL_HANDLE;
        }
    }
    m_pipeline_records.clear();
    m_pipeline_free.clear();

    for (auto& rec : m_pl_records) {
        if (rec.layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, rec.layout, nullptr);
            rec.layout = VK_NULL_HANDLE;
        }
    }
    m_pl_records.clear();
    m_pl_free.clear();

    // Descriptor sets are owned by their pool — freeing the pool below
    // releases all sets. We don't need to free individually here.
    m_dset_records.clear();
    m_dset_free.clear();

    for (auto pool : m_descriptor_pools) {
        if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, pool, nullptr);
    }
    m_descriptor_pools.clear();

    for (auto& rec : m_dsl_records) {
        if (rec.layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device, rec.layout, nullptr);
            rec.layout = VK_NULL_HANDLE;
        }
    }
    m_dsl_records.clear();
    m_dsl_free.clear();

    for (auto& rec : m_samplers) {
        if (rec.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(m_device, rec.sampler, nullptr);
            rec.sampler = VK_NULL_HANDLE;
        }
    }
    m_samplers.clear();
    m_samplers_free.clear();

    for (auto& rec : m_textures) {
        if (rec.view  != VK_NULL_HANDLE) vkDestroyImageView(m_device, rec.view, nullptr);
        if (rec.image != VK_NULL_HANDLE) vmaDestroyImage(m_allocator, rec.image, rec.alloc);
        rec.view = VK_NULL_HANDLE; rec.image = VK_NULL_HANDLE; rec.alloc = VK_NULL_HANDLE;
    }
    m_textures.clear();
    m_textures_free.clear();

    for (auto& rec : m_buffers) {
        if (rec.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(m_allocator, rec.buffer, rec.alloc);
        rec.buffer = VK_NULL_HANDLE; rec.alloc = VK_NULL_HANDLE; rec.mapped = nullptr;
    }
    m_buffers.clear();
    m_buffers_free.clear();

    for (auto& rec : m_shader_modules) {
        if (rec.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, rec.module, nullptr);
            rec.module = VK_NULL_HANDLE;
        }
    }
    m_shader_modules.clear();
    m_shader_modules_free.clear();

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

bool Rhi::create_instance(const Config& config) {
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
#elif defined(ULDUM_PLATFORM_ANDROID)
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
#endif
    };

    std::vector<const char*> layers;

    VkDebugUtilsMessengerCreateInfoEXT debug_info{};

    // Validation layers on Android require the developer to drop
    // libVkLayer_khronos_validation.so into platforms/android/app/src/main/jniLibs/<abi>/
    // (the SDK/NDK does not ship them in the APK). When present, the APK-
    // packaged .so is loaded by the Android Vulkan loader automatically and
    // VK_LAYER_KHRONOS_validation can be enabled in the usual way.
    //
    // Probe for the layer before requesting it — if the user hasn't bundled
    // it, we still want the engine to launch (sans validation) rather than
    // failing vkCreateInstance with VK_ERROR_LAYER_NOT_PRESENT.
    bool want_validation = config.enable_validation;
    if (want_validation) {
        u32 layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> available(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, available.data());
        bool found = false;
        for (const auto& l : available) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                found = true; break;
            }
        }
        if (!found) {
            log::warn(TAG, "VK_LAYER_KHRONOS_validation not available — install Vulkan SDK "
                           "or bundle libVkLayer_khronos_validation.so in jniLibs. Continuing "
                           "without validation.");
            want_validation = false;
        }
    }

    if (want_validation) {
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
    if (want_validation) {
        ci.pNext = &debug_info;
    }

    {
        VkResult rc = vkCreateInstance(&ci, nullptr, &m_instance);
        if (rc != VK_SUCCESS) {
            log::error(TAG, "vkCreateInstance failed: VkResult {}", static_cast<int>(rc));
            log::error(TAG, "  API: 1.3  |  extensions: {}  |  layers: {}",
                       extensions.size(), layers.size());
            for (auto* e : extensions) log::error(TAG, "    ext: {}", e);
            for (auto* l : layers)     log::error(TAG, "    layer: {}", l);
            return false;
        }
    }

    if (want_validation) {
        create_debug_messenger(m_instance, &debug_info, &m_debug_messenger);
    }

    log::info(TAG, "Vulkan instance created (API 1.3)");
    return true;
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------

bool Rhi::create_surface(platform::Platform& platform) {
#ifdef ULDUM_PLATFORM_WINDOWS
    VkWin32SurfaceCreateInfoKHR ci{};
    ci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    ci.hinstance = static_cast<HINSTANCE>(platform.native_instance_handle());
    ci.hwnd      = static_cast<HWND>(platform.native_window_handle());

    if (vkCreateWin32SurfaceKHR(m_instance, &ci, nullptr, &m_surface) != VK_SUCCESS) {
        log::error(TAG, "Failed to create Win32 Vulkan surface");
        return false;
    }
#elif defined(ULDUM_PLATFORM_ANDROID)
    ANativeWindow* window = static_cast<ANativeWindow*>(platform.native_window_handle());
    if (!window) {
        log::error(TAG, "Android surface creation requested but no ANativeWindow — "
                        "APP_CMD_INIT_WINDOW hasn't fired yet");
        return false;
    }
    VkAndroidSurfaceCreateInfoKHR ci{};
    ci.sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = window;
    if (vkCreateAndroidSurfaceKHR(m_instance, &ci, nullptr, &m_surface) != VK_SUCCESS) {
        log::error(TAG, "Failed to create Android Vulkan surface");
        return false;
    }
#endif

    m_native_window = platform.native_window_handle();
    return true;
}

void Rhi::wait_idle() {
    if (m_device != VK_NULL_HANDLE) vkDeviceWaitIdle(m_device);
}

void Rhi::recreate_surface(platform::Platform& platform) {
    if (m_device == VK_NULL_HANDLE) return;  // not initialized yet

    // The old swapchain and surface are tied to a now-destroyed
    // ANativeWindow. Drain any in-flight work before tearing them down
    // so we don't free resources the GPU is still using; then rebuild
    // bottom-up against the new native window.
    vkDeviceWaitIdle(m_device);
    destroy_swapchain();
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    m_native_window = nullptr;

    if (!create_surface(platform)) {
        log::error(TAG, "recreate_surface: create_surface failed — rendering will stay black until the next resume");
        return;
    }
    // Rebuild sync primitives that may have been signaled against the
    // old surface's present engine. Semaphore is the only one with a
    // pairing to present; fences are wholly CPU-side.
    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        vkDestroySemaphore(m_device, m_image_available[i], nullptr);
        VkSemaphoreCreateInfo sem_ci{};
        sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_image_available[i]);
    }

    if (!create_swapchain(platform.width(), platform.height())) {
        log::error(TAG, "recreate_surface: swapchain creation failed");
        return;
    }
    log::info(TAG, "Surface + swapchain recreated after window handoff");
}

// ---------------------------------------------------------------------------
// Physical device
// ---------------------------------------------------------------------------

bool Rhi::pick_physical_device() {
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

bool Rhi::create_device() {
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

    // Enable Vulkan 1.2 features (descriptor indexing for bindless textures)
    VkPhysicalDeviceVulkan12Features features_12{};
    features_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features_12.pNext = &features_13;
    features_12.descriptorIndexing                         = VK_TRUE;
    features_12.shaderSampledImageArrayNonUniformIndexing  = VK_TRUE;
    features_12.runtimeDescriptorArray                     = VK_TRUE;
    features_12.descriptorBindingPartiallyBound            = VK_TRUE;
    features_12.descriptorBindingVariableDescriptorCount   = VK_TRUE;
    features_12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features_12;
    // Renderer batches per-model draws into a single indirect call
    // with drawCount = unique-model count. Without this feature, any
    // scene with two distinct visible models (e.g. archer + grunt)
    // trips a Vulkan validation error and undefined behavior.
    features2.features.multiDrawIndirect = VK_TRUE;

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

bool Rhi::create_swapchain(u32 width, u32 height) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical_device, m_surface, &caps);

    // Pick format. We need an sRGB variant so the hardware applies the
    // gamma curve when our shaders write lit (linear-space) colors. Without
    // it, midtones get crushed and everything looks dark — common on Adreno,
    // which often exposes R8G8B8A8_SRGB rather than B8G8R8A8_SRGB as the
    // first sRGB entry in the list.
    u32 format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical_device, m_surface, &format_count, formats.data());

    VkSurfaceFormatKHR chosen_format = formats[0];
    bool picked_srgb = false;
    for (const auto& fmt : formats) {
        if (fmt.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (fmt.format == VK_FORMAT_B8G8R8A8_SRGB ||
            fmt.format == VK_FORMAT_R8G8B8A8_SRGB) {
            chosen_format = fmt;
            picked_srgb = true;
            break;
        }
    }
    if (!picked_srgb) {
        log::warn(TAG, "No sRGB swapchain format available (first format: {}); "
                       "colors will render without gamma correction",
                  static_cast<int>(chosen_format.format));
    }

    // Pick extent — skip if zero (window minimized or during display switch)
    VkExtent2D extent;
    if (caps.currentExtent.width != std::numeric_limits<u32>::max()) {
        extent = caps.currentExtent;
    } else {
        extent.width  = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        m_swapchain_extent = {0, 0};
        return true;  // defer until window has a valid size
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

    // preTransform = IDENTITY means "I render in the native surface orientation,
    // compositor please rotate my output to match the display." Using
    // currentTransform instead would require us to pre-rotate in the view
    // matrix + swap extent W/H — faster on mobile (no compositor blit) but
    // more complex. IDENTITY is correct and slightly slower; revisit if
    // Android perf needs the optimization.
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        ci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    } else {
        ci.preTransform = caps.currentTransform;
    }
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO (vsync, always supported) when vsync is on; otherwise prefer
    // MAILBOX (uncapped, low latency) and fall back to FIFO if the device
    // doesn't offer it. m_prefer_vsync is runtime-togglable via set_vsync().
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!m_prefer_vsync) {
        u32 mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical_device, m_surface, &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical_device, m_surface, &mode_count, modes.data());
        bool found_mailbox = false;
        for (auto mode : modes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
                found_mailbox = true;
                break;
            }
        }
        if (!found_mailbox) {
            log::info(TAG, "VSync off requested but MAILBOX unavailable; using FIFO");
        }
    }
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

    // Destroy old swapchain + size-coupled attachments after creating
    // the new swapchain. The MSAA color and depth images are sized
    // against the swapchain extent, so a resize re-creates them
    // below — without freeing the old pair here, every resize leaks
    // a dedicated VMA allocation and shutdown trips
    // "Unfreed dedicated allocations found!".
    if (old_swapchain != VK_NULL_HANDLE) {
        for (auto view : m_swapchain_views) vkDestroyImageView(m_device, view, nullptr);
        vkDestroySwapchainKHR(m_device, old_swapchain, nullptr);
        if (m_msaa_color_view) {
            vkDestroyImageView(m_device, m_msaa_color_view, nullptr);
            m_msaa_color_view = VK_NULL_HANDLE;
        }
        if (m_msaa_color_image) {
            vmaDestroyImage(m_allocator, m_msaa_color_image, m_msaa_color_alloc);
            m_msaa_color_image = VK_NULL_HANDLE;
            m_msaa_color_alloc = VK_NULL_HANDLE;
        }
        if (m_depth_view) {
            vkDestroyImageView(m_device, m_depth_view, nullptr);
            m_depth_view = VK_NULL_HANDLE;
        }
        if (m_depth_image) {
            vmaDestroyImage(m_allocator, m_depth_image, m_depth_alloc);
            m_depth_image = VK_NULL_HANDLE;
            m_depth_alloc = VK_NULL_HANDLE;
        }
    }

    m_swapchain_format = chosen_format.format;
    m_swapchain_extent = extent;

    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, nullptr);
    m_swapchain_images.resize(image_count);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &image_count, m_swapchain_images.data());

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

    // Create MSAA color target (transient — GPU may use on-chip memory)
    {
        VkImageCreateInfo color_ci{};
        color_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        color_ci.imageType     = VK_IMAGE_TYPE_2D;
        color_ci.format        = m_swapchain_format;
        color_ci.extent        = {extent.width, extent.height, 1};
        color_ci.mipLevels     = 1;
        color_ci.arrayLayers   = 1;
        color_ci.samples       = m_msaa_samples;
        color_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        color_ci.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        color_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        alloc_ci.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;

        if (vmaCreateImage(m_allocator, &color_ci, &alloc_ci, &m_msaa_color_image, &m_msaa_color_alloc, nullptr) != VK_SUCCESS) {
            log::error(TAG, "Failed to create MSAA color image");
            return false;
        }

        VkImageViewCreateInfo view_ci{};
        view_ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image    = m_msaa_color_image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format   = m_swapchain_format;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        if (vkCreateImageView(m_device, &view_ci, nullptr, &m_msaa_color_view) != VK_SUCCESS) {
            log::error(TAG, "Failed to create MSAA color image view");
            return false;
        }
    }

    // Create depth buffer (MSAA — matches color sample count)
    {
        VkImageCreateInfo depth_ci{};
        depth_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depth_ci.imageType     = VK_IMAGE_TYPE_2D;
        depth_ci.format        = m_depth_format;
        depth_ci.extent        = {extent.width, extent.height, 1};
        depth_ci.mipLevels     = 1;
        depth_ci.arrayLayers   = 1;
        depth_ci.samples       = m_msaa_samples;
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

    // Resize m_render_finished to match the new image count. Swapchain
    // recreates on Android commonly change the image count (the surface
    // reports different minImageCount after INIT_WINDOW vs. after the
    // initial acquire), so this has to grow or shrink on every recreate.
    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    while (m_render_finished.size() > image_count) {
        vkDestroySemaphore(m_device, m_render_finished.back(), nullptr);
        m_render_finished.pop_back();
    }
    while (m_render_finished.size() < image_count) {
        VkSemaphore sem = VK_NULL_HANDLE;
        if (vkCreateSemaphore(m_device, &sem_ci, nullptr, &sem) != VK_SUCCESS) {
            log::error(TAG, "Failed to create render_finished semaphore");
            return false;
        }
        m_render_finished.push_back(sem);
    }

    log::info(TAG, "Swapchain created: {}x{}, {} images", extent.width, extent.height, image_count);
    return true;
}

void Rhi::destroy_swapchain() {
    if (m_msaa_color_view)  { vkDestroyImageView(m_device, m_msaa_color_view, nullptr); m_msaa_color_view = VK_NULL_HANDLE; }
    if (m_msaa_color_image) { vmaDestroyImage(m_allocator, m_msaa_color_image, m_msaa_color_alloc); m_msaa_color_image = VK_NULL_HANDLE; m_msaa_color_alloc = VK_NULL_HANDLE; }
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

bool Rhi::create_command_resources() {
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

bool Rhi::create_sync_objects() {
    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (u32 i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_image_available[i]) != VK_SUCCESS ||
            vkCreateFence(m_device, &fence_ci, nullptr, &m_in_flight[i]) != VK_SUCCESS)
        {
            log::error(TAG, "Failed to create sync objects");
            return false;
        }
    }

    // m_render_finished is per-swapchain-image and created/resized inside
    // create_swapchain — it has to match the image count on every resize,
    // so we don't do a one-shot creation here.

    // Dedicated pool + fence for begin_oneshot. Separate from
    // m_command_pools so background asset uploads can run without
    // racing the main thread's frame recording. The fence replaces
    // vkQueueWaitIdle at end_oneshot so a oneshot doesn't stall every
    // other in-flight submission.
    {
        VkCommandPoolCreateInfo pool_ci{};
        pool_ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // Transient: hint to the driver that command buffers from this
        // pool are short-lived. Reset bit lets us avoid re-allocating
        // the pool each call.
        pool_ci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
                                 | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_ci.queueFamilyIndex = m_graphics_family;
        if (vkCreateCommandPool(m_device, &pool_ci, nullptr, &m_oneshot_pool) != VK_SUCCESS) {
            log::error(TAG, "Failed to create oneshot command pool");
            return false;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = 0;  // not signaled; end_oneshot waits on it after submit
        if (vkCreateFence(m_device, &fci, nullptr, &m_oneshot_fence) != VK_SUCCESS) {
            log::error(TAG, "Failed to create oneshot fence");
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

void Rhi::handle_resize(u32 width, u32 height) {
    if (width == 0 || height == 0) return;
    m_swapchain_dirty = true;
}

void Rhi::set_vsync(bool enabled) {
    if (enabled == m_prefer_vsync) return;
    m_prefer_vsync = enabled;
    // Reuse the resize recreate path: the next begin_frame() rebuilds the
    // swapchain, which re-reads m_prefer_vsync to pick the present mode.
    m_swapchain_dirty = true;
}

CommandList Rhi::begin_oneshot() {
    // The mutex serializes concurrent oneshot calls — pool allocation
    // is externally synchronized per Vulkan spec. Held until end_oneshot
    // returns so the matching alloc / submit / wait / free run as one
    // critical section. (If oneshot throughput ever becomes a concern,
    // the right fix is a pool-per-thread, not finer-grained locking.)
    m_oneshot_mutex.lock();

    VkCommandBufferAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool        = m_oneshot_pool;
    alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_device, &alloc_info, &cmd);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    return CommandList(*this, cmd);
}

void Rhi::end_oneshot(CommandList& cl) {
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(cl.backend_handle());
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;

    // Per-call fence instead of vkQueueWaitIdle: only blocks until
    // THIS submission completes, not every other in-flight present /
    // frame draw. Reset before submitting because the fence is
    // re-used across oneshot calls.
    vkResetFences(m_device, 1, &m_oneshot_fence);
    vkQueueSubmit(m_graphics_queue, 1, &submit, m_oneshot_fence);
    vkWaitForFences(m_device, 1, &m_oneshot_fence, VK_TRUE, UINT64_MAX);

    vkFreeCommandBuffers(m_device, m_oneshot_pool, 1, &cmd);

    m_oneshot_mutex.unlock();
}

// ── Resource factories ──────────────────────────────────────────────────

ShaderModuleHandle Rhi::create_shader_module(std::span<const u8> spirv) {
    if (spirv.empty() || (spirv.size() % 4) != 0) {
        log::error(TAG, "create_shader_module: empty or non-multiple-of-4 bytecode ({} bytes)", spirv.size());
        return {};
    }
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(spirv.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &ci, nullptr, &mod) != VK_SUCCESS) {
        log::error(TAG, "vkCreateShaderModule failed");
        return {};
    }

    u32 idx;
    if (!m_shader_modules_free.empty()) {
        idx = m_shader_modules_free.back();
        m_shader_modules_free.pop_back();
    } else {
        idx = static_cast<u32>(m_shader_modules.size());
        m_shader_modules.emplace_back();
    }
    auto& rec = m_shader_modules[idx];
    rec.module = mod;
    // Bump generation, skipping 0 which marks a free slot.
    rec.generation += 1;
    if (rec.generation == 0) rec.generation = 1;
    return ShaderModuleHandle{idx, rec.generation};
}

void Rhi::destroy_shader_module(ShaderModuleHandle h) {
    auto* rec = detail::lookup(m_shader_modules, h);
    if (!rec) return;  // invalid / stale
    if (rec->module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(m_device, rec->module, nullptr);
        rec->module = VK_NULL_HANDLE;
    }
    rec->generation = 0;  // mark slot free
    m_shader_modules_free.push_back(h.index);
}

// resolve(ShaderModuleHandle) — inlined in vulkan_rhi.h

// ── Buffer / Texture / Sampler factories ─────────────────────────────────

namespace {

// acquire_slot / bump_generation / lookup live in rhi/detail/slot_table.h
// (shared with the GLES backend). Pull them into this TU's unqualified
// scope so the existing call sites read unchanged.
using detail::acquire_slot;
using detail::bump_generation;
using detail::lookup;

VkBufferUsageFlags to_vk_buffer_usage(BufferUsage u) {
    VkBufferUsageFlags f = 0;
    if (any(u, BufferUsage::Vertex))      f |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (any(u, BufferUsage::Index))       f |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (any(u, BufferUsage::Uniform))     f |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (any(u, BufferUsage::Storage))     f |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (any(u, BufferUsage::Indirect))    f |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (any(u, BufferUsage::TransferSrc)) f |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (any(u, BufferUsage::TransferDst)) f |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return f;
}

VkImageUsageFlags to_vk_image_usage(TextureUsage u) {
    VkImageUsageFlags f = 0;
    if (any(u, TextureUsage::Sampled))         f |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (any(u, TextureUsage::ColorAttachment)) f |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (any(u, TextureUsage::DepthAttachment)) f |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (any(u, TextureUsage::Storage))         f |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (any(u, TextureUsage::TransferSrc))     f |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (any(u, TextureUsage::TransferDst))     f |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return f;
}

static TextureFormat from_vk_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM:               return TextureFormat::R8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM:         return TextureFormat::R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:          return TextureFormat::R8G8B8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM:         return TextureFormat::B8G8R8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB:          return TextureFormat::B8G8R8A8_SRGB;
        case VK_FORMAT_D32_SFLOAT:             return TextureFormat::D32_SFLOAT;
        default: break;
    }
    return TextureFormat::Undefined;
}

VkFormat to_vk_format(TextureFormat f) {
    switch (f) {
        case TextureFormat::R8_UNORM:            return VK_FORMAT_R8_UNORM;
        case TextureFormat::R8G8B8A8_UNORM:      return VK_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::R8G8B8A8_SRGB:       return VK_FORMAT_R8G8B8A8_SRGB;
        case TextureFormat::B8G8R8A8_UNORM:      return VK_FORMAT_B8G8R8A8_UNORM;
        case TextureFormat::B8G8R8A8_SRGB:       return VK_FORMAT_B8G8R8A8_SRGB;
        case TextureFormat::R16_UNORM:           return VK_FORMAT_R16_UNORM;
        case TextureFormat::R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case TextureFormat::R32_SFLOAT:          return VK_FORMAT_R32_SFLOAT;
        case TextureFormat::D32_SFLOAT:          return VK_FORMAT_D32_SFLOAT;
        case TextureFormat::R32G32_SFLOAT:       return VK_FORMAT_R32G32_SFLOAT;
        case TextureFormat::R32G32B32_SFLOAT:    return VK_FORMAT_R32G32B32_SFLOAT;
        case TextureFormat::R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case TextureFormat::R32_UINT:            return VK_FORMAT_R32_UINT;
        case TextureFormat::R32G32B32A32_UINT:   return VK_FORMAT_R32G32B32A32_UINT;
        case TextureFormat::BC1_RGB_UNORM:       return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case TextureFormat::BC1_RGB_SRGB:        return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
        case TextureFormat::BC3_RGBA_UNORM:      return VK_FORMAT_BC3_UNORM_BLOCK;
        case TextureFormat::BC3_RGBA_SRGB:       return VK_FORMAT_BC3_SRGB_BLOCK;
        case TextureFormat::BC5_RG_UNORM:        return VK_FORMAT_BC5_UNORM_BLOCK;
        case TextureFormat::ASTC_4x4_UNORM:      return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
        case TextureFormat::ASTC_4x4_SRGB:       return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
        case TextureFormat::ETC2_RGB8_UNORM:     return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
        case TextureFormat::ETC2_RGBA8_UNORM:    return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
        case TextureFormat::Undefined:           return VK_FORMAT_UNDEFINED;
    }
    return VK_FORMAT_UNDEFINED;
}

VkFilter to_vk_filter(Filter f) {
    return f == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}
VkSamplerMipmapMode to_vk_mipmap_mode(MipmapMode m) {
    return m == MipmapMode::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}
VkSamplerAddressMode to_vk_address(AddressMode a) {
    switch (a) {
        case AddressMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case AddressMode::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}
VkCompareOp to_vk_compare_op(CompareOp op) {
    switch (op) {
        case CompareOp::Never:        return VK_COMPARE_OP_NEVER;
        case CompareOp::Less:         return VK_COMPARE_OP_LESS;
        case CompareOp::Equal:        return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater:      return VK_COMPARE_OP_GREATER;
        case CompareOp::NotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Always:       return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_NEVER;
}

VkBorderColor to_vk_border(BorderColor b) {
    switch (b) {
        case BorderColor::TransparentBlack: return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        case BorderColor::OpaqueBlack:      return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        case BorderColor::OpaqueWhite:      return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}

VkImageAspectFlags aspect_for_usage(TextureUsage u) {
    return any(u, TextureUsage::DepthAttachment)
        ? VK_IMAGE_ASPECT_DEPTH_BIT
        : VK_IMAGE_ASPECT_COLOR_BIT;
}

} // anonymous

TextureFormat Rhi::swapchain_format() const { return from_vk_format(m_swapchain_format); }
TextureFormat Rhi::depth_format()     const { return from_vk_format(m_depth_format); }

BufferHandle Rhi::create_buffer(const BufferDesc& desc) {
    if (desc.size == 0 || desc.usage == BufferUsage::None) {
        log::error(TAG, "create_buffer: invalid desc (size={}, usage=0)", desc.size);
        return {};
    }

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = desc.size;
    bci.usage = to_vk_buffer_usage(desc.usage);

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    switch (desc.memory) {
        case MemoryUsage::GpuOnly:
            break;
        case MemoryUsage::HostSequential:
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                      | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case MemoryUsage::HostRandom:
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                      | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
    }

    VkBuffer       buf  = VK_NULL_HANDLE;
    VmaAllocation  alloc = VK_NULL_HANDLE;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(m_allocator, &bci, &aci, &buf, &alloc, &info) != VK_SUCCESS) {
        log::error(TAG, "vmaCreateBuffer failed (size={})", desc.size);
        return {};
    }

    u32 idx = acquire_slot(m_buffers, m_buffers_free);
    auto& rec = m_buffers[idx];
    rec.buffer = buf;
    rec.alloc  = alloc;
    rec.mapped = info.pMappedData;  // null when not persistently mapped
    rec.size   = desc.size;
    u32 gen    = bump_generation(rec);
    return BufferHandle{idx, gen};
}

void Rhi::destroy_buffer(BufferHandle h) {
    auto* rec = detail::lookup(m_buffers, h);
    if (!rec) return;
    if (rec->buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_allocator, rec->buffer, rec->alloc);
        rec->buffer = VK_NULL_HANDLE;
        rec->alloc  = VK_NULL_HANDLE;
        rec->mapped = nullptr;
    }
    rec->generation = 0;
    m_buffers_free.push_back(h.index);
}

// resolve(BufferHandle) — inlined in vulkan_rhi.h

VmaAllocation Rhi::alloc_of(BufferHandle h) const {
    const auto* rec = detail::lookup(m_buffers, h);
    return rec ? rec->alloc : VK_NULL_HANDLE;
}

void* Rhi::mapped_ptr(BufferHandle h) const {
    const auto* rec = detail::lookup(m_buffers, h);
    return rec ? rec->mapped : nullptr;
}

TextureHandle Rhi::create_texture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0 || desc.format == TextureFormat::Undefined) {
        log::error(TAG, "create_texture: invalid desc ({}x{}, format=undefined?)",
                   desc.width, desc.height);
        return {};
    }

    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.format        = to_vk_format(desc.format);
    ici.extent        = { desc.width, desc.height, desc.depth };
    ici.mipLevels     = desc.mip_levels;
    ici.arrayLayers   = desc.array_layers;
    ici.samples       = static_cast<VkSampleCountFlagBits>(desc.sample_count);
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = to_vk_image_usage(desc.usage);
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    switch (desc.type) {
        case TextureType::Texture2D:   ici.imageType = VK_IMAGE_TYPE_2D; break;
        case TextureType::TextureCube: ici.imageType = VK_IMAGE_TYPE_2D;
                                       ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
                                       break;
        case TextureType::Texture3D:   ici.imageType = VK_IMAGE_TYPE_3D; break;
    }

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;

    VkImage       img   = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    if (vmaCreateImage(m_allocator, &ici, &aci, &img, &alloc, nullptr) != VK_SUCCESS) {
        log::error(TAG, "vmaCreateImage failed ({}x{})", desc.width, desc.height);
        return {};
    }

    // Default view: covers all mips/layers, dim derived from texture type.
    VkImageViewCreateInfo vci{};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = img;
    vci.format   = ici.format;
    switch (desc.type) {
        case TextureType::Texture2D:
            vci.viewType = (desc.array_layers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                   : VK_IMAGE_VIEW_TYPE_2D;
            break;
        case TextureType::TextureCube: vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE; break;
        case TextureType::Texture3D:   vci.viewType = VK_IMAGE_VIEW_TYPE_3D;   break;
    }
    vci.subresourceRange.aspectMask     = aspect_for_usage(desc.usage);
    vci.subresourceRange.baseMipLevel   = 0;
    vci.subresourceRange.levelCount     = desc.mip_levels;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount     = desc.array_layers;

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(m_device, &vci, nullptr, &view) != VK_SUCCESS) {
        log::error(TAG, "vkCreateImageView failed");
        vmaDestroyImage(m_allocator, img, alloc);
        return {};
    }

    u32 idx = acquire_slot(m_textures, m_textures_free);
    auto& rec = m_textures[idx];
    rec.image = img;
    rec.view  = view;
    rec.alloc = alloc;
    u32 gen = bump_generation(rec);
    return TextureHandle{idx, gen};
}

void Rhi::destroy_texture(TextureHandle h) {
    auto* rec = detail::lookup(m_textures, h);
    if (!rec) return;
    if (rec->view  != VK_NULL_HANDLE) vkDestroyImageView(m_device, rec->view, nullptr);
    if (rec->image != VK_NULL_HANDLE) vmaDestroyImage(m_allocator, rec->image, rec->alloc);
    rec->view = VK_NULL_HANDLE; rec->image = VK_NULL_HANDLE; rec->alloc = VK_NULL_HANDLE;
    rec->generation = 0;
    m_textures_free.push_back(h.index);
}

// resolve(TextureHandle) / resolve_view — inlined in vulkan_rhi.h

VmaAllocation Rhi::alloc_of(TextureHandle h) const {
    const auto* rec = detail::lookup(m_textures, h);
    return rec ? rec->alloc : VK_NULL_HANDLE;
}

SamplerHandle Rhi::create_sampler(const SamplerDesc& desc) {
    VkSamplerCreateInfo sci{};
    sci.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter        = to_vk_filter(desc.mag_filter);
    sci.minFilter        = to_vk_filter(desc.min_filter);
    sci.mipmapMode       = to_vk_mipmap_mode(desc.mipmap_mode);
    sci.addressModeU     = to_vk_address(desc.address_u);
    sci.addressModeV     = to_vk_address(desc.address_v);
    sci.addressModeW     = to_vk_address(desc.address_w);
    sci.mipLodBias       = desc.mip_lod_bias;
    sci.anisotropyEnable = (desc.max_anisotropy > 1.0f) ? VK_TRUE : VK_FALSE;
    sci.maxAnisotropy    = desc.max_anisotropy;
    sci.compareEnable    = desc.compare_enable ? VK_TRUE : VK_FALSE;
    sci.compareOp        = to_vk_compare_op(desc.compare_op);
    sci.minLod           = desc.min_lod;
    sci.maxLod           = desc.max_lod;
    sci.borderColor      = to_vk_border(desc.border_color);

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(m_device, &sci, nullptr, &sampler) != VK_SUCCESS) {
        log::error(TAG, "vkCreateSampler failed");
        return {};
    }

    u32 idx = acquire_slot(m_samplers, m_samplers_free);
    auto& rec = m_samplers[idx];
    rec.sampler = sampler;
    u32 gen = bump_generation(rec);
    return SamplerHandle{idx, gen};
}

void Rhi::destroy_sampler(SamplerHandle h) {
    auto* rec = detail::lookup(m_samplers, h);
    if (!rec) return;
    if (rec->sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, rec->sampler, nullptr);
        rec->sampler = VK_NULL_HANDLE;
    }
    rec->generation = 0;
    m_samplers_free.push_back(h.index);
}

// resolve(SamplerHandle) — inlined in vulkan_rhi.h

// ── Pipeline / descriptor enum translation ──────────────────────────────

namespace {

VkDescriptorType to_vk_descriptor_type(DescriptorType t) {
    switch (t) {
        case DescriptorType::UniformBuffer:        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case DescriptorType::StorageBuffer:        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case DescriptorType::SampledImage:         return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case DescriptorType::CombinedImageSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

VkShaderStageFlags to_vk_shader_stage(ShaderStage s) {
    VkShaderStageFlags f = 0;
    if (any(s, ShaderStage::Vertex))   f |= VK_SHADER_STAGE_VERTEX_BIT;
    if (any(s, ShaderStage::Fragment)) f |= VK_SHADER_STAGE_FRAGMENT_BIT;
    return f;
}

VkPrimitiveTopology to_vk_topology(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::TriangleList:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PrimitiveTopology::LineList:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCullModeFlags to_vk_cull(CullMode c) {
    switch (c) {
        case CullMode::None:  return VK_CULL_MODE_NONE;
        case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back:  return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_BACK_BIT;
}

VkFrontFace to_vk_front_face(FrontFace f) {
    return f == FrontFace::CounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
}

VkPolygonMode to_vk_polygon_mode(PolygonMode m) {
    return m == PolygonMode::Line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
}

VkBlendFactor to_vk_blend_factor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One:              return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcColor:         return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColor:         return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::OneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:         return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ZERO;
}

VkBlendOp to_vk_blend_op(BlendOp o) {
    switch (o) {
        case BlendOp::Add:             return VK_BLEND_OP_ADD;
        case BlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BlendOp::Min:             return VK_BLEND_OP_MIN;
        case BlendOp::Max:             return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkVertexInputRate to_vk_input_rate(bool per_instance) {
    return per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
}

} // anonymous

// ── Descriptor set layouts ──────────────────────────────────────────────

DescriptorSetLayoutHandle Rhi::create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) {
    std::vector<VkDescriptorSetLayoutBinding> vk_bindings(desc.bindings.size());
    std::vector<VkDescriptorBindingFlags>     vk_flags(desc.bindings.size(), 0);
    bool any_bindless = false;

    for (usize i = 0; i < desc.bindings.size(); ++i) {
        const auto& b = desc.bindings[i];
        vk_bindings[i] = {};
        vk_bindings[i].binding         = b.binding;
        vk_bindings[i].descriptorType  = to_vk_descriptor_type(b.type);
        vk_bindings[i].descriptorCount = b.count;
        vk_bindings[i].stageFlags      = to_vk_shader_stage(b.stages);

        VkDescriptorBindingFlags f = 0;
        if (b.partially_bound)   { f |= VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT; any_bindless = true; }
        if (b.variable_count)    { f |= VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT; any_bindless = true; }
        if (b.update_after_bind) { f |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT; any_bindless = true; }
        vk_flags[i] = f;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
    flags_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_ci.bindingCount  = static_cast<u32>(vk_flags.size());
    flags_ci.pBindingFlags = vk_flags.data();

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = static_cast<u32>(vk_bindings.size());
    ci.pBindings    = vk_bindings.data();
    if (any_bindless) {
        ci.pNext = &flags_ci;
        ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    }

    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(m_device, &ci, nullptr, &layout) != VK_SUCCESS) {
        log::error(TAG, "vkCreateDescriptorSetLayout failed");
        return {};
    }

    u32 idx = acquire_slot(m_dsl_records, m_dsl_free);
    auto& rec = m_dsl_records[idx];
    rec.layout = layout;
    u32 gen = bump_generation(rec);
    return DescriptorSetLayoutHandle{idx, gen};
}

void Rhi::destroy_descriptor_set_layout(DescriptorSetLayoutHandle h) {
    auto* rec = detail::lookup(m_dsl_records, h);
    if (!rec) return;
    if (rec->layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, rec->layout, nullptr);
        rec->layout = VK_NULL_HANDLE;
    }
    rec->generation = 0;
    m_dsl_free.push_back(h.index);
}

// resolve(DescriptorSetLayoutHandle) — inlined in vulkan_rhi.h

// ── Descriptor pool growth ──────────────────────────────────────────────

bool Rhi::grow_descriptor_pool() {
    // Generous fixed-size pool — supports the engine's largest descriptor
    // workload (bindless + per-frame + per-entity + UI) for one session
    // before needing to grow. Each call appends a fresh pool to the ring.
    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         128 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         128 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          64  },
    };
    VkDescriptorPoolCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT
                     | VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    ci.maxSets       = 256;
    ci.poolSizeCount = static_cast<u32>(std::size(sizes));
    ci.pPoolSizes    = sizes;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(m_device, &ci, nullptr, &pool) != VK_SUCCESS) {
        log::error(TAG, "Failed to create descriptor pool");
        return false;
    }
    m_descriptor_pools.push_back(pool);
    return true;
}

DescriptorSetHandle Rhi::allocate_descriptor_set(DescriptorSetLayoutHandle layout_h,
                                                       u32 variable_count) {
    VkDescriptorSetLayout layout = resolve(layout_h);
    if (layout == VK_NULL_HANDLE) return {};

    if (m_descriptor_pools.empty() && !grow_descriptor_pool()) return {};

    VkDescriptorSetVariableDescriptorCountAllocateInfo var_ai{};
    var_ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    var_ai.descriptorSetCount = 1;
    var_ai.pDescriptorCounts  = &variable_count;

    auto try_alloc = [&](VkDescriptorPool pool, VkDescriptorSet& out) -> VkResult {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.pNext              = (variable_count > 0) ? &var_ai : nullptr;
        ai.descriptorPool     = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &layout;
        return vkAllocateDescriptorSets(m_device, &ai, &out);
    };

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool current = m_descriptor_pools.back();
    VkResult r = try_alloc(current, set);
    if (r != VK_SUCCESS) {
        if (!grow_descriptor_pool()) return {};
        current = m_descriptor_pools.back();
        r = try_alloc(current, set);
        if (r != VK_SUCCESS) {
            log::error(TAG, "vkAllocateDescriptorSets failed even after pool grow ({})",
                       static_cast<int>(r));
            return {};
        }
    }

    u32 idx = acquire_slot(m_dset_records, m_dset_free);
    auto& rec = m_dset_records[idx];
    rec.set    = set;
    rec.source = current;
    u32 gen = bump_generation(rec);
    return DescriptorSetHandle{idx, gen};
}

void Rhi::update_descriptor_set(DescriptorSetHandle h, std::span<const WriteDescriptor> writes) {
    VkDescriptorSet set = resolve(h);
    if (set == VK_NULL_HANDLE || writes.empty()) return;

    std::vector<VkDescriptorBufferInfo> buf_infos(writes.size());
    std::vector<VkDescriptorImageInfo>  img_infos(writes.size());
    std::vector<VkWriteDescriptorSet>   vk_writes(writes.size());

    for (usize i = 0; i < writes.size(); ++i) {
        const auto& w = writes[i];
        vk_writes[i] = {};
        vk_writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        vk_writes[i].dstSet          = set;
        vk_writes[i].dstBinding      = w.binding;
        vk_writes[i].dstArrayElement = w.array_element;
        vk_writes[i].descriptorCount = 1;
        vk_writes[i].descriptorType  = to_vk_descriptor_type(w.type);

        switch (w.type) {
            case DescriptorType::UniformBuffer:
            case DescriptorType::StorageBuffer: {
                buf_infos[i].buffer = resolve(w.buffer);
                buf_infos[i].offset = w.buffer_offset;
                buf_infos[i].range  = (w.buffer_range == ~0ull) ? VK_WHOLE_SIZE : w.buffer_range;
                vk_writes[i].pBufferInfo = &buf_infos[i];
                break;
            }
            case DescriptorType::SampledImage:
            case DescriptorType::CombinedImageSampler: {
                VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                switch (w.image_layout) {
                    case WriteImageLayout::ShaderReadOnly:       layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; break;
                    case WriteImageLayout::DepthStencilReadOnly: layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL; break;
                    case WriteImageLayout::DepthReadOnly:        layout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL; break;
                    case WriteImageLayout::General:              layout = VK_IMAGE_LAYOUT_GENERAL; break;
                }
                img_infos[i].imageLayout = layout;
                img_infos[i].imageView   = resolve_view(w.texture);
                img_infos[i].sampler     = resolve(w.sampler);
                vk_writes[i].pImageInfo  = &img_infos[i];
                break;
            }
        }
    }

    vkUpdateDescriptorSets(m_device, static_cast<u32>(vk_writes.size()), vk_writes.data(), 0, nullptr);
}

void Rhi::free_descriptor_set(DescriptorSetHandle h) {
    if (!h.is_valid() || h.index >= m_dset_records.size()) return;
    auto& rec = m_dset_records[h.index];
    if (rec.generation != h.generation) return;
    if (rec.set != VK_NULL_HANDLE && rec.source != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(m_device, rec.source, 1, &rec.set);
    }
    rec.set = VK_NULL_HANDLE;
    rec.source = VK_NULL_HANDLE;
    rec.generation = 0;
    m_dset_free.push_back(h.index);
}

// resolve(DescriptorSetHandle) — inlined in vulkan_rhi.h

// ── Pipeline layout ─────────────────────────────────────────────────────

PipelineLayoutHandle Rhi::create_pipeline_layout(const PipelineLayoutDesc& desc) {
    std::vector<VkDescriptorSetLayout> vk_layouts(desc.set_layouts.size());
    for (usize i = 0; i < desc.set_layouts.size(); ++i) {
        vk_layouts[i] = resolve(desc.set_layouts[i]);
    }
    std::vector<VkPushConstantRange> vk_pcs(desc.push_constants.size());
    for (usize i = 0; i < desc.push_constants.size(); ++i) {
        vk_pcs[i].stageFlags = to_vk_shader_stage(desc.push_constants[i].stages);
        vk_pcs[i].offset     = desc.push_constants[i].offset;
        vk_pcs[i].size       = desc.push_constants[i].size;
    }

    VkPipelineLayoutCreateInfo ci{};
    ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount         = static_cast<u32>(vk_layouts.size());
    ci.pSetLayouts            = vk_layouts.empty() ? nullptr : vk_layouts.data();
    ci.pushConstantRangeCount = static_cast<u32>(vk_pcs.size());
    ci.pPushConstantRanges    = vk_pcs.empty() ? nullptr : vk_pcs.data();

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(m_device, &ci, nullptr, &layout) != VK_SUCCESS) {
        log::error(TAG, "vkCreatePipelineLayout failed");
        return {};
    }

    u32 idx = acquire_slot(m_pl_records, m_pl_free);
    auto& rec = m_pl_records[idx];
    rec.layout = layout;
    u32 gen = bump_generation(rec);
    return PipelineLayoutHandle{idx, gen};
}

void Rhi::destroy_pipeline_layout(PipelineLayoutHandle h) {
    auto* rec = detail::lookup(m_pl_records, h);
    if (!rec) return;
    if (rec->layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(m_device, rec->layout, nullptr);
        rec->layout = VK_NULL_HANDLE;
    }
    rec->generation = 0;
    m_pl_free.push_back(h.index);
}

// resolve(PipelineLayoutHandle) — inlined in vulkan_rhi.h

// ── Graphics pipeline ───────────────────────────────────────────────────

PipelineHandle Rhi::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    // Stages
    std::vector<VkPipelineShaderStageCreateInfo> vk_stages(desc.stages.size());
    for (usize i = 0; i < desc.stages.size(); ++i) {
        vk_stages[i] = {};
        vk_stages[i].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vk_stages[i].stage  = static_cast<VkShaderStageFlagBits>(to_vk_shader_stage(desc.stages[i].stage));
        vk_stages[i].module = resolve(desc.stages[i].module);
        vk_stages[i].pName  = desc.stages[i].entry;
    }

    // Vertex input
    std::vector<VkVertexInputBindingDescription>   vk_bindings(desc.vertex_input.bindings.size());
    std::vector<VkVertexInputAttributeDescription> vk_attrs(desc.vertex_input.attributes.size());
    for (usize i = 0; i < desc.vertex_input.bindings.size(); ++i) {
        vk_bindings[i] = {};
        vk_bindings[i].binding   = desc.vertex_input.bindings[i].binding;
        vk_bindings[i].stride    = desc.vertex_input.bindings[i].stride;
        vk_bindings[i].inputRate = to_vk_input_rate(desc.vertex_input.bindings[i].per_instance);
    }
    for (usize i = 0; i < desc.vertex_input.attributes.size(); ++i) {
        vk_attrs[i] = {};
        vk_attrs[i].location = desc.vertex_input.attributes[i].location;
        vk_attrs[i].binding  = desc.vertex_input.attributes[i].binding;
        vk_attrs[i].offset   = desc.vertex_input.attributes[i].offset;
        vk_attrs[i].format   = to_vk_format(desc.vertex_input.attributes[i].format);
    }
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = static_cast<u32>(vk_bindings.size());
    vi.pVertexBindingDescriptions      = vk_bindings.empty() ? nullptr : vk_bindings.data();
    vi.vertexAttributeDescriptionCount = static_cast<u32>(vk_attrs.size());
    vi.pVertexAttributeDescriptions    = vk_attrs.empty() ? nullptr : vk_attrs.data();

    // Input assembly
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = to_vk_topology(desc.topology);

    // Viewport/scissor — always dynamic in this engine
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    // Rasterizer
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode             = to_vk_polygon_mode(desc.rasterizer.polygon_mode);
    rs.cullMode                = to_vk_cull(desc.rasterizer.cull_mode);
    rs.frontFace               = to_vk_front_face(desc.rasterizer.front_face);
    rs.depthBiasEnable         = desc.rasterizer.depth_bias_enable ? VK_TRUE : VK_FALSE;
    rs.depthBiasConstantFactor = desc.rasterizer.depth_bias_constant_factor;
    rs.depthBiasSlopeFactor    = desc.rasterizer.depth_bias_slope_factor;
    rs.lineWidth               = desc.rasterizer.line_width;

    // Multisample
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(desc.multisample.sample_count);

    // Depth/stencil
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = desc.depth_stencil.depth_test_enable ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = desc.depth_stencil.depth_write_enable ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp   = to_vk_compare_op(desc.depth_stencil.depth_compare);

    // Blend
    std::vector<VkPipelineColorBlendAttachmentState> vk_blend(desc.blend_attachments.size());
    for (usize i = 0; i < desc.blend_attachments.size(); ++i) {
        const auto& a = desc.blend_attachments[i];
        vk_blend[i] = {};
        vk_blend[i].blendEnable         = a.blend_enable ? VK_TRUE : VK_FALSE;
        vk_blend[i].srcColorBlendFactor = to_vk_blend_factor(a.src_color_factor);
        vk_blend[i].dstColorBlendFactor = to_vk_blend_factor(a.dst_color_factor);
        vk_blend[i].colorBlendOp        = to_vk_blend_op(a.color_op);
        vk_blend[i].srcAlphaBlendFactor = to_vk_blend_factor(a.src_alpha_factor);
        vk_blend[i].dstAlphaBlendFactor = to_vk_blend_factor(a.dst_alpha_factor);
        vk_blend[i].alphaBlendOp        = to_vk_blend_op(a.alpha_op);
        vk_blend[i].colorWriteMask      = static_cast<VkColorComponentFlags>(a.write_mask);
    }
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = static_cast<u32>(vk_blend.size());
    cb.pAttachments    = vk_blend.empty() ? nullptr : vk_blend.data();

    // Dynamic state — viewport + scissor are always dynamic; cull mode
    // is opt-in per pipeline so authors who never need doubleSided don't
    // pay for the per-draw cull-mode write.
    std::vector<VkDynamicState> dyn_states = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    if (desc.rasterizer.cull_mode_dynamic) {
        // Core in Vulkan 1.3; was VK_EXT_extended_dynamic_state before.
        dyn_states.push_back(VK_DYNAMIC_STATE_CULL_MODE);
    }
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = static_cast<u32>(dyn_states.size());
    dyn.pDynamicStates    = dyn_states.data();

    // Dynamic rendering
    std::vector<VkFormat> color_formats(desc.render.color_formats.size());
    for (usize i = 0; i < color_formats.size(); ++i) {
        color_formats[i] = to_vk_format(desc.render.color_formats[i]);
    }
    VkPipelineRenderingCreateInfo render_ci{};
    render_ci.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    render_ci.colorAttachmentCount    = static_cast<u32>(color_formats.size());
    render_ci.pColorAttachmentFormats = color_formats.empty() ? nullptr : color_formats.data();
    render_ci.depthAttachmentFormat   = to_vk_format(desc.render.depth_format);

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.pNext               = &render_ci;
    pci.stageCount          = static_cast<u32>(vk_stages.size());
    pci.pStages             = vk_stages.data();
    pci.pVertexInputState   = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState      = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState   = &ms;
    pci.pDepthStencilState  = &ds;
    pci.pColorBlendState    = &cb;
    pci.pDynamicState       = &dyn;
    pci.layout              = resolve(desc.layout);

    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe) != VK_SUCCESS) {
        log::error(TAG, "vkCreateGraphicsPipelines failed");
        return {};
    }

    u32 idx = acquire_slot(m_pipeline_records, m_pipeline_free);
    auto& rec = m_pipeline_records[idx];
    rec.pipeline = pipe;
    u32 gen = bump_generation(rec);
    return PipelineHandle{idx, gen};
}

void Rhi::destroy_pipeline(PipelineHandle h) {
    auto* rec = detail::lookup(m_pipeline_records, h);
    if (!rec) return;
    if (rec->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(m_device, rec->pipeline, nullptr);
        rec->pipeline = VK_NULL_HANDLE;
    }
    rec->generation = 0;
    m_pipeline_free.push_back(h.index);
}

// resolve(PipelineHandle) — inlined in vulkan_rhi.h

CommandList Rhi::begin_frame() {
    vkWaitForFences(m_device, 1, &m_in_flight[m_frame_index], VK_TRUE, UINT64_MAX);

    VkResult result = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX,
        m_image_available[m_frame_index], VK_NULL_HANDLE, &m_current_image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || m_swapchain_dirty) {
        m_swapchain_dirty = false;
        vkDeviceWaitIdle(m_device);

        // The acquire may have signaled the semaphore even on failure
        // (driver-dependent). Recreate it unconditionally to guarantee it
        // starts unsignaled before the next acquire.
        vkDestroySemaphore(m_device, m_image_available[m_frame_index], nullptr);
        VkSemaphoreCreateInfo sem_ci{};
        sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(m_device, &sem_ci, nullptr, &m_image_available[m_frame_index]);

        create_swapchain(m_swapchain_extent.width, m_swapchain_extent.height);
        if (m_swapchain_extent.width == 0 || m_swapchain_extent.height == 0) {
            m_swapchain_dirty = true;  // retry next frame
        }
        return CommandList{};  // invalid — caller skips the frame
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

    // Transition: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL (MSAA color + swapchain resolve target)
    VkImageMemoryBarrier2 barriers[3]{};

    // MSAA color image
    barriers[0].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    barriers[0].srcAccessMask = VK_ACCESS_2_NONE;
    barriers[0].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].image         = m_msaa_color_image;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Swapchain image (resolve target)
    barriers[1].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    barriers[1].srcAccessMask = VK_ACCESS_2_NONE;
    barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[1].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[1].image         = m_swapchain_images[m_current_image_index];
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    // Depth (MSAA)
    barriers[2].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[2].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    barriers[2].srcAccessMask = VK_ACCESS_2_NONE;
    barriers[2].dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barriers[2].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barriers[2].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[2].newLayout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barriers[2].image         = m_depth_image;
    barriers[2].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount  = 3;
    dep.pImageMemoryBarriers     = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);

    m_frame_active = true;
    return CommandList{*this, cmd};
}

void Rhi::begin_rendering() {
    if (!m_frame_active) return;
    VkCommandBuffer cmd = m_command_buffers[m_frame_index];

    // Render to MSAA color target, resolve to swapchain image
    VkRenderingAttachmentInfo color_attachment{};
    color_attachment.sType              = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attachment.imageView          = m_msaa_color_view;
    color_attachment.imageLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.loadOp             = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp            = VK_ATTACHMENT_STORE_OP_DONT_CARE;  // MSAA image is transient
    color_attachment.clearValue         = {{{0.1f, 0.1f, 0.1f, 1.0f}}};
    color_attachment.resolveMode        = VK_RESOLVE_MODE_AVERAGE_BIT;
    color_attachment.resolveImageView   = m_swapchain_views[m_current_image_index];
    color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

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

void Rhi::end_frame() {
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
    submit.pSignalSemaphores    = &m_render_finished[m_current_image_index];

    vkQueueSubmit(m_graphics_queue, 1, &submit, m_in_flight[m_frame_index]);

    // Present
    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &m_render_finished[m_current_image_index];
    present.swapchainCount     = 1;
    present.pSwapchains        = &m_swapchain;
    present.pImageIndices      = &m_current_image_index;

    VkResult result = vkQueuePresentKHR(m_present_queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Hard error — swapchain can no longer present to this surface.
        m_swapchain_dirty = true;
    }
    // VK_SUBOPTIMAL_KHR is intentionally ignored. On Android it's returned
    // every frame when our preTransform = IDENTITY doesn't match the
    // surface's currentTransform (a common, benign state). Treating it as
    // "recreate now" would trigger a per-frame swapchain-destroy loop,
    // which in turn makes Android's BufferQueue warn about buffers being
    // freed while dequeued. Let the compositor keep handling the rotation.

    m_frame_index = (m_frame_index + 1) % MAX_FRAMES_IN_FLIGHT;
}

// ---------------------------------------------------------------------------
// VMA allocator
// ---------------------------------------------------------------------------

bool Rhi::create_allocator() {
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

} // namespace uldum::rhi
