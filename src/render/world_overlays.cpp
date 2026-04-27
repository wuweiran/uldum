#include "render/world_overlays.h"

#include "render/gpu_texture.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "asset/texture.h"
#include "core/log.h"

#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace uldum::render {

static constexpr const char* TAG = "WorldOverlays";

namespace {

constexpr const char* kVertSpv = "engine/shaders/world_overlay.vert.spv";
constexpr const char* kFragSpv = "engine/shaders/world_overlay.frag.spv";

constexpr u32 kMaxVerts  = 16 * 1024;     // per-frame vertex ceiling
constexpr u32 kMaxDraws  = 256;           // per-frame draw ceiling
constexpr f32 kZBias     = 2.0f;          // raise off terrain to avoid z-fight

// Vertex layout — matches world_overlay.vert. RGBA8 is premultiplied
// alpha, packed in one u32 (R in low byte, A in high).
struct Vertex {
    f32 x, y, z;
    f32 u, v;
    u32 rgba;
};
static_assert(sizeof(Vertex) == 24);

struct PushConstants {
    glm::mat4 mvp;
    glm::vec4 tint;
};

struct DrawCmd {
    u32             first_vertex;
    u32             vertex_count;
    VkDescriptorSet desc_set;
    glm::vec4       tint;
};

u32 pack_premul_rgba(f32 r, f32 g, f32 b, f32 a) {
    if (r < 0) r = 0; else if (r > 1) r = 1;
    if (g < 0) g = 0; else if (g > 1) g = 1;
    if (b < 0) b = 0; else if (b > 1) b = 1;
    if (a < 0) a = 0; else if (a > 1) a = 1;
    u32 R = static_cast<u32>(r * a * 255.0f + 0.5f);
    u32 G = static_cast<u32>(g * a * 255.0f + 0.5f);
    u32 B = static_cast<u32>(b * a * 255.0f + 0.5f);
    u32 A = static_cast<u32>(a       * 255.0f + 0.5f);
    return R | (G << 8) | (B << 16) | (A << 24);
}

} // namespace

struct WorldOverlays::Impl {
    rhi::VulkanRhi* rhi = nullptr;

    VkPipelineLayout      pipeline_layout = VK_NULL_HANDLE;
    VkPipeline            pipeline        = VK_NULL_HANDLE;
    VkDescriptorSetLayout desc_layout     = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool       = VK_NULL_HANDLE;

    // One default texture per TextureId — generated procedurally at
    // init. Future: expose a setter so a map's hud.json can override
    // by replacing the GpuTexture / desc_set at a slot.
    struct Decal {
        GpuTexture       tex{};
        VkDescriptorSet  set = VK_NULL_HANDLE;
    };
    std::array<Decal, static_cast<usize>(WorldOverlays::TextureId::kCount)> decals{};

    // Per-frame mapped VBO; written each frame, drawn once.
    struct Frame {
        VkBuffer       vb     = VK_NULL_HANDLE;
        VmaAllocation  alloc  = VK_NULL_HANDLE;
        Vertex*        mapped = nullptr;
    };
    std::array<Frame, rhi::MAX_FRAMES_IN_FLIGHT> frames{};

    u32                  next_vertex = 0;
    std::vector<DrawCmd> cmds;
};

// ── Pipeline ─────────────────────────────────────────────────────────────

static VkShaderModule load_shader(VkDevice device, std::string_view path) {
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) return VK_NULL_HANDLE;
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) { log::error(TAG, "shader not found: '{}'", path); return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &mod);
    return mod;
}

static bool create_descriptor_layout(WorldOverlays::Impl& s) {
    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ci.bindingCount = 1;
    ci.pBindings    = &b;
    return vkCreateDescriptorSetLayout(s.rhi->device(), &ci, nullptr, &s.desc_layout) == VK_SUCCESS;
}

