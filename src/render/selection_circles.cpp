#include "render/selection_circles.h"

#include "render/camera.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "simulation/world.h"
#include "simulation/components.h"
#include "map/terrain_data.h"
#include "asset/asset.h"
#include "core/log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/vec3.hpp>
#include <array>
#include <cmath>
#include <cstring>

namespace uldum::render {

static constexpr const char* TAG = "SelCircle";

namespace {

constexpr u32 kSegments     = 48;           // around the ring
constexpr f32 kStrokeWidth  = 4.0f;         // world units — constant across unit sizes so
                                            // small and large units get visually-equivalent rings
constexpr f32 kZBias        = 2.0f;         // world units up, to avoid z-fighting with terrain
constexpr u32 kVertsPerRing = kSegments * 2;
constexpr u32 kIdxPerRing   = kSegments * 6;
constexpr u32 kMaxRings     = 48;           // WC3 selection cap (24) × headroom

struct Vertex { f32 x, y, z; };
static_assert(sizeof(Vertex) == 12);

struct PushConstants {
    glm::mat4 mvp;       // view_projection (positions are already in world space)
    glm::vec4 color;     // premultiplied alpha
};

} // namespace

struct SelectionCircles::Impl {
    rhi::VulkanRhi* rhi = nullptr;

    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       pipeline        = VK_NULL_HANDLE;

    // One static index buffer — indices are local to each ring (0..kVertsPerRing-1);
    // per-draw `vertex_offset` shifts which ring's vertices they address.
    VkBuffer       ib       = VK_NULL_HANDLE;
    VmaAllocation  ib_alloc = VK_NULL_HANDLE;

    // Per-frame-in-flight dynamic vertex buffers. Each frame we rebuild
    // world-space ring vertices (terrain-conformed) for every selected
    // unit into the ring's own slot inside this VB, then issue one draw
    // per unit with its vertex_offset.
    struct Ring { VkBuffer vb = VK_NULL_HANDLE; VmaAllocation alloc = VK_NULL_HANDLE; void* mapped = nullptr; };
    std::array<Ring, rhi::MAX_FRAMES_IN_FLIGHT> ring_vbs{};
};

// ── Shader loader ────────────────────────────────────────────────────────
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

// ── Pipeline ─────────────────────────────────────────────────────────────
static bool create_pipeline(SelectionCircles::Impl& s) {
    VkDevice device = s.rhi->device();

    VkShaderModule vert = load_shader(device, "engine/shaders/selection_circle.vert.spv");
    VkShaderModule frag = load_shader(device, "engine/shaders/selection_circle.frag.spv");
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

    VkVertexInputAttributeDescription attr{};
    attr.location = 0; attr.binding = 0;
    attr.format   = VK_FORMAT_R32G32B32_SFLOAT;
    attr.offset   = 0;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions    = &attr;

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

    // Depth test enabled so buildings / other 3D geometry between camera
    // and the ring can occlude it; depth write disabled so the decal
    // doesn't block things drawn afterward. Z-bias on the vertices (kZBias)
    // pushes the ring off the terrain surface to avoid z-fighting.
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

static bool create_buffers(SelectionCircles::Impl& s) {
    // Static index buffer — indices are local to a ring (0..kVertsPerRing-1);
    // `vkCmdDrawIndexed(..., vertex_offset)` adds the offset per-ring.
    std::vector<u16> idx;
    idx.reserve(kIdxPerRing);
    for (u32 i = 0; i < kSegments; ++i) {
        u16 inner_i = static_cast<u16>(i * 2);
        u16 outer_i = static_cast<u16>(i * 2 + 1);
        u16 inner_n = static_cast<u16>(((i + 1) % kSegments) * 2);
        u16 outer_n = static_cast<u16>(((i + 1) % kSegments) * 2 + 1);
        idx.push_back(inner_i); idx.push_back(outer_i); idx.push_back(outer_n);
        idx.push_back(inner_i); idx.push_back(outer_n); idx.push_back(inner_n);
    }
    {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = idx.size() * sizeof(u16);
        bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(s.rhi->allocator(), &bci, &aci, &s.ib, &s.ib_alloc, &info) != VK_SUCCESS) return false;
        std::memcpy(info.pMappedData, idx.data(), idx.size() * sizeof(u16));
    }

    // Per-frame-in-flight dynamic VBs sized for up to kMaxRings selected units.
    VkDeviceSize vb_bytes = static_cast<VkDeviceSize>(kMaxRings) * kVertsPerRing * sizeof(Vertex);
    for (auto& r : s.ring_vbs) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = vb_bytes;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(s.rhi->allocator(), &bci, &aci, &r.vb, &r.alloc, &info) != VK_SUCCESS) return false;
        r.mapped = info.pMappedData;
    }
    return true;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

SelectionCircles::SelectionCircles() = default;
SelectionCircles::~SelectionCircles() { shutdown(); }

