#include "ui/render_interface.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "core/log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>

namespace uldum::ui {

static constexpr const char* TAG = "UI";

// Keep in sync with Rml::Vertex (position f32×2, colour u8×4 premul, tex_coord f32×2).
static_assert(sizeof(Rml::Vertex) == 20, "Rml::Vertex layout changed");

// ── Shader loading helper (mirrors the pattern in renderer.cpp) ──────────
static VkShaderModule load_shader(VkDevice device, std::string_view path) {
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) return VK_NULL_HANDLE;
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) {
        log::error(TAG, "UI shader not found: '{}'", path);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &mod);
    return mod;
}

RenderInterface::RenderInterface(rhi::VulkanRhi& rhi) : m_rhi(rhi) {}
RenderInterface::~RenderInterface() { shutdown(); }

bool RenderInterface::init() {
    if (!create_descriptor_layout()) { log::error(TAG, "desc layout create failed"); return false; }
    if (!create_descriptor_pool())   { log::error(TAG, "desc pool create failed"); return false; }
    if (!create_sampler())           { log::error(TAG, "sampler create failed"); return false; }
    if (!create_pipeline_layout())   { log::error(TAG, "pipe layout create failed"); return false; }
    if (!create_pipeline())          { log::error(TAG, "pipeline create failed"); return false; }
    if (!create_white_texture())     { log::error(TAG, "white texture create failed"); return false; }
    return true;
}

void RenderInterface::shutdown() {
    VkDevice device = m_rhi.device();
    if (device == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device);

    for (auto& [h, g] : m_geometries) destroy_geometry(g);
    m_geometries.clear();
    for (auto& [h, t] : m_textures)   destroy_texture(t);
    m_textures.clear();
    destroy_texture(m_white);

    if (m_pipeline)        { vkDestroyPipeline(device, m_pipeline, nullptr);              m_pipeline = VK_NULL_HANDLE; }
    if (m_pipeline_layout) { vkDestroyPipelineLayout(device, m_pipeline_layout, nullptr); m_pipeline_layout = VK_NULL_HANDLE; }
    if (m_sampler)         { vkDestroySampler(device, m_sampler, nullptr);                m_sampler = VK_NULL_HANDLE; }
    if (m_desc_pool)       { vkDestroyDescriptorPool(device, m_desc_pool, nullptr);       m_desc_pool = VK_NULL_HANDLE; }
    if (m_desc_layout)     { vkDestroyDescriptorSetLayout(device, m_desc_layout, nullptr); m_desc_layout = VK_NULL_HANDLE; }
}

// ── One-time setup ───────────────────────────────────────────────────────

bool RenderInterface::create_descriptor_layout() {
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 1;
    ci.pBindings    = &b;
    return vkCreateDescriptorSetLayout(m_rhi.device(), &ci, nullptr, &m_desc_layout) == VK_SUCCESS;
}

bool RenderInterface::create_descriptor_pool() {
    // 128 texture sets — well beyond what Shell UI typically needs. Each RML
    // document uses a handful (font atlas + any image decorators).
    VkDescriptorPoolSize sz{};
    sz.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sz.descriptorCount = 128;

    VkDescriptorPoolCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets       = 128;
    ci.poolSizeCount = 1;
    ci.pPoolSizes    = &sz;
    return vkCreateDescriptorPool(m_rhi.device(), &ci, nullptr, &m_desc_pool) == VK_SUCCESS;
}

bool RenderInterface::create_sampler() {
    VkSamplerCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter    = VK_FILTER_LINEAR;
    ci.minFilter    = VK_FILTER_LINEAR;
    ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(m_rhi.device(), &ci, nullptr, &m_sampler) == VK_SUCCESS;
}

bool RenderInterface::create_pipeline_layout() {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo ci{};
    ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount         = 1;
    ci.pSetLayouts            = &m_desc_layout;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges    = &pc;
    return vkCreatePipelineLayout(m_rhi.device(), &ci, nullptr, &m_pipeline_layout) == VK_SUCCESS;
}

