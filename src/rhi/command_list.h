#pragma once

// Backend-agnostic command recording. Wraps a backend command buffer
// (VkCommandBuffer today) and exposes the small subset of operations the
// engine actually records each frame: pipeline binding, descriptor binding,
// vertex / index input, dynamic viewport / scissor, push constants, draws,
// pipeline barriers, and a couple of one-shot copies.
//
// Methods take RHI handles and resolve them at the call site so consumers
// never touch raw Vk objects.

#include "core/types.h"
#include "rhi/handles.h"
#include "rhi/types.h"

#include <vulkan/vulkan.h>

#include <span>

namespace uldum::rhi {

class VulkanRhi;

// Pipeline stages — bitmask. Map to VkPipelineStageFlags2 inside the RHI.
enum class PipelineStage : u32 {
    None                  = 0,
    TopOfPipe             = 1u << 0,
    DrawIndirect          = 1u << 1,
    VertexInput           = 1u << 2,
    VertexShader          = 1u << 3,
    FragmentShader        = 1u << 4,
    EarlyFragmentTests    = 1u << 5,
    LateFragmentTests     = 1u << 6,
    ColorAttachmentOutput = 1u << 7,
    ComputeShader         = 1u << 8,
    Transfer              = 1u << 9,
    BottomOfPipe          = 1u << 10,
    AllGraphics           = 1u << 11,
    AllCommands           = 1u << 12,
};
constexpr PipelineStage operator|(PipelineStage a, PipelineStage b) {
    return static_cast<PipelineStage>(static_cast<u32>(a) | static_cast<u32>(b));
}

// Access masks — bitmask. Map to VkAccessFlags2 inside the RHI.
enum class AccessFlag : u32 {
    None                        = 0,
    ShaderRead                  = 1u << 0,
    ShaderWrite                 = 1u << 1,
    ColorAttachmentRead         = 1u << 2,
    ColorAttachmentWrite        = 1u << 3,
    DepthStencilAttachmentRead  = 1u << 4,
    DepthStencilAttachmentWrite = 1u << 5,
    TransferRead                = 1u << 6,
    TransferWrite               = 1u << 7,
    HostRead                    = 1u << 8,
    HostWrite                   = 1u << 9,
    MemoryRead                  = 1u << 10,
    MemoryWrite                 = 1u << 11,
};
constexpr AccessFlag operator|(AccessFlag a, AccessFlag b) {
    return static_cast<AccessFlag>(static_cast<u32>(a) | static_cast<u32>(b));
}

enum class ImageLayout : u8 {
    Undefined,
    General,
    ColorAttachmentOptimal,
    DepthStencilAttachmentOptimal,
    DepthStencilReadOnlyOptimal,
    DepthAttachmentOptimal,
    DepthReadOnlyOptimal,
    ShaderReadOnlyOptimal,
    TransferSrcOptimal,
    TransferDstOptimal,
    PresentSrcKHR,
};

enum class IndexType : u8 { U16, U32 };

enum class ImageAspect : u8 { Color, Depth, Stencil, DepthStencil };

struct ImageBarrier {
    TextureHandle image;
    PipelineStage src_stage    = PipelineStage::TopOfPipe;
    PipelineStage dst_stage    = PipelineStage::BottomOfPipe;
    AccessFlag    src_access   = AccessFlag::None;
    AccessFlag    dst_access   = AccessFlag::None;
    ImageLayout   old_layout   = ImageLayout::Undefined;
    ImageLayout   new_layout   = ImageLayout::Undefined;
    ImageAspect   aspect       = ImageAspect::Color;
    u32           base_mip     = 0;
    u32           level_count  = 1;
    u32           base_layer   = 0;
    u32           layer_count  = 1;
};

struct BufferImageCopy {
    u64 buffer_offset      = 0;
    i32 image_offset_x     = 0;
    i32 image_offset_y     = 0;
    i32 image_offset_z     = 0;
    u32 image_extent_w     = 1;
    u32 image_extent_h     = 1;
    u32 image_extent_d     = 1;
    u32 mip_level          = 0;
    u32 base_array_layer   = 0;
    u32 layer_count        = 1;
    ImageAspect aspect     = ImageAspect::Color;
};

// Lightweight value wrapper around a VkCommandBuffer + back-pointer to the
// RHI (for handle resolution). Passed by reference through draw paths.
class CommandList {
public:
    CommandList(VulkanRhi& rhi, VkCommandBuffer cmd) : m_rhi(&rhi), m_cmd(cmd) {}

    // Pipeline + descriptor binding
    void bind_pipeline(PipelineHandle pipeline);
    void bind_descriptor_sets(PipelineLayoutHandle layout, u32 first_set,
                              std::span<const DescriptorSetHandle> sets);
    void bind_descriptor_set(PipelineLayoutHandle layout, u32 set_index,
                             DescriptorSetHandle set) {
        bind_descriptor_sets(layout, set_index, std::span{&set, 1});
    }

    // Vertex / index input
    void bind_vertex_buffer(u32 binding, BufferHandle buf, u64 offset = 0);
    void bind_index_buffer(BufferHandle buf, u64 offset, IndexType type);

    // Dynamic state
    void set_viewport(f32 x, f32 y, f32 width, f32 height,
                      f32 min_depth = 0.0f, f32 max_depth = 1.0f);
    void set_scissor(i32 x, i32 y, u32 width, u32 height);

    // Push constants
    void push_constants(PipelineLayoutHandle layout, ShaderStage stages,
                        u32 offset, u32 size, const void* data);

    // Draw
    void draw(u32 vertex_count, u32 instance_count = 1,
              u32 first_vertex = 0, u32 first_instance = 0);
    void draw_indexed(u32 index_count, u32 instance_count = 1,
                      u32 first_index = 0, i32 vertex_offset = 0,
                      u32 first_instance = 0);
    void draw_indexed_indirect(BufferHandle buf, u64 offset,
                               u32 draw_count, u32 stride);

    // Barriers + copies
    void image_barriers(std::span<const ImageBarrier> barriers);
    void image_barrier(const ImageBarrier& b) { image_barriers(std::span{&b, 1}); }
    void copy_buffer_to_image(BufferHandle src, TextureHandle dst,
                              std::span<const BufferImageCopy> regions,
                              ImageLayout dst_layout = ImageLayout::TransferDstOptimal);
    void clear_color_image(TextureHandle image, f32 r, f32 g, f32 b, f32 a,
                           ImageLayout layout = ImageLayout::TransferDstOptimal);

    // Backend-specific accessor for ImGui or other libraries that need the
    // raw VkCommandBuffer. Stripping this is part of the later header-cleanup
    // pass once nothing outside the RHI imports vulkan.h directly.
    VkCommandBuffer raw() const { return m_cmd; }
    VulkanRhi&      rhi() const { return *m_rhi; }

private:
    VulkanRhi*      m_rhi;
    VkCommandBuffer m_cmd;
};

} // namespace uldum::rhi