bool SelectionCircles::init(rhi::VulkanRhi& rhi) {
    m_impl = new Impl{};
    m_impl->rhi = &rhi;
    if (!create_pipeline(*m_impl)) { log::error(TAG, "pipeline create failed"); return false; }
    if (!create_buffers(*m_impl))  { log::error(TAG, "buffer create failed");   return false; }
    log::info(TAG, "selection circles initialized");
    return true;
}

void SelectionCircles::shutdown() {
    if (!m_impl) return;
    VkDevice device = m_impl->rhi ? m_impl->rhi->device() : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        for (auto& r : m_impl->ring_vbs) {
            if (r.vb) vmaDestroyBuffer(m_impl->rhi->allocator(), r.vb, r.alloc);
        }
        if (m_impl->ib) vmaDestroyBuffer(m_impl->rhi->allocator(), m_impl->ib, m_impl->ib_alloc);
        if (m_impl->pipeline)        vkDestroyPipeline(device, m_impl->pipeline, nullptr);
        if (m_impl->pipeline_layout) vkDestroyPipelineLayout(device, m_impl->pipeline_layout, nullptr);
    }
    delete m_impl;
    m_impl = nullptr;
}

// ── Per-frame draw ────────────────────────────────────────────────────────

void SelectionCircles::draw(VkCommandBuffer cmd,
                            const Camera& camera,
                            const simulation::World& world,
                            const map::TerrainData* terrain,
                            const std::vector<simulation::Unit>& selected,
                            simulation::Player local_player,
                            f32 alpha) {
    if (!m_impl || !m_impl->pipeline || !terrain || selected.empty()) return;

    // Pick ring slot by current frame index — same rationale as the HUD
    // batcher: RHI waits on the frame's fence before returning this
    // command buffer, so it's safe to rewrite this VB now.
    u32 slot = m_impl->rhi->frame_index();
    auto& ring = m_impl->ring_vbs[slot];
    if (!ring.mapped) return;

    // Collect eligible units + emit their vertices into the VB slot.
    struct Emitted { glm::vec4 color; };
    std::vector<Emitted> emitted;
    emitted.reserve(selected.size());
    auto* vb_verts = static_cast<Vertex*>(ring.mapped);

    constexpr glm::vec4 kColorLocal { 0.24f * 0.8f, 1.00f * 0.8f, 0.36f * 0.8f, 0.8f };
    constexpr glm::vec4 kColorOther { 1.00f * 0.8f, 0.28f * 0.8f, 0.24f * 0.8f, 0.8f };

    for (auto unit : selected) {
        if (emitted.size() >= kMaxRings) break;
        const auto* tf  = world.transforms.get(unit.id);
        const auto* sel = world.selectables.get(unit.id);
        if (!tf || !sel) continue;

        glm::vec3 ip = tf->interp_position(alpha);
        f32 cx = ip.x, cy = ip.y;

        f32 base_r = (sel->selection_radius > 0.0f) ? sel->selection_radius : 48.0f;
        // Constant world-unit stroke width regardless of unit size. The
        // ring sits just outside the selection radius and extends inward
        // by kStrokeWidth, so the outer edge grows with the unit but the
        // stroke thickness stays visually consistent.
        f32 r_out  = base_r;
        f32 r_in   = base_r - kStrokeWidth;
        if (r_in < 0.0f) r_in = 0.0f;

        // kSegments pairs (inner, outer) around the ring, each with its
        // own terrain-sampled z. Slight upward z-bias to prevent z-fighting.
        u32 base_vtx = static_cast<u32>(emitted.size()) * kVertsPerRing;
        for (u32 i = 0; i < kSegments; ++i) {
            f32 a  = (static_cast<f32>(i) / kSegments) * 6.28318530718f;
            f32 ca = std::cos(a), sa = std::sin(a);

            f32 ix = cx + r_in * ca, iy = cy + r_in * sa;
            f32 ox = cx + r_out * ca, oy = cy + r_out * sa;
            f32 iz = map::sample_height(*terrain, ix, iy) + kZBias;
            f32 oz = map::sample_height(*terrain, ox, oy) + kZBias;

            vb_verts[base_vtx + i * 2 + 0] = { ix, iy, iz };
            vb_verts[base_vtx + i * 2 + 1] = { ox, oy, oz };
        }

        const auto* owner = world.owners.get(unit.id);
        bool is_local = owner && owner->player.id == local_player.id;
        emitted.push_back({ is_local ? kColorLocal : kColorOther });
    }

    if (emitted.empty()) return;

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

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &ring.vb, &offset);
    vkCmdBindIndexBuffer(cmd, m_impl->ib, 0, VK_INDEX_TYPE_UINT16);

    glm::mat4 view_proj = camera.view_projection();

    for (size_t i = 0; i < emitted.size(); ++i) {
        PushConstants pc{};
        pc.mvp   = view_proj;
        pc.color = emitted[i].color;
        vkCmdPushConstants(cmd, m_impl->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(PushConstants), &pc);
        vkCmdDrawIndexed(cmd, kIdxPerRing, 1,
                         0,                                            // first_index
                         static_cast<i32>(i * kVertsPerRing),           // vertex_offset
                         0);
    }
}

} // namespace uldum::render