bool RenderInterface::create_pipeline() {
    VkDevice device = m_rhi.device();
    VkShaderModule vert = load_shader(device, "engine/shaders/ui_shell.vert.spv");
    VkShaderModule frag = load_shader(device, "engine/shaders/ui_shell.frag.spv");
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(Rml::Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0;  // pos
    attrs[0].binding  = 0;
    attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset   = offsetof(Rml::Vertex, position);
    attrs[1].location = 1;  // color (u8×4 normalized → vec4 in shader)
    attrs[1].binding  = 0;
    attrs[1].format   = VK_FORMAT_R8G8B8A8_UNORM;
    attrs[1].offset   = offsetof(Rml::Vertex, colour);
    attrs[2].location = 2;  // uv
    attrs[2].binding  = 0;
    attrs[2].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset   = offsetof(Rml::Vertex, tex_coord);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = m_rhi.msaa_samples();

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    // Premultiplied alpha blend.
    VkPipelineColorBlendAttachmentState ba{};
    ba.blendEnable         = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.colorBlendOp        = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ba.alphaBlendOp        = VK_BLEND_OP_ADD;
    ba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                           | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &ba;

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkFormat color_format = m_rhi.swapchain_format();
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = 1;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat   = m_rhi.depth_format();

    VkGraphicsPipelineCreateInfo ci{};
    ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext               = &rendering;
    ci.stageCount          = 2;
    ci.pStages             = stages;
    ci.pVertexInputState   = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState      = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState   = &ms;
    ci.pDepthStencilState  = &ds;
    ci.pColorBlendState    = &cb;
    ci.pDynamicState       = &dyn;
    ci.layout              = m_pipeline_layout;

    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &m_pipeline);
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    if (r != VK_SUCCESS) {
        log::error(TAG, "vkCreateGraphicsPipelines (UI) failed: {}", static_cast<int>(r));
        return false;
    }
    return true;
}

// ── Texture helpers ──────────────────────────────────────────────────────

VkDescriptorSet RenderInterface::allocate_texture_set(VkImageView view) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = m_desc_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &m_desc_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_rhi.device(), &ai, &set) != VK_SUCCESS) {
        log::error(TAG, "UI descriptor pool exhausted (raise size?)");
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo img{};
    img.sampler     = m_sampler;
    img.imageView   = view;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = set;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &img;
    vkUpdateDescriptorSets(m_rhi.device(), 1, &w, 0, nullptr);
    return set;
}

RenderInterface::Texture RenderInterface::create_texture_from_rgba(const u8* rgba, u32 w, u32 h) {
    Texture tex{};
    VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;

    // Staging buffer (host visible).
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;  // gives us pMappedData
    VkBuffer stage = VK_NULL_HANDLE;
    VmaAllocation stage_alloc = VK_NULL_HANDLE;
    VmaAllocationInfo stage_info{};
    if (vmaCreateBuffer(m_rhi.allocator(), &bci, &aci, &stage, &stage_alloc, &stage_info) != VK_SUCCESS) {
        log::error(TAG, "staging buffer create failed");
        return tex;
    }
    if (!stage_info.pMappedData) {
        log::error(TAG, "staging buffer not mapped");
        vmaDestroyBuffer(m_rhi.allocator(), stage, stage_alloc);
        return tex;
    }
    std::memcpy(stage_info.pMappedData, rgba, size);

    // Image (device-local).
    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent        = { w, h, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_aci{};
    img_aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_rhi.allocator(), &ici, &img_aci, &tex.image, &tex.alloc, nullptr) != VK_SUCCESS) {
        log::error(TAG, "image create failed");
        vmaDestroyBuffer(m_rhi.allocator(), stage, stage_alloc);
        return tex;
    }

    // Upload via one-shot command buffer: UNDEFINED → TRANSFER_DST → copy → SHADER_READ_ONLY.
    VkCommandBuffer cmd = m_rhi.begin_oneshot();
    {
        VkImageMemoryBarrier to_xfer{};
        to_xfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_xfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        to_xfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_xfer.srcAccessMask       = 0;
        to_xfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_xfer.image               = tex.image;
        to_xfer.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_xfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_xfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_xfer);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent      = { w, h, 1 };
        vkCmdCopyBufferToImage(cmd, stage, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        VkImageMemoryBarrier to_read{};
        to_read.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_read.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        to_read.image               = tex.image;
        to_read.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_read);
    }
    m_rhi.end_oneshot(cmd);

    vmaDestroyBuffer(m_rhi.allocator(), stage, stage_alloc);

    // View + descriptor set.
    VkImageViewCreateInfo vci{};
    vci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image            = tex.image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_rhi.device(), &vci, nullptr, &tex.view) != VK_SUCCESS) {
        log::error(TAG, "image view create failed");
        vmaDestroyImage(m_rhi.allocator(), tex.image, tex.alloc);
        tex = {};
        return tex;
    }
    tex.set = allocate_texture_set(tex.view);
    return tex;
}

