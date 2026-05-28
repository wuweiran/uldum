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
#include <span>
#include <vector>

#include "rhi/handles.h"
#include "rhi/types.h"
#include "rhi/command_list.h"

namespace uldum::platform { class Platform; }

namespace uldum::rhi {

static constexpr u32 MAX_FRAMES_IN_FLIGHT = 2;

struct Config {
    bool enable_validation = true;
};

class Rhi {
public:
    Rhi() = default;
    ~Rhi();

    bool init(const Config& config, platform::Platform& platform);
    void shutdown();

    // begin_frame acquires swapchain image, returns command list to record into.
    // Returns an invalid CommandList if swapchain needs recreate (caller should
    // skip the frame; check with cmd.is_valid()).
    // Call begin_rendering() after any pre-render passes (e.g. shadow) to start the main pass.
    CommandList begin_frame();
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

    // Block until all submitted GPU work has finished. Used by consumers
    // before destroying resources that may be in-flight (texture rebind,
    // session teardown). Backend-agnostic.
    void wait_idle();

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
    Extent2D         extent()    const { return { m_swapchain_extent.width, m_swapchain_extent.height }; }
    VkExtent2D       vk_extent() const { return m_swapchain_extent; }
    TextureFormat    swapchain_format() const;
    TextureFormat    depth_format()     const;
    // Backend-typed accessors — only valid in backend-tied code (ImGui).
    VkFormat         swapchain_format_vk() const { return m_swapchain_format; }
    VkFormat         depth_format_vk()     const { return m_depth_format; }
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
    CommandList begin_oneshot();
    void        end_oneshot(CommandList& cmd);

    // ── Resource factories (Stage 1 of the RHI abstraction) ──────────────
    //
    // These return opaque handles; the backing Vulkan object is owned by
    // the RHI. `resolve()` exposes the raw Vk type for use during the
    // transition period — once Stage 5 lands and no consumer code outside
    // src/rhi/ touches Vk types, these resolvers become private.

    // Create a shader module from a span of SPIR-V bytecode. The bytes
    // are uploaded synchronously; the caller may free its copy on return.
    // Returns an invalid handle (and logs) on failure.
    ShaderModuleHandle create_shader_module(std::span<const u8> spirv);
    void               destroy_shader_module(ShaderModuleHandle h);
    VkShaderModule     resolve(ShaderModuleHandle h) const;

    // Buffers. `mapped_ptr` returns a persistent CPU pointer for
    // HostSequential / HostRandom memory; null for GpuOnly. `alloc_of`
    // exposes the VMA handle for direct vmaCmdCopy/staging until
    // higher-level upload helpers replace it.
    BufferHandle  create_buffer(const BufferDesc& desc);
    void          destroy_buffer(BufferHandle h);
    VkBuffer      resolve(BufferHandle h) const;
    VmaAllocation alloc_of(BufferHandle h) const;
    void*         mapped_ptr(BufferHandle h) const;

    // Textures bundle image + view + allocation. resolve_view() returns
    // the default view that covers all mips / layers. Stage 3 will add
    // explicit subresource view creation if any consumer needs it.
    TextureHandle create_texture(const TextureDesc& desc);
    void          destroy_texture(TextureHandle h);
    VkImage       resolve(TextureHandle h) const;
    VkImageView   resolve_view(TextureHandle h) const;
    VmaAllocation alloc_of(TextureHandle h) const;

    // Samplers. Cheap; engine typically creates a handful and reuses.
    SamplerHandle create_sampler(const SamplerDesc& desc);
    void          destroy_sampler(SamplerHandle h);
    VkSampler     resolve(SamplerHandle h) const;

    // Descriptor set layouts.
    DescriptorSetLayoutHandle create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc);
    void                      destroy_descriptor_set_layout(DescriptorSetLayoutHandle h);
    VkDescriptorSetLayout     resolve(DescriptorSetLayoutHandle h) const;

    // Descriptor sets. Pool management is hidden — the RHI keeps a free
    // list of pools and grows when the active pool fills up.
    // `variable_count` is only used when the layout has a
    // VARIABLE_DESCRIPTOR_COUNT_BIT binding (bindless arrays). Pass 0
    // otherwise; the alloc uses the binding's static size.
    DescriptorSetHandle  allocate_descriptor_set(DescriptorSetLayoutHandle layout, u32 variable_count = 0);
    void                 update_descriptor_set(DescriptorSetHandle h,
                                                std::span<const WriteDescriptor> writes);
    void                 free_descriptor_set(DescriptorSetHandle h);
    VkDescriptorSet      resolve(DescriptorSetHandle h) const;

    // Pipeline layout + graphics pipeline.
    PipelineLayoutHandle create_pipeline_layout(const PipelineLayoutDesc& desc);
    void                 destroy_pipeline_layout(PipelineLayoutHandle h);
    VkPipelineLayout     resolve(PipelineLayoutHandle h) const;

    PipelineHandle       create_graphics_pipeline(const GraphicsPipelineDesc& desc);
    void                 destroy_pipeline(PipelineHandle h);
    VkPipeline           resolve(PipelineHandle h) const;

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

    // ── Resource record tables ───────────────────────────────────────────
    // Flat vector indexed by handle.index. `generation` in the record
    // matches handle.generation when the slot is live. A freed slot has
    // `generation == 0`; its index goes into the free list for reuse.
    struct ShaderModuleRecord {
        VkShaderModule module = VK_NULL_HANDLE;
        u32            generation = 0;
    };
    std::vector<ShaderModuleRecord> m_shader_modules;
    std::vector<u32>                m_shader_modules_free;

