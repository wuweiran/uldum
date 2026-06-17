#pragma once

// OpenGL ES 3.1 RHI backend (Android-only).
//
// Mirrors the public interface of the Vulkan backend in
// rhi/vulkan/vulkan_rhi.h — consumer code references `rhi::Rhi`,
// `rhi::CommandList`, etc., which the build-system selector in
// rhi/rhi.h resolves to whichever backend is active.
//
// Implementation status: SKELETON. Methods declared and the class
// shape established; most bodies are TODO stubs that log a warning
// and return invalid handles. Filling them in is its own multi-week
// project tracked outside the RHI abstraction work.
//
// Mapping notes (Vulkan → GL ES):
//   - VkBuffer            → GLuint (gen by glGenBuffers)
//   - VkImage             → GLuint (gen by glGenTextures)
//   - VkSampler           → GLuint (gen by glGenSamplers)
//   - VkPipeline          → encoded state struct + linked program
//   - VkPipelineLayout    → vector of binding-table mappings
//   - VkDescriptorSet     → vector of (binding-slot → texture/buffer) tuples
//   - VkCommandBuffer     → recorded command list (replayed inline)
//   - Push constants      → emulated via per-draw small UBO
//   - Pipeline barriers   → mostly no-ops (driver-managed in GL)
//   - Image layouts       → no-op (GL has no explicit layout concept)
//
// Shader format: GLSL ES 3.10 produced by spirv-cross at asset-pack
// time; the engine ships both SPIR-V (for Vulkan target) and GLSL ES
// (for this backend) inside engine.uldpak, picked by file extension.

#include "core/types.h"

#include <array>
#include <span>
#include <vector>

#include "rhi/handles.h"
#include "rhi/types.h"
#include "rhi/command_list.h"

namespace uldum::platform { class Platform; }

namespace uldum::rhi {

static constexpr u32 MAX_FRAMES_IN_FLIGHT = 2;

struct Config {
    bool enable_validation = true;  // GL_KHR_debug callback when supported
};

class Rhi {
public:
    Rhi();
    ~Rhi();
    Rhi(const Rhi&) = delete;
    Rhi& operator=(const Rhi&) = delete;

    bool init(const Config& config, platform::Platform& platform);
    void shutdown();

    // Frame management. begin_frame returns an invalid CommandList if the
    // surface needs recreation and the caller should skip the frame.
    CommandList begin_frame();
    void        begin_rendering();  // bind default framebuffer + clear
    void        end_frame();        // eglSwapBuffers

    // One-shot transfer / setup commands.
    CommandList begin_oneshot();
    void        end_oneshot(CommandList& cmd);

    // Lifecycle
    void handle_resize(u32 width, u32 height);
    void set_vsync(bool enabled);  // eglSwapInterval(1/0) — applies next swap
    void recreate_surface(platform::Platform& platform);
    void wait_idle();  // glFinish

    // Opaque native window pointer (ANativeWindow*). Compared against the
    // platform's latest handle to detect Android's post-background handoff.
    void* native_window_handle() const { return m_native_window; }

    // Surface info
    Extent2D      extent()        const { return m_extent; }
    TextureFormat swapchain_format() const;
    TextureFormat depth_format()     const;
    u32           msaa_samples() const { return m_msaa_samples; }
    u32           frame_index()  const { return m_frame_index; }

    // Resource factories
    BufferHandle  create_buffer(const BufferDesc& desc);
    void          destroy_buffer(BufferHandle h);
    void*         mapped_ptr(BufferHandle h) const;

    // GLES-only: push the CPU shadow of a host-visible buffer to the GL
    // object via glBufferSubData. Called by command_list.cpp before any
    // GL operation that reads the buffer (vertex / index / UBO / SSBO
    // binding, copy_buffer_to_image source, indirect draws). No-op if
    // the shadow is clean or the buffer is GpuOnly. Public because it's
    // used across translation units, but not part of the public RHI
    // contract — Vulkan has no equivalent (VMA gives a persistent map).
    void          sync_buffer_to_gpu(BufferHandle h);

    TextureHandle create_texture(const TextureDesc& desc);
    void          destroy_texture(TextureHandle h);

    SamplerHandle create_sampler(const SamplerDesc& desc);
    void          destroy_sampler(SamplerHandle h);

    ShaderModuleHandle create_shader_module(std::span<const u8> source);
    void               destroy_shader_module(ShaderModuleHandle h);