void RenderInterface::destroy_texture(Texture& tex) {
    if (tex.set)   { vkFreeDescriptorSets(m_rhi.device(), m_desc_pool, 1, &tex.set); tex.set = VK_NULL_HANDLE; }
    if (tex.view)  { vkDestroyImageView(m_rhi.device(), tex.view, nullptr);          tex.view = VK_NULL_HANDLE; }
    if (tex.image) { vmaDestroyImage(m_rhi.allocator(), tex.image, tex.alloc);       tex.image = VK_NULL_HANDLE; tex.alloc = VK_NULL_HANDLE; }
}

bool RenderInterface::create_white_texture() {
    u8 pixel[4] = {255, 255, 255, 255};
    m_white = create_texture_from_rgba(pixel, 1, 1);
    return m_white.set != VK_NULL_HANDLE;
}

// ── Geometry ─────────────────────────────────────────────────────────────

void RenderInterface::destroy_geometry(Geometry& g) {
    if (g.vb) vmaDestroyBuffer(m_rhi.allocator(), g.vb, g.vb_alloc);
    if (g.ib) vmaDestroyBuffer(m_rhi.allocator(), g.ib, g.ib_alloc);
    g = {};
}

Rml::CompiledGeometryHandle RenderInterface::CompileGeometry(
    Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    Geometry g{};
    g.index_count = static_cast<u32>(indices.size());

    auto make = [&](VkBufferUsageFlags usage, VkDeviceSize size, const void* src,
                    VkBuffer& buf, VmaAllocation& alloc) -> bool {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = size;
        bci.usage = usage;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;  // host-visible, good for UI volumes
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_rhi.allocator(), &bci, &aci, &buf, &alloc, &info) != VK_SUCCESS)
            return false;
        if (!info.pMappedData) return false;
        std::memcpy(info.pMappedData, src, size);
        return true;
    };

    VkDeviceSize vb_size = vertices.size() * sizeof(Rml::Vertex);
    VkDeviceSize ib_size = indices.size()  * sizeof(u32);
    if (!make(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vb_size, vertices.data(), g.vb, g.vb_alloc)) return 0;

    // RmlUi indices are `int`; Vulkan wants u32. Same size, trivially copy.
    if (!make(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, ib_size, indices.data(), g.ib, g.ib_alloc)) {
        vmaDestroyBuffer(m_rhi.allocator(), g.vb, g.vb_alloc);
        return 0;
    }

    auto handle = m_next_geom++;
    m_geometries.emplace(handle, g);
    return handle;
}

void RenderInterface::ensure_pipeline_bound() {
    if (m_pipeline_bound) return;
    m_pipeline_bound = true;
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

    VkViewport vp{};
    vp.width    = static_cast<float>(m_extent.width);
    vp.height   = static_cast<float>(m_extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(m_cmd, 0, 1, &vp);

    // Default scissor covers full viewport until SetScissorRegion overrides.
    m_scissor         = { {0, 0}, m_extent };
    m_scissor_enabled = false;
    vkCmdSetScissor(m_cmd, 0, 1, &m_scissor);
}

void RenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry,
                                     Rml::Vector2f translation,
                                     Rml::TextureHandle texture)
{
    auto it = m_geometries.find(geometry);
    if (it == m_geometries.end()) return;
    auto& g = it->second;

    ensure_pipeline_bound();

    // MVP = ortho * transform * translate(translation).
    // Vulkan's NDC has +Y down; glm::ortho is OpenGL-convention (+Y up), so
    // we swap bottom/top to make Y=0 map to the top of the screen and Y=H
    // to the bottom — matching RmlUi's window-pixel coordinates.
    glm::mat4 ortho = glm::ortho(0.0f, static_cast<float>(m_extent.width),
                                 0.0f, static_cast<float>(m_extent.height),
                                 -1.0f, 1.0f);
    glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(translation.x, translation.y, 0.0f));
    glm::mat4 mvp = ortho * m_transform * t;
    vkCmdPushConstants(m_cmd, m_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(glm::mat4), glm::value_ptr(mvp));

    // Apply scissor (track state to avoid rebinding when unchanged).
    vkCmdSetScissor(m_cmd, 0, 1, &m_scissor);

    VkDescriptorSet tex_set = m_white.set;
    if (texture != 0) {
        auto t_it = m_textures.find(texture);
        if (t_it != m_textures.end() && t_it->second.set) tex_set = t_it->second.set;
    }
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipeline_layout, 0, 1, &tex_set, 0, nullptr);

    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(m_cmd, 0, 1, &g.vb, &off);
    vkCmdBindIndexBuffer(m_cmd, g.ib, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(m_cmd, g.index_count, 1, 0, 0, 0);
}

void RenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
    auto it = m_geometries.find(geometry);
    if (it == m_geometries.end()) return;
    destroy_geometry(it->second);
    m_geometries.erase(it);
}

// ── Texture virtuals ─────────────────────────────────────────────────────

Rml::TextureHandle RenderInterface::LoadTexture(Rml::Vector2i& texture_dimensions,
                                                const Rml::String& source)
{
    auto* mgr = asset::AssetManager::instance();
    std::vector<u8> bytes;
    if (mgr) bytes = mgr->read_file_bytes(source.c_str());
    if (bytes.empty()) {
        // Filesystem fallback (mirrors FileInterface so loose files in dist work).
        std::FILE* f = std::fopen(source.c_str(), "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            auto n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            bytes.resize(static_cast<size_t>(n));
            std::fread(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }
    if (bytes.empty()) {
        log::warn(TAG, "LoadTexture: '{}' not found", source.c_str());
        return 0;
    }

    auto decoded = asset::load_texture_from_memory(bytes.data(), static_cast<u32>(bytes.size()));
    if (!decoded || decoded->channels != 4) {
        log::warn(TAG, "LoadTexture: '{}' decode failed / not RGBA", source.c_str());
        return 0;
    }
    Texture tex = create_texture_from_rgba(decoded->pixels.data(), decoded->width, decoded->height);
    if (tex.set == VK_NULL_HANDLE) return 0;

    texture_dimensions = Rml::Vector2i(static_cast<int>(decoded->width),
                                       static_cast<int>(decoded->height));
    auto handle = m_next_tex++;
    m_textures.emplace(handle, tex);
    return handle;
}

Rml::TextureHandle RenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                    Rml::Vector2i source_dimensions)
{
    // Used for the font glyph atlas. RmlUi passes raw RGBA bytes.
    Texture tex = create_texture_from_rgba(source.data(),
                                           static_cast<u32>(source_dimensions.x),
                                           static_cast<u32>(source_dimensions.y));
    if (tex.set == VK_NULL_HANDLE) return 0;
    auto handle = m_next_tex++;
    m_textures.emplace(handle, tex);
    return handle;
}

void RenderInterface::ReleaseTexture(Rml::TextureHandle texture) {
    auto it = m_textures.find(texture);
    if (it == m_textures.end()) return;
    destroy_texture(it->second);
    m_textures.erase(it);
}

// ── Scissor / transform ──────────────────────────────────────────────────

void RenderInterface::EnableScissorRegion(bool enable) {
    m_scissor_enabled = enable;
    if (!enable) m_scissor = { {0, 0}, m_extent };
}

void RenderInterface::SetScissorRegion(Rml::Rectanglei region) {
    m_scissor.offset.x      = region.Left();
    m_scissor.offset.y      = region.Top();
    m_scissor.extent.width  = static_cast<u32>(region.Width());
    m_scissor.extent.height = static_cast<u32>(region.Height());
}

void RenderInterface::SetTransform(const Rml::Matrix4f* transform) {
    if (transform) {
        // Rml::Matrix4f is column-major in RmlUi's default config. Reinterpret
        // as a glm::mat4 (also column-major). If RmlUi is built with
        // ROW_MAJOR_MATRICES we'd need to transpose; we build default.
        std::memcpy(&m_transform, transform, sizeof(glm::mat4));
    } else {
        m_transform = glm::mat4(1.0f);
    }
}

// ── Frame setup ──────────────────────────────────────────────────────────

void RenderInterface::begin_frame(VkCommandBuffer cmd, VkExtent2D extent) {
    m_cmd              = cmd;
    m_extent           = extent;
    m_pipeline_bound   = false;
    m_scissor_enabled  = false;
    m_scissor          = { {0, 0}, extent };
    m_transform        = glm::mat4(1.0f);
}

} // namespace uldum::ui
