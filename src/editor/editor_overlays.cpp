#include "editor/editor_overlays.h"

#include "render/gpu_texture.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "core/log.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstring>

namespace uldum::editor {

static constexpr const char* TAG = "EditorOverlays";

namespace {

// Reuse the engine's world-overlay shader pair — same vertex layout
// (vec3 pos + vec2 uv + RGBA8) and same push-constant block, so we
// don't ship a second pair of SPIR-V binaries inside engine.uldpak.
// The editor binds a 1x1 white texture to the sampler so the fragment
// shader's `texture * in_color` multiply collapses to in_color.
constexpr const char* kVertSpv = "engine/shaders/world_overlay.vert.spv";
constexpr const char* kFragSpv = "engine/shaders/world_overlay.frag.spv";

constexpr u32 kMaxVerts = 8 * 1024;   // 4096 line segments per frame
constexpr f32 kZBias    = 1.0f;       // small lift so wires don't z-fight terrain

struct Vertex {
    f32 x, y, z;
    f32 u, v;     // unused for lines; kept for shader-layout parity
    u32 rgba;     // premultiplied-alpha RGBA8
};
static_assert(sizeof(Vertex) == 24);

struct PushConstants {
    glm::mat4 mvp;
    glm::vec4 tint;
};

// Premultiplied alpha — matches the world_overlay shader's expectation
// (it produces premultiplied output for premultiplied-alpha blend).
u32 pack_premul_rgba(glm::vec4 c) {
    auto clamp = [](f32 v) { return v < 0 ? 0.0f : (v > 1 ? 1.0f : v); };
    f32 r = clamp(c.r), g = clamp(c.g), b = clamp(c.b), a = clamp(c.a);
    u32 R = static_cast<u32>(r * a * 255.0f + 0.5f);
    u32 G = static_cast<u32>(g * a * 255.0f + 0.5f);
    u32 B = static_cast<u32>(b * a * 255.0f + 0.5f);
    u32 A = static_cast<u32>(a     * 255.0f + 0.5f);
    return R | (G << 8) | (B << 16) | (A << 24);
}

} // namespace

struct EditorOverlays::Impl {
    rhi::VulkanRhi* rhi = nullptr;

    VkPipelineLayout      pipeline_layout = VK_NULL_HANDLE;
    VkPipeline            pipeline        = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout     = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool       = VK_NULL_HANDLE;
    VkDescriptorSet       desc_set        = VK_NULL_HANDLE;

    // 1x1 white texture stand-in so the shared fragment shader's
    // sample is identity for line draws.
    render::GpuTexture    white_tex{};

    struct Frame {
        VkBuffer      vb     = VK_NULL_HANDLE;
        VmaAllocation alloc  = VK_NULL_HANDLE;
        Vertex*       mapped = nullptr;
    };
    std::array<Frame, rhi::MAX_FRAMES_IN_FLIGHT> frames{};

    u32 next_vertex = 0;
};

// ── Pipeline ─────────────────────────────────────────────────────────────

static VkShaderModule load_shader(VkDevice device, std::string_view path) {
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) return VK_NULL_HANDLE;
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) {
        log::error(TAG, "shader not found: '{}'", path);
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

static bool create_descriptors(EditorOverlays::Impl& s) {
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 1;
    lci.pBindings    = &b;
    if (vkCreateDescriptorSetLayout(s.rhi->device(), &lci, nullptr, &s.desc_layout) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize sz{};
    sz.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sz.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets       = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &sz;
    if (vkCreateDescriptorPool(s.rhi->device(), &pci, nullptr, &s.desc_pool) != VK_SUCCESS) {
        return false;
    }
    return true;
}

static bool create_white_texture(EditorOverlays::Impl& s) {
    const u8 white[4] = { 255, 255, 255, 255 };
    s.white_tex = render::upload_texture_rgba(*s.rhi, white, 1, 1,
                                              /*srgb=*/false, /*clamp=*/true);
    if (s.white_tex.image == VK_NULL_HANDLE) return false;

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = s.desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &s.desc_layout;
    if (vkAllocateDescriptorSets(s.rhi->device(), &dsai, &s.desc_set) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorImageInfo info{};
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.imageView   = s.white_tex.view;
    info.sampler     = s.white_tex.sampler;
    VkWriteDescriptorSet wr{};
    wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet          = s.desc_set;
    wr.dstBinding      = 0;
    wr.descriptorCount = 1;
    wr.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo      = &info;
    vkUpdateDescriptorSets(s.rhi->device(), 1, &wr, 0, nullptr);
    return true;
}

static bool create_pipeline(EditorOverlays::Impl& s) {
    VkDevice device = s.rhi->device();

    VkShaderModule vert = load_shader(device, kVertSpv);
    VkShaderModule frag = load_shader(device, kFragSpv);
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &s.desc_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pc;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &s.pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
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
    binding.stride    = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0; attrs[0].binding = 0;
    attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset   = offsetof(Vertex, x);
    attrs[1].location = 1; attrs[1].binding = 0;
    attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[1].offset   = offsetof(Vertex, u);
    attrs[2].location = 2; attrs[2].binding = 0;
    attrs[2].format   = VK_FORMAT_R8G8B8A8_UNORM;
    attrs[2].offset   = offsetof(Vertex, rgba);

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;  // wider needs the wideLines device feature

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = s.rhi->msaa_samples();

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

    // Premultiplied-alpha blend — matches the world_overlay shader's
    // output expectation, so the colors we pack must also be
    // premultiplied (see pack_premul_rgba).
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

    VkFormat color_format = s.rhi->swapchain_format();
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount    = 1;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat   = s.rhi->depth_format();

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
    ci.layout              = s.pipeline_layout;

    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &s.pipeline);
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    return r == VK_SUCCESS;
}

static bool create_buffers(EditorOverlays::Impl& s) {
    VkDeviceSize bytes = static_cast<VkDeviceSize>(kMaxVerts) * sizeof(Vertex);
    for (auto& f : s.frames) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = bytes;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(s.rhi->allocator(), &bci, &aci, &f.vb, &f.alloc, &info) != VK_SUCCESS) {
            return false;
        }
        f.mapped = static_cast<Vertex*>(info.pMappedData);
    }
    return true;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

EditorOverlays::EditorOverlays() = default;
EditorOverlays::~EditorOverlays() { shutdown(); }

bool EditorOverlays::init(rhi::VulkanRhi& rhi) {
    m_impl = new Impl{};
    m_impl->rhi = &rhi;
    if (!create_descriptors(*m_impl))    { log::error(TAG, "descriptors failed"); return false; }
    if (!create_pipeline(*m_impl))       { log::error(TAG, "pipeline failed");    return false; }
    if (!create_buffers(*m_impl))        { log::error(TAG, "buffers failed");     return false; }
    if (!create_white_texture(*m_impl))  { log::error(TAG, "white tex failed");   return false; }
    log::info(TAG, "editor overlays initialized");
    return true;
}

void EditorOverlays::shutdown() {
    if (!m_impl) return;
    VkDevice device = m_impl->rhi ? m_impl->rhi->device() : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        render::destroy_texture(*m_impl->rhi, m_impl->white_tex);
        for (auto& f : m_impl->frames) {
            if (f.vb) vmaDestroyBuffer(m_impl->rhi->allocator(), f.vb, f.alloc);
        }
        if (m_impl->pipeline)        vkDestroyPipeline(device, m_impl->pipeline, nullptr);
        if (m_impl->pipeline_layout) vkDestroyPipelineLayout(device, m_impl->pipeline_layout, nullptr);
        if (m_impl->desc_pool)       vkDestroyDescriptorPool(device, m_impl->desc_pool, nullptr);
        if (m_impl->desc_layout)     vkDestroyDescriptorSetLayout(device, m_impl->desc_layout, nullptr);
    }
    delete m_impl;
    m_impl = nullptr;
}