static bool create_descriptor_pool(WorldOverlays::Impl& s) {
    constexpr u32 kSlots = static_cast<u32>(WorldOverlays::TextureId::kCount);
    VkDescriptorPoolSize sz{};
    sz.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sz.descriptorCount = kSlots;

    VkDescriptorPoolCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets       = kSlots;
    ci.poolSizeCount = 1;
    ci.pPoolSizes    = &sz;
    return vkCreateDescriptorPool(s.rhi->device(), &ci, nullptr, &s.desc_pool) == VK_SUCCESS;
}

static bool create_pipeline(WorldOverlays::Impl& s) {
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
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = s.rhi->msaa_samples();

    // Same depth setup as the previous ground-decal pipelines: test
    // against scene depth so 3D geometry occludes the indicator, but
    // no write so subsequent draws aren't affected.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;

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

static bool create_buffers(WorldOverlays::Impl& s) {
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

WorldOverlays::WorldOverlays() = default;
WorldOverlays::~WorldOverlays() { shutdown(); }

bool WorldOverlays::init(rhi::VulkanRhi& rhi) {
    m_impl = new Impl{};
    m_impl->rhi = &rhi;
    if (!create_descriptor_layout(*m_impl)) { log::error(TAG, "desc layout failed"); return false; }
    if (!create_descriptor_pool(*m_impl))   { log::error(TAG, "desc pool failed");   return false; }
    if (!create_pipeline(*m_impl))          { log::error(TAG, "pipeline failed");    return false; }
    if (!create_buffers(*m_impl))           { log::error(TAG, "buffers failed");     return false; }
    // No engine-side default decals. Each slot is unbound until a
    // map's hud.json calls set_texture(); add_* calls referencing an
    // unbound slot are silently dropped. The engine's own
    // default-texture / map-override merging mechanism will fill the
    // gap once it lands.
    log::info(TAG, "world overlays initialized");
    return true;
}

void WorldOverlays::shutdown() {
    if (!m_impl) return;
    VkDevice device = m_impl->rhi ? m_impl->rhi->device() : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        for (auto& d : m_impl->decals) {
            destroy_texture(*m_impl->rhi, d.tex);
            // descriptor set freed implicitly with the pool
        }
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

// ── Geometry helpers ──────────────────────────────────────────────────────

namespace {

u32 append(WorldOverlays::Impl& s, u32 count) {
    if (s.next_vertex + count > kMaxVerts) return UINT32_MAX;
    u32 first = s.next_vertex;
    s.next_vertex += count;
    return first;
}

void write_v(WorldOverlays::Impl& s, u32 slot, u32 i,
             glm::vec3 p, f32 u, f32 v, u32 rgba) {
    auto& f = s.frames[slot];
    if (!f.mapped) return;
    f.mapped[i] = { p.x, p.y, p.z + kZBias, u, v, rgba };
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────

bool WorldOverlays::set_texture(TextureId id, std::string_view path) {
    if (!m_impl || !m_impl->rhi || path.empty()) return false;
    if (static_cast<usize>(id) >= m_impl->decals.size()) return false;

    auto* mgr = asset::AssetManager::instance();
    if (!mgr) return false;
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) {
        log::warn(TAG, "set_texture: '{}' not found", path);
        return false;
    }
    auto decoded = asset::load_texture_from_memory(bytes.data(),
                                                   static_cast<u32>(bytes.size()));
    if (!decoded) {
        log::warn(TAG, "set_texture: decode '{}' failed: {}", path, decoded.error());
        return false;
    }
    if (decoded->channels != 4) {
        log::warn(TAG, "set_texture: '{}' has {} channels, expected 4", path, decoded->channels);
        return false;
    }

    GpuTexture new_tex = upload_texture_rgba(*m_impl->rhi,
                                             decoded->pixels.data(),
                                             decoded->width, decoded->height,
                                             /*srgb=*/false, /*clamp=*/true);
    if (new_tex.image == VK_NULL_HANDLE) {
        log::warn(TAG, "set_texture: upload of '{}' failed", path);
        return false;
    }

    // Wait for in-flight frames to finish referencing the old image
    // before destroying it. This is rare (called once per session at
    // map load), so the stall is acceptable.
    vkDeviceWaitIdle(m_impl->rhi->device());
    auto& d = m_impl->decals[static_cast<usize>(id)];
    destroy_texture(*m_impl->rhi, d.tex);
    d.tex = new_tex;

    // Allocate the slot's descriptor set on first use; on subsequent
    // calls we just re-point the existing set at the new image.
    if (d.set == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = m_impl->desc_pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts        = &m_impl->desc_layout;
        if (vkAllocateDescriptorSets(m_impl->rhi->device(), &dsai, &d.set) != VK_SUCCESS) {
            log::warn(TAG, "set_texture: descriptor set alloc failed for slot {}",
                      static_cast<u32>(id));
            destroy_texture(*m_impl->rhi, d.tex);
            d.tex = {};
            return false;
        }
    }

    VkDescriptorImageInfo info{};
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    info.imageView   = d.tex.view;
    info.sampler     = d.tex.sampler;
    VkWriteDescriptorSet wr{};
    wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet          = d.set;
    wr.dstBinding      = 0;
    wr.descriptorCount = 1;
    wr.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo      = &info;
    vkUpdateDescriptorSets(m_impl->rhi->device(), 1, &wr, 0, nullptr);

    log::info(TAG, "set_texture[{}] = '{}' ({}x{})",
              static_cast<u32>(id), path, decoded->width, decoded->height);
    return true;
}

void WorldOverlays::begin_frame() {
    if (!m_impl) return;
    m_impl->next_vertex = 0;
    m_impl->cmds.clear();
}

void WorldOverlays::add_ring(glm::vec3 center, f32 radius, f32 thickness,
                             glm::vec4 color, TextureId tex,
                             u32 samples_per_ring) {
    if (!m_impl || !m_impl->pipeline || radius <= 0.0f || thickness <= 0.0f) return;
    if (m_impl->decals[(usize)tex].set == VK_NULL_HANDLE) return;  // unbound slot
    if (samples_per_ring < 8) samples_per_ring = 8;
    u32 slot = m_impl->rhi->frame_index();
    u32 vert_count = samples_per_ring * 6;
    u32 first = append(*m_impl, vert_count);
    if (first == UINT32_MAX) return;
    if (m_impl->cmds.size() >= kMaxDraws) return;

    u32 rgba = pack_premul_rgba(color.r, color.g, color.b, color.a);
    f32 r_in  = std::max(0.0f, radius - thickness * 0.5f);
    f32 r_out = radius + thickness * 0.5f;
    constexpr f32 TWO_PI = 6.28318530718f;
    f32 step = TWO_PI / samples_per_ring;
    u32 cursor = first;
    for (u32 i = 0; i < samples_per_ring; ++i) {
        f32 a0 = step * i;
        f32 a1 = step * ((i + 1) % samples_per_ring);
        f32 v0 = (f32)i / (f32)samples_per_ring;
        f32 v1 = (f32)(i + 1) / (f32)samples_per_ring;
        f32 c0 = std::cos(a0), s0 = std::sin(a0);
        f32 c1 = std::cos(a1), s1 = std::sin(a1);
        glm::vec3 in0 { center.x + r_in  * c0, center.y + r_in  * s0, center.z };
        glm::vec3 out0{ center.x + r_out * c0, center.y + r_out * s0, center.z };
        glm::vec3 in1 { center.x + r_in  * c1, center.y + r_in  * s1, center.z };
        glm::vec3 out1{ center.x + r_out * c1, center.y + r_out * s1, center.z };
        // CCW: in0(u=0,v0), out0(u=1,v0), out1(u=1,v1), in1(u=0,v1)
        write_v(*m_impl, slot, cursor++, in0,  0.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, out0, 1.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, out1, 1.0f, v1, rgba);
        write_v(*m_impl, slot, cursor++, in0,  0.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, out1, 1.0f, v1, rgba);
        write_v(*m_impl, slot, cursor++, in1,  0.0f, v1, rgba);
    }
    m_impl->cmds.push_back({first, vert_count,
                            m_impl->decals[(usize)tex].set, glm::vec4(1.0f)});
}

void WorldOverlays::add_path(std::span<const glm::vec3> samples, f32 thickness,
                             glm::vec4 color, TextureId tex) {
    if (!m_impl || !m_impl->pipeline || samples.size() < 2 || thickness <= 0.0f) return;
    if (m_impl->decals[(usize)tex].set == VK_NULL_HANDLE) return;  // unbound slot
    u32 slot = m_impl->rhi->frame_index();
    u32 segs = static_cast<u32>(samples.size()) - 1;
    u32 vert_count = segs * 6;
    u32 first = append(*m_impl, vert_count);
    if (first == UINT32_MAX) return;
    if (m_impl->cmds.size() >= kMaxDraws) return;

    u32 rgba = pack_premul_rgba(color.r, color.g, color.b, color.a);
    f32 hw = thickness * 0.5f;
    u32 cursor = first;
    for (u32 i = 0; i < segs; ++i) {
        glm::vec3 p0 = samples[i];
        glm::vec3 p1 = samples[i + 1];
        f32 v0 = (f32)i / (f32)segs;
        f32 v1 = (f32)(i + 1) / (f32)segs;
        glm::vec3 d  = p1 - p0;
        d.z = 0;
        f32 len2 = d.x * d.x + d.y * d.y;
        glm::vec3 side{ 0.0f, 0.0f, 0.0f };
        if (len2 >= 1e-4f) {
            f32 inv = 1.0f / std::sqrt(len2);
            side = glm::vec3{ -d.y * inv, d.x * inv, 0.0f };
        }
        glm::vec3 a0 = p0 - side * hw;
        glm::vec3 b0 = p0 + side * hw;
        glm::vec3 a1 = p1 - side * hw;
        glm::vec3 b1 = p1 + side * hw;
        // CCW: a0(u=0,v0), b0(u=1,v0), b1(u=1,v1), a1(u=0,v1)
        write_v(*m_impl, slot, cursor++, a0, 0.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, b0, 1.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, b1, 1.0f, v1, rgba);
        write_v(*m_impl, slot, cursor++, a0, 0.0f, v0, rgba);
        write_v(*m_impl, slot, cursor++, b1, 1.0f, v1, rgba);
        write_v(*m_impl, slot, cursor++, a1, 0.0f, v1, rgba);
    }
    m_impl->cmds.push_back({first, vert_count,
                            m_impl->decals[(usize)tex].set, glm::vec4(1.0f)});
}

void WorldOverlays::add_quad(glm::vec3 center, f32 half_extent,
                             glm::vec4 color, TextureId tex) {
    if (!m_impl || !m_impl->pipeline || half_extent <= 0.0f) return;
    if (m_impl->decals[(usize)tex].set == VK_NULL_HANDLE) return;  // unbound slot
    u32 slot = m_impl->rhi->frame_index();
    u32 vert_count = 6;
    u32 first = append(*m_impl, vert_count);
    if (first == UINT32_MAX) return;
    if (m_impl->cmds.size() >= kMaxDraws) return;

    u32 rgba = pack_premul_rgba(color.r, color.g, color.b, color.a);
    f32 e = half_extent;
    glm::vec3 a{ center.x - e, center.y - e, center.z };  // u=0,v=0
    glm::vec3 b{ center.x + e, center.y - e, center.z };  // u=1,v=0
    glm::vec3 c{ center.x + e, center.y + e, center.z };  // u=1,v=1
    glm::vec3 d{ center.x - e, center.y + e, center.z };  // u=0,v=1
    u32 cursor = first;
    write_v(*m_impl, slot, cursor++, a, 0.0f, 0.0f, rgba);
    write_v(*m_impl, slot, cursor++, b, 1.0f, 0.0f, rgba);
    write_v(*m_impl, slot, cursor++, c, 1.0f, 1.0f, rgba);
    write_v(*m_impl, slot, cursor++, a, 0.0f, 0.0f, rgba);
    write_v(*m_impl, slot, cursor++, c, 1.0f, 1.0f, rgba);
    write_v(*m_impl, slot, cursor++, d, 0.0f, 1.0f, rgba);
    m_impl->cmds.push_back({first, vert_count,
                            m_impl->decals[(usize)tex].set, glm::vec4(1.0f)});
}

void WorldOverlays::add_cone(glm::vec3 origin, glm::vec3 dir, f32 half_angle,
                             f32 radius, glm::vec4 color, TextureId tex,
                             u32 segments) {
    if (!m_impl || !m_impl->pipeline || radius <= 0.0f || half_angle <= 0.0f) return;
    if (m_impl->decals[(usize)tex].set == VK_NULL_HANDLE) return;  // unbound slot
    if (segments < 4) segments = 4;
    u32 slot = m_impl->rhi->frame_index();
    u32 vert_count = segments * 3;  // triangle fan as triangle list
    u32 first = append(*m_impl, vert_count);
    if (first == UINT32_MAX) return;
    if (m_impl->cmds.size() >= kMaxDraws) return;

    u32 rgba = pack_premul_rgba(color.r, color.g, color.b, color.a);
    // Normalize dir in XY.
    glm::vec3 d2 = dir; d2.z = 0;
    f32 len = std::sqrt(d2.x * d2.x + d2.y * d2.y);
    if (len < 1e-4f) return;
    glm::vec3 fwd = d2 / len;
    glm::vec3 side{ -fwd.y, fwd.x, 0.0f };

    u32 cursor = first;
    for (u32 i = 0; i < segments; ++i) {
        f32 t0 = (f32)i / (f32)segments;
        f32 t1 = (f32)(i + 1) / (f32)segments;
        // Map [0,1] → [-half_angle, +half_angle]
        f32 a0 = (t0 * 2.0f - 1.0f) * half_angle;
        f32 a1 = (t1 * 2.0f - 1.0f) * half_angle;
        f32 c0 = std::cos(a0), s0 = std::sin(a0);
        f32 c1 = std::cos(a1), s1 = std::sin(a1);
        glm::vec3 r0 = origin + (fwd * c0 + side * s0) * radius;
        glm::vec3 r1 = origin + (fwd * c1 + side * s1) * radius;
        // origin is u=0.5, v=0 (apex). Rim points: u=t, v=1.
        write_v(*m_impl, slot, cursor++, origin, 0.5f, 0.0f, rgba);
        write_v(*m_impl, slot, cursor++, r0,     t0,   1.0f, rgba);
        write_v(*m_impl, slot, cursor++, r1,     t1,   1.0f, rgba);
    }
    m_impl->cmds.push_back({first, vert_count,
                            m_impl->decals[(usize)tex].set, glm::vec4(1.0f)});
}

// ── Per-frame draw ────────────────────────────────────────────────────────

void WorldOverlays::draw(VkCommandBuffer cmd, const glm::mat4& view_projection) {
    if (!m_impl || !m_impl->pipeline || m_impl->cmds.empty()) return;
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

    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &f.vb, &zero);

    PushConstants pc{};
    pc.mvp = view_projection;
    VkDescriptorSet last_set = VK_NULL_HANDLE;
    for (const auto& d : m_impl->cmds) {
        if (d.desc_set != last_set) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_impl->pipeline_layout, 0, 1,
                                    &d.desc_set, 0, nullptr);
            last_set = d.desc_set;
        }
        pc.tint = d.tint;
        vkCmdPushConstants(cmd, m_impl->pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(PushConstants), &pc);
        vkCmdDraw(cmd, d.vertex_count, 1, d.first_vertex, 0);
    }
}

} // namespace uldum::render