    struct BufferRecord {
        VkBuffer       buffer     = VK_NULL_HANDLE;
        VmaAllocation  alloc      = VK_NULL_HANDLE;
        void*          mapped     = nullptr;  // persistent map for host-visible memory
        u64            size       = 0;
        u32            generation = 0;
    };
    std::vector<BufferRecord> m_buffers;
    std::vector<u32>          m_buffers_free;

    struct TextureRecord {
        VkImage       image      = VK_NULL_HANDLE;
        VkImageView   view       = VK_NULL_HANDLE;
        VmaAllocation alloc      = VK_NULL_HANDLE;
        u32           generation = 0;
    };
    std::vector<TextureRecord> m_textures;
    std::vector<u32>           m_textures_free;

    struct SamplerRecord {
        VkSampler sampler    = VK_NULL_HANDLE;
        u32       generation = 0;
    };
    std::vector<SamplerRecord> m_samplers;
    std::vector<u32>           m_samplers_free;

    struct DescriptorSetLayoutRecord {
        VkDescriptorSetLayout layout     = VK_NULL_HANDLE;
        u32                   generation = 0;
    };
    std::vector<DescriptorSetLayoutRecord> m_dsl_records;
    std::vector<u32>                       m_dsl_free;

    struct DescriptorSetRecord {
        VkDescriptorSet set         = VK_NULL_HANDLE;
        VkDescriptorPool source     = VK_NULL_HANDLE;  // which pool the set lives in
        u32             generation  = 0;
    };
    std::vector<DescriptorSetRecord> m_dset_records;
    std::vector<u32>                 m_dset_free;

    // Descriptor pool ring. Grows on demand when allocation fails. All
    // pools share the same generous size class — text + UI + scene over
    // a long session typically need a few hundred sets total.
    std::vector<VkDescriptorPool> m_descriptor_pools;
    bool grow_descriptor_pool();

    struct PipelineLayoutRecord {
        VkPipelineLayout layout     = VK_NULL_HANDLE;
        u32              generation = 0;
    };
    std::vector<PipelineLayoutRecord> m_pl_records;
    std::vector<u32>                  m_pl_free;

    struct PipelineRecord {
        VkPipeline pipeline   = VK_NULL_HANDLE;
        u32        generation = 0;
    };
    std::vector<PipelineRecord> m_pipeline_records;
    std::vector<u32>            m_pipeline_free;
};

// ── Inline resolve() — hot path ────────────────────────────────────────────
// These get called once per cmd.bind_*/cmd.push_*/cmd.draw_* dispatch through
// CommandList. Inlining the 4-line lookup lets the optimizer fold the resolve
// + Vk call into a single function and skip the cross-TU jump.

inline VkShaderModule Rhi::resolve(ShaderModuleHandle h) const {
    if (!h.is_valid() || h.index >= m_shader_modules.size()) return VK_NULL_HANDLE;
    const auto& rec = m_shader_modules[h.index];
    return rec.generation == h.generation ? rec.module : VK_NULL_HANDLE;
}
inline VkBuffer Rhi::resolve(BufferHandle h) const {
    if (!h.is_valid() || h.index >= m_buffers.size()) return VK_NULL_HANDLE;
    const auto& rec = m_buffers[h.index];
    return rec.generation == h.generation ? rec.buffer : VK_NULL_HANDLE;
}
inline VkImage Rhi::resolve(TextureHandle h) const {
    if (!h.is_valid() || h.index >= m_textures.size()) return VK_NULL_HANDLE;
    const auto& rec = m_textures[h.index];
    return rec.generation == h.generation ? rec.image : VK_NULL_HANDLE;
}
inline VkImageView Rhi::resolve_view(TextureHandle h) const {
    if (!h.is_valid() || h.index >= m_textures.size()) return VK_NULL_HANDLE;
    const auto& rec = m_textures[h.index];
    return rec.generation == h.generation ? rec.view : VK_NULL_HANDLE;
}
inline VkSampler Rhi::resolve(SamplerHandle h) const {
    if (!h.is_valid() || h.index >= m_samplers.size()) return VK_NULL_HANDLE;
    const auto& rec = m_samplers[h.index];
    return rec.generation == h.generation ? rec.sampler : VK_NULL_HANDLE;
}
inline VkDescriptorSetLayout Rhi::resolve(DescriptorSetLayoutHandle h) const {
    if (!h.is_valid() || h.index >= m_dsl_records.size()) return VK_NULL_HANDLE;
    const auto& rec = m_dsl_records[h.index];
    return rec.generation == h.generation ? rec.layout : VK_NULL_HANDLE;
}
inline VkDescriptorSet Rhi::resolve(DescriptorSetHandle h) const {
    if (!h.is_valid() || h.index >= m_dset_records.size()) return VK_NULL_HANDLE;
    const auto& rec = m_dset_records[h.index];
    return rec.generation == h.generation ? rec.set : VK_NULL_HANDLE;
}
inline VkPipelineLayout Rhi::resolve(PipelineLayoutHandle h) const {
    if (!h.is_valid() || h.index >= m_pl_records.size()) return VK_NULL_HANDLE;
    const auto& rec = m_pl_records[h.index];
    return rec.generation == h.generation ? rec.layout : VK_NULL_HANDLE;
}
inline VkPipeline Rhi::resolve(PipelineHandle h) const {
    if (!h.is_valid() || h.index >= m_pipeline_records.size()) return VK_NULL_HANDLE;
    const auto& rec = m_pipeline_records[h.index];
    return rec.generation == h.generation ? rec.pipeline : VK_NULL_HANDLE;
}

} // namespace uldum::rhi
