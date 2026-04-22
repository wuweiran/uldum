#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include "core/types.h"

#include <unordered_map>

namespace uldum::rhi { class VulkanRhi; }

namespace uldum::ui {

// Real Vulkan implementation of RmlUi's render contract.
//
// Pipeline:
//   - Vertex format matches Rml::Vertex (pos f32×2, color u8×4 premul, uv f32×2)
//   - ui_shell.vert + ui_shell.frag (shipped in engine.uldpak)
//   - Premultiplied-alpha blending, no depth, no cull
//   - Dynamic viewport + scissor
//   - Push constant: mat4 (ortho × transform × translate)
//
// Per-frame flow:
//   Shell::render() calls begin_frame(cmd, extent), then RmlUi's
//   Context::Render() which invokes our virtuals. First RenderGeometry
//   call of the frame binds the pipeline + viewport lazily.
class RenderInterface final : public Rml::RenderInterface {
public:
    explicit RenderInterface(rhi::VulkanRhi& rhi);
    ~RenderInterface() override;

    // One-time setup: pipeline, descriptor layout/pool, sampler, default
    // 1×1 white texture. Must be called after the RHI is up.
    bool init();
    void shutdown();

    // Set the active cmd buffer + viewport for this frame. Called by
    // Shell::render immediately before Context::Render().
    void begin_frame(VkCommandBuffer cmd, VkExtent2D extent);

    // ── Rml::RenderInterface ─────────────────────────────────────────────
    Rml::CompiledGeometryHandle CompileGeometry(
        Rml::Span<const Rml::Vertex> vertices,
        Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
                                   const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    struct Geometry {
        VkBuffer      vb         = VK_NULL_HANDLE;
        VmaAllocation vb_alloc   = VK_NULL_HANDLE;
        VkBuffer      ib         = VK_NULL_HANDLE;
        VmaAllocation ib_alloc   = VK_NULL_HANDLE;
        u32           index_count = 0;
    };
    struct Texture {
        VkImage         image  = VK_NULL_HANDLE;
        VmaAllocation   alloc  = VK_NULL_HANDLE;
        VkImageView     view   = VK_NULL_HANDLE;
        VkDescriptorSet set    = VK_NULL_HANDLE;
    };

    // Creation helpers
    bool create_descriptor_layout();
    bool create_descriptor_pool();
    bool create_sampler();
    bool create_pipeline_layout();
    bool create_pipeline();
    bool create_white_texture();
    VkDescriptorSet allocate_texture_set(VkImageView view);
    Texture create_texture_from_rgba(const u8* rgba, u32 w, u32 h);
    void    destroy_texture(Texture& tex);
    void    destroy_geometry(Geometry& g);

    // Lazy-bind pipeline + viewport once per frame (first RenderGeometry call).
    void ensure_pipeline_bound();

    rhi::VulkanRhi&       m_rhi;
    VkDescriptorSetLayout m_desc_layout     = VK_NULL_HANDLE;
    VkDescriptorPool      m_desc_pool       = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline        = VK_NULL_HANDLE;
    VkSampler             m_sampler         = VK_NULL_HANDLE;
    Texture               m_white;

    std::unordered_map<Rml::CompiledGeometryHandle, Geometry> m_geometries;
    std::unordered_map<Rml::TextureHandle, Texture>           m_textures;
    Rml::CompiledGeometryHandle m_next_geom = 1;
    Rml::TextureHandle          m_next_tex  = 1;

    // Per-frame state
    VkCommandBuffer m_cmd             = VK_NULL_HANDLE;
    VkExtent2D      m_extent          = {0, 0};
    bool            m_pipeline_bound  = false;
    bool            m_scissor_enabled = false;
    VkRect2D        m_scissor         = {};
    glm::mat4       m_transform       = glm::mat4(1.0f);
};

} // namespace uldum::ui
