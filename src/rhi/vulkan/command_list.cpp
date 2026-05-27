#include "rhi/command_list.h"
#include "rhi/vulkan/vulkan_rhi.h"

namespace uldum::rhi {

// ── Stage / access translation ──────────────────────────────────────────

static VkPipelineStageFlags2 to_vk_stage2(PipelineStage s) {
    VkPipelineStageFlags2 r = 0;
    auto u = static_cast<u32>(s);
    if (u & static_cast<u32>(PipelineStage::TopOfPipe))             r |= VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    if (u & static_cast<u32>(PipelineStage::DrawIndirect))          r |= VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    if (u & static_cast<u32>(PipelineStage::VertexInput))           r |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    if (u & static_cast<u32>(PipelineStage::VertexShader))          r |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (u & static_cast<u32>(PipelineStage::FragmentShader))        r |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    if (u & static_cast<u32>(PipelineStage::EarlyFragmentTests))    r |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    if (u & static_cast<u32>(PipelineStage::LateFragmentTests))     r |= VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    if (u & static_cast<u32>(PipelineStage::ColorAttachmentOutput)) r |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (u & static_cast<u32>(PipelineStage::ComputeShader))         r |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (u & static_cast<u32>(PipelineStage::Transfer))              r |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    if (u & static_cast<u32>(PipelineStage::BottomOfPipe))          r |= VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    if (u & static_cast<u32>(PipelineStage::AllGraphics))           r |= VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    if (u & static_cast<u32>(PipelineStage::AllCommands))           r |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    if (r == 0) r = VK_PIPELINE_STAGE_2_NONE;
    return r;
}

static VkAccessFlags2 to_vk_access2(AccessFlag a) {
    VkAccessFlags2 r = 0;
    auto u = static_cast<u32>(a);
    if (u & static_cast<u32>(AccessFlag::ShaderRead))                  r |= VK_ACCESS_2_SHADER_READ_BIT;
    if (u & static_cast<u32>(AccessFlag::ShaderWrite))                 r |= VK_ACCESS_2_SHADER_WRITE_BIT;
    if (u & static_cast<u32>(AccessFlag::ColorAttachmentRead))         r |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
    if (u & static_cast<u32>(AccessFlag::ColorAttachmentWrite))        r |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    if (u & static_cast<u32>(AccessFlag::DepthStencilAttachmentRead))  r |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    if (u & static_cast<u32>(AccessFlag::DepthStencilAttachmentWrite)) r |= VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    if (u & static_cast<u32>(AccessFlag::TransferRead))                r |= VK_ACCESS_2_TRANSFER_READ_BIT;
    if (u & static_cast<u32>(AccessFlag::TransferWrite))               r |= VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (u & static_cast<u32>(AccessFlag::HostRead))                    r |= VK_ACCESS_2_HOST_READ_BIT;
    if (u & static_cast<u32>(AccessFlag::HostWrite))                   r |= VK_ACCESS_2_HOST_WRITE_BIT;
    if (u & static_cast<u32>(AccessFlag::MemoryRead))                  r |= VK_ACCESS_2_MEMORY_READ_BIT;
    if (u & static_cast<u32>(AccessFlag::MemoryWrite))                 r |= VK_ACCESS_2_MEMORY_WRITE_BIT;
    if (r == 0) r = VK_ACCESS_2_NONE;
    return r;
}

static VkImageLayout to_vk_layout(ImageLayout l) {
    switch (l) {
        case ImageLayout::Undefined:                      return VK_IMAGE_LAYOUT_UNDEFINED;
        case ImageLayout::General:                        return VK_IMAGE_LAYOUT_GENERAL;
        case ImageLayout::ColorAttachmentOptimal:         return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case ImageLayout::DepthStencilAttachmentOptimal:  return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case ImageLayout::DepthStencilReadOnlyOptimal:    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        case ImageLayout::DepthAttachmentOptimal:         return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        case ImageLayout::DepthReadOnlyOptimal:           return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        case ImageLayout::ShaderReadOnlyOptimal:          return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ImageLayout::TransferSrcOptimal:             return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ImageLayout::TransferDstOptimal:             return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ImageLayout::PresentSrcKHR:                  return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

static VkImageAspectFlags to_vk_aspect(ImageAspect a) {
    switch (a) {
        case ImageAspect::Color:        return VK_IMAGE_ASPECT_COLOR_BIT;
        case ImageAspect::Depth:        return VK_IMAGE_ASPECT_DEPTH_BIT;
        case ImageAspect::Stencil:      return VK_IMAGE_ASPECT_STENCIL_BIT;
        case ImageAspect::DepthStencil: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static VkShaderStageFlags to_vk_shader_stage(ShaderStage s) {
    VkShaderStageFlags r = 0;
    if (any(s, ShaderStage::Vertex))   r |= VK_SHADER_STAGE_VERTEX_BIT;
    if (any(s, ShaderStage::Fragment)) r |= VK_SHADER_STAGE_FRAGMENT_BIT;
    return r;
}

// ── Methods ──────────────────────────────────────────────────────────────

// Cast the backend-erased handle back to its native type. `m_cmd` is
// stored as `void*` in the public header so consumers don't see vulkan.h.
static inline VkCommandBuffer vk(void* p) { return static_cast<VkCommandBuffer>(p); }

void CommandList::bind_pipeline(PipelineHandle pipeline) {
    vkCmdBindPipeline(vk(m_cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, m_rhi->resolve(pipeline));
}

void CommandList::bind_descriptor_sets(PipelineLayoutHandle layout, u32 first_set,
                                       std::span<const DescriptorSetHandle> sets) {
    // 8 sets covers Vulkan's typical maxBoundDescriptorSets (4 on mobile, 8 on
    // desktop). If you hit this in practice, the pipeline-layout design is the
    // problem, not the buffer size.
    constexpr u32 kMax = 8;
    VkDescriptorSet vk_sets[kMax];
    const u32 n = std::min(static_cast<u32>(sets.size()), kMax);
    for (u32 i = 0; i < n; ++i) vk_sets[i] = m_rhi->resolve(sets[i]);
    vkCmdBindDescriptorSets(vk(m_cmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_rhi->resolve(layout), first_set, n, vk_sets, 0, nullptr);
}

void CommandList::bind_vertex_buffer(u32 binding, BufferHandle buf, u64 offset) {
    VkBuffer vb = m_rhi->resolve(buf);
    VkDeviceSize o = offset;
    vkCmdBindVertexBuffers(vk(m_cmd), binding, 1, &vb, &o);
}

void CommandList::bind_index_buffer(BufferHandle buf, u64 offset, IndexType type) {
    vkCmdBindIndexBuffer(vk(m_cmd), m_rhi->resolve(buf), offset,
                         type == IndexType::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void CommandList::set_viewport(f32 x, f32 y, f32 width, f32 height,
                               f32 min_depth, f32 max_depth) {
    VkViewport vp{};
    vp.x = x; vp.y = y; vp.width = width; vp.height = height;
    vp.minDepth = min_depth; vp.maxDepth = max_depth;
    vkCmdSetViewport(vk(m_cmd), 0, 1, &vp);
}

void CommandList::set_scissor(i32 x, i32 y, u32 width, u32 height) {
    VkRect2D r{{x, y}, {width, height}};
    vkCmdSetScissor(vk(m_cmd), 0, 1, &r);
}

void CommandList::push_constants(PipelineLayoutHandle layout, ShaderStage stages,
                                 u32 offset, u32 size, const void* data) {
    vkCmdPushConstants(vk(m_cmd), m_rhi->resolve(layout), to_vk_shader_stage(stages),
                       offset, size, data);
}

void CommandList::draw(u32 vertex_count, u32 instance_count,
                       u32 first_vertex, u32 first_instance) {
    vkCmdDraw(vk(m_cmd), vertex_count, instance_count, first_vertex, first_instance);
}

void CommandList::draw_indexed(u32 index_count, u32 instance_count,
                               u32 first_index, i32 vertex_offset, u32 first_instance) {
    vkCmdDrawIndexed(vk(m_cmd), index_count, instance_count, first_index, vertex_offset, first_instance);
}

void CommandList::draw_indexed_indirect(BufferHandle buf, u64 offset,
                                        u32 draw_count, u32 stride) {
    vkCmdDrawIndexedIndirect(vk(m_cmd), m_rhi->resolve(buf), offset, draw_count, stride);
}

void CommandList::image_barriers(std::span<const ImageBarrier> barriers) {
    // Heap fallback for the rare batch — most call sites issue 1–2 barriers.
    constexpr u32 kInline = 8;
    VkImageMemoryBarrier2 inline_bars[kInline];
    std::vector<VkImageMemoryBarrier2> heap_bars;
    const u32 n = static_cast<u32>(barriers.size());
    VkImageMemoryBarrier2* vk_bars = inline_bars;
    if (n > kInline) {
        heap_bars.resize(n);
        vk_bars = heap_bars.data();
    }
    for (u32 i = 0; i < n; ++i) {
        const auto& b = barriers[i];
        vk_bars[i] = {};
        vk_bars[i].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        vk_bars[i].srcStageMask     = to_vk_stage2(b.src_stage);
        vk_bars[i].srcAccessMask    = to_vk_access2(b.src_access);
        vk_bars[i].dstStageMask     = to_vk_stage2(b.dst_stage);
        vk_bars[i].dstAccessMask    = to_vk_access2(b.dst_access);
        vk_bars[i].oldLayout        = to_vk_layout(b.old_layout);
        vk_bars[i].newLayout        = to_vk_layout(b.new_layout);
        vk_bars[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vk_bars[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vk_bars[i].image            = m_rhi->resolve(b.image);
        vk_bars[i].subresourceRange = {
            to_vk_aspect(b.aspect), b.base_mip, b.level_count, b.base_layer, b.layer_count
        };
    }
    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = n;
    dep.pImageMemoryBarriers    = vk_bars;
    vkCmdPipelineBarrier2(vk(m_cmd), &dep);
}

void CommandList::copy_buffer_to_image(BufferHandle src, TextureHandle dst,
                                       std::span<const BufferImageCopy> regions,
                                       ImageLayout dst_layout) {
    // Inline fits cubemap (6) and small layer counts. Heap fallback covers
    // the terrain texture array (up to 16 layers) and anything bigger.
    constexpr u32 kInline = 8;
    VkBufferImageCopy inline_regions[kInline];
    std::vector<VkBufferImageCopy> heap_regions;
    const u32 n = static_cast<u32>(regions.size());
    VkBufferImageCopy* vk_regions = inline_regions;
    if (n > kInline) {
        heap_regions.resize(n);
        vk_regions = heap_regions.data();
    }
    for (u32 i = 0; i < n; ++i) {
        const auto& r = regions[i];
        vk_regions[i] = {};
        vk_regions[i].bufferOffset      = r.buffer_offset;
        vk_regions[i].imageSubresource  = { to_vk_aspect(r.aspect), r.mip_level, r.base_array_layer, r.layer_count };
        vk_regions[i].imageOffset       = { r.image_offset_x, r.image_offset_y, r.image_offset_z };
        vk_regions[i].imageExtent       = { r.image_extent_w, r.image_extent_h, r.image_extent_d };
    }
    vkCmdCopyBufferToImage(vk(m_cmd), m_rhi->resolve(src), m_rhi->resolve(dst),
                           to_vk_layout(dst_layout), n, vk_regions);
}

void CommandList::clear_color_image(TextureHandle image, f32 r, f32 g, f32 b, f32 a,
                                    ImageLayout layout) {
    VkClearColorValue clear{};
    clear.float32[0] = r; clear.float32[1] = g; clear.float32[2] = b; clear.float32[3] = a;
    VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(vk(m_cmd), m_rhi->resolve(image), to_vk_layout(layout), &clear, 1, &range);
}

static VkAttachmentLoadOp to_vk_load(LoadOp o) {
    switch (o) {
        case LoadOp::Load:     return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::Clear:    return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_LOAD;
}
static VkAttachmentStoreOp to_vk_store(StoreOp o) {
    switch (o) {
        case StoreOp::Store:    return VK_ATTACHMENT_STORE_OP_STORE;
        case StoreOp::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_STORE_OP_STORE;
}

void CommandList::begin_rendering(const RenderingDesc& desc) {
    constexpr u32 kInlineColor = 4;
    VkRenderingAttachmentInfo inline_color[kInlineColor];
    std::vector<VkRenderingAttachmentInfo> heap_color;
    const u32 nc = static_cast<u32>(desc.color_attachments.size());
    VkRenderingAttachmentInfo* vk_color = inline_color;
    if (nc > kInlineColor) {
        heap_color.resize(nc);
        vk_color = heap_color.data();
    }
    for (u32 i = 0; i < nc; ++i) {
        const auto& a = desc.color_attachments[i];
        vk_color[i] = {};
        vk_color[i].sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        vk_color[i].imageView   = m_rhi->resolve_view(a.image);
        vk_color[i].imageLayout = to_vk_layout(a.layout);
        vk_color[i].loadOp      = to_vk_load(a.load);
        vk_color[i].storeOp     = to_vk_store(a.store);
        vk_color[i].clearValue.color = {{ a.clear.r, a.clear.g, a.clear.b, a.clear.a }};
    }

    VkRenderingAttachmentInfo vk_depth{};
    if (desc.depth) {
        vk_depth.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        vk_depth.imageView   = m_rhi->resolve_view(desc.depth->image);
        vk_depth.imageLayout = to_vk_layout(desc.depth->layout);
        vk_depth.loadOp      = to_vk_load(desc.depth->load);
        vk_depth.storeOp     = to_vk_store(desc.depth->store);
        vk_depth.clearValue.depthStencil = { desc.depth->clear.depth, desc.depth->clear.stencil };
    }

    VkRenderingInfo rendering{};
    rendering.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea           = { { desc.area_x, desc.area_y },
                                       { desc.area_width, desc.area_height } };
    rendering.layerCount           = desc.layer_count;
    rendering.colorAttachmentCount = nc;
    rendering.pColorAttachments    = (nc > 0) ? vk_color : nullptr;
    rendering.pDepthAttachment     = desc.depth ? &vk_depth : nullptr;
    vkCmdBeginRendering(vk(m_cmd), &rendering);
}

void CommandList::end_rendering() {
    vkCmdEndRendering(vk(m_cmd));
}

} // namespace uldum::rhi