    DescriptorSetLayoutHandle create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc);
    void                      destroy_descriptor_set_layout(DescriptorSetLayoutHandle h);

    DescriptorSetHandle  allocate_descriptor_set(DescriptorSetLayoutHandle layout, u32 variable_count = 0);
    void                 update_descriptor_set(DescriptorSetHandle h,
                                                std::span<const WriteDescriptor> writes);
    void                 free_descriptor_set(DescriptorSetHandle h);

    PipelineLayoutHandle create_pipeline_layout(const PipelineLayoutDesc& desc);
    void                 destroy_pipeline_layout(PipelineLayoutHandle h);

    PipelineHandle       create_graphics_pipeline(const GraphicsPipelineDesc& desc);
    void                 destroy_pipeline(PipelineHandle h);

    // Internal accessors — used by gles/command_list.cpp to translate
    // CommandList method calls into glDraw / glBindTexture / etc.
    struct BufferRecord;
    struct TextureRecord;
    struct SamplerRecord;
    struct DescriptorSetLayoutRecord;
    struct DescriptorSetRecord;
    struct PipelineLayoutRecord;
    struct PipelineRecord;
    struct ShaderModuleRecord;

    const BufferRecord*              buffer_record(BufferHandle h) const;
    const TextureRecord*             texture_record(TextureHandle h) const;
    const SamplerRecord*             sampler_record(SamplerHandle h) const;
    const DescriptorSetRecord*       descriptor_set_record(DescriptorSetHandle h) const;
    const DescriptorSetLayoutRecord* descriptor_set_layout_record(DescriptorSetLayoutHandle h) const;
    const PipelineLayoutRecord*      pipeline_layout_record(PipelineLayoutHandle h) const;
    const PipelineRecord*            pipeline_record(PipelineHandle h) const;

    // Per-frame "current cmd state" used by CommandList. GL is immediate-
    // mode so all state changes are global — we record bindings here and
    // flush them at draw time (so e.g. bind_vertex_buffer can come *after*
    // bind_pipeline and still apply the right vertex format).
    struct CmdState {
        PipelineHandle pipeline{};
        // Index-buffer state (set by bind_index_buffer, consumed by draw_indexed)
        BufferHandle  index_buffer{};
        u64           index_offset = 0;
        // GLenum (== unsigned int) — kept as the raw type to avoid pulling
        // <GLES3/gl32.h> into the public header. 0x1405 = GL_UNSIGNED_INT.
        unsigned int  index_type   = 0x1405;
        // Vertex-buffer bindings: indexed by binding slot (0..N).
        struct VtxBind {
            BufferHandle buffer{};
            u64          offset = 0;
        };
        std::array<VtxBind, 8> vertex_buffers{};
        // Descriptor sets currently bound — replayed at draw time so the
        // pipeline layout's mapping from (set, binding) → GL slot can be
        // applied with the right resources.
        std::array<DescriptorSetHandle, 4> sets{};
        PipelineLayoutHandle               sets_layout{};  // layout used in last bind_descriptor_sets
        // Set true whenever pipeline / vertex / index bindings change so
        // draw / draw_indexed knows to rebuild VAO-style state.
        bool dirty = true;
    };
    CmdState& cmd_state() { return m_cmd_state; }

    // GLES baseInstance emulation. Writes `base` into the shared draw-info
    // UBO at slot kDrawInstanceInfoSlot so vertex shaders can add it to
    // gl_InstanceID. Called per-draw by command_list.cpp from
    // draw_indexed_indirect and friends. Cached to avoid redundant
    // glBufferSubData when consecutive draws share the same baseInstance.
    void set_base_instance(u32 base);

private:
    void* m_native_window = nullptr;
    Extent2D m_extent{};
    u32 m_msaa_samples = 1;
    u32 m_frame_index  = 0;
    // ES 3.1 core requires a non-zero VAO bound for draw calls. We create
    // one at init and keep it bound for the context's lifetime; vertex
    // format is reapplied per draw in command_list.cpp::apply_vertex_input.
    unsigned int m_default_vao = 0;  // GLuint without pulling in GL headers
    CmdState m_cmd_state;

    // Record tables live in the .cpp where the BufferRecord etc. structs
    // are defined fully. The header forward-declares them so consumer
    // builds don't pull in GLES headers transitively.
    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace uldum::rhi
