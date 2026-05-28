#pragma once

// Private header — full definitions of Rhi's nested Record types for the
// GLES backend. Included by gles_rhi.cpp (which owns the record tables)
// and gles/command_list.cpp (which dereferences accessor returns). Not
// included by anything else — consumer code uses opaque handles.
//
// Splitting this out keeps GL types (GLuint / GLenum) out of the public
// gles_rhi.h while still letting both backend translation units see the
// full layout for member access.

#include "rhi/gles/gles_rhi.h"
#include "rhi/types.h"

#include <GLES3/gl32.h>

#include <vector>

namespace uldum::rhi {

struct Rhi::BufferRecord {
    GLuint name        = 0;
    GLenum target      = GL_ARRAY_BUFFER;
    // CPU shadow for host-visible buffers. The Vulkan backend gives consumers
    // a persistent VMA mapping; GL ES has no equivalent without EXT_buffer_
    // storage (which the Android emulator and many drivers lack). We allocate
    // a CPU buffer instead and expose its pointer from mapped_ptr(); the
    // shadow is pushed to the GL buffer (glBufferSubData) just before any
    // GL operation that reads it. `shadow` is empty for GpuOnly buffers.
    std::vector<u8> shadow;
    bool   shadow_dirty = false;  // set on map; cleared on sync
    u64    size        = 0;
    u32    generation  = 0;
};

struct Rhi::TextureRecord {
    GLuint name        = 0;
    GLenum target      = GL_TEXTURE_2D;
    GLenum internal_format = 0;
    u32    width       = 0;
    u32    height      = 0;
    u32    layers      = 1;
    u32    mips        = 1;
    u32    generation  = 0;
};

struct Rhi::SamplerRecord {
    GLuint name        = 0;
    u32    generation  = 0;
};

struct Rhi::DescriptorSetLayoutRecord {
    // GL ES has no descriptor sets; we record the binding declarations
    // (binding index, descriptor type, count) so update_descriptor_set
    // can populate the right binding-table slot at runtime.
    std::vector<DescriptorSetLayoutBinding> bindings;
    u32 generation = 0;
};

struct Rhi::DescriptorSetRecord {
    struct Binding {
        u32            binding       = 0;
        DescriptorType type          = DescriptorType::UniformBuffer;
        u32            array_element = 0;
        // Resolved at update time; replayed at bind time.
        TextureHandle texture;
        SamplerHandle sampler;
        BufferHandle  buffer;
        u64           buffer_offset = 0;
        u64           buffer_range  = 0;
    };
    DescriptorSetLayoutHandle layout{};
    std::vector<Binding>      bindings;
    u32                       generation = 0;
};

struct Rhi::PipelineLayoutRecord {
    // List of layouts in declaration order. CommandList uses this in
    // bind_descriptor_sets to map (set_index, binding) → flat GL binding.
    //
    // Convention: set N's binding M → GL binding (N * kBindingsPerSet + M).
    // kBindingsPerSet is generous (16) so most realistic layouts fit.
    // Push constants get a dedicated UBO at binding kPushConstantSlot.
    static constexpr u32 kBindingsPerSet  = 16;
    static constexpr u32 kPushConstantSlot = 30;  // reserved UBO

    std::vector<DescriptorSetLayoutHandle> set_layouts;
    std::vector<PushConstantRange>         push_constants;
    GLuint push_constant_ubo = 0;   // 0 if no push constants
    u32    push_constant_size = 0;  // total size across all ranges
    u32    generation = 0;
};

struct Rhi::PipelineRecord {
    GLuint program = 0;

    // Vertex format — applied via glVertexAttribPointer at draw time.
    // We can't pre-set up a VAO without the actual vertex buffer bound,
    // so we record the format and re-apply per draw. (For perf, a future
    // pass could cache VAOs keyed by (program, vertex-buffer GL name).)
    struct VertexAttr {
        u32 location;
        u32 binding;
        u32 offset;
        TextureFormat format;  // reused for vertex formats
    };
    std::vector<VertexBindingDesc>   vertex_bindings;
    std::vector<VertexAttr>          vertex_attrs;

    // Fixed-function state, replayed in bind_pipeline.
    PrimitiveTopology      topology = PrimitiveTopology::TriangleList;
    RasterizerState        rasterizer;
    DepthStencilState      depth_stencil;
    BlendAttachmentState   blend;  // only first attachment; ES has 1 default FBO
    MultisampleState       multisample;

    PipelineLayoutHandle layout;
    u32 generation = 0;
};

struct Rhi::ShaderModuleRecord {
    GLuint shader      = 0;
    GLenum stage       = GL_VERTEX_SHADER;
    u32    generation  = 0;
};

} // namespace uldum::rhi