// ── API ──────────────────────────────────────────────────────────────────

void EditorOverlays::begin_frame() {
    if (!m_impl) return;
    m_impl->next_vertex = 0;
}

void EditorOverlays::add_line(glm::vec3 a, glm::vec3 b, glm::vec4 color) {
    if (!m_impl || !m_impl->pipeline) return;
    if (m_impl->next_vertex + 2 > kMaxVerts) return;
    u32 slot = m_impl->rhi->frame_index();
    auto& f = m_impl->frames[slot];
    if (!f.mapped) return;
    u32 rgba = pack_premul_rgba(color);
    f.mapped[m_impl->next_vertex++] = { a.x, a.y, a.z + kZBias, 0.0f, 0.0f, rgba };
    f.mapped[m_impl->next_vertex++] = { b.x, b.y, b.z + kZBias, 0.0f, 0.0f, rgba };
}

void EditorOverlays::add_polyline(std::span<const glm::vec3> samples,
                                  glm::vec4 color, bool closed) {
    if (!m_impl || !m_impl->pipeline || samples.size() < 2) return;
    u32 segs = static_cast<u32>(samples.size() - 1) + (closed ? 1u : 0u);
    if (m_impl->next_vertex + segs * 2 > kMaxVerts) return;
    u32 slot = m_impl->rhi->frame_index();
    auto& f = m_impl->frames[slot];
    if (!f.mapped) return;
    u32 rgba = pack_premul_rgba(color);
    u32 cursor = m_impl->next_vertex;
    auto write = [&](glm::vec3 p) {
        f.mapped[cursor++] = { p.x, p.y, p.z + kZBias, 0.0f, 0.0f, rgba };
    };
    for (usize i = 0; i + 1 < samples.size(); ++i) {
        write(samples[i]);
        write(samples[i + 1]);
    }
    if (closed) {
        write(samples.back());
        write(samples.front());
    }
    m_impl->next_vertex = cursor;
}

// ── Draw ─────────────────────────────────────────────────────────────────

void EditorOverlays::draw(VkCommandBuffer cmd, const glm::mat4& view_projection) {
    if (!m_impl || !m_impl->pipeline || m_impl->next_vertex == 0) return;
    u32 slot = m_impl->rhi->frame_index();
    auto& f = m_impl->frames[slot];
    if (!f.mapped) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_impl->pipeline);
    VkExtent2D ex = m_impl->rhi->extent();
    VkViewport vp{};
    vp.width    = static_cast<f32>(ex.width);
    vp.height   = static_cast<f32>(ex.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, ex};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_impl->pipeline_layout, 0, 1,
                            &m_impl->desc_set, 0, nullptr);

    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &f.vb, &zero);

    PushConstants pc{};
    pc.mvp  = view_projection;
    pc.tint = glm::vec4(1.0f);
    vkCmdPushConstants(cmd, m_impl->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(PushConstants), &pc);
    vkCmdDraw(cmd, m_impl->next_vertex, 1, 0, 0);
}

} // namespace uldum::editor
