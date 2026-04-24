#include "hud/hud.h"
#include "hud/node.h"
#include "hud/font.h"
#include "hud/world.h"
#include "hud/text_tag.h"
#include "hud/hud_loader.h"
#include "hud/action_bar.h"
#include "hud/minimap.h"
#include "hud/layout.h"

#include <nlohmann/json.hpp>

#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "asset/texture.h"
#include "simulation/world.h"
#include "simulation/components.h"
#include "simulation/ability_def.h"
#include "simulation/simulation.h"
#include "simulation/fog_of_war.h"
#include "map/terrain_data.h"
#include "input/selection.h"
#include "input/input_bindings.h"
#include "platform/platform.h"
#include "render/camera.h"
#include "network/protocol.h"
#include "core/log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace uldum::hud {

static constexpr const char* TAG = "HUD";

// Emit a pre-built packet via the sync callback, tagged with the owner
// of the node it applies to. Host-side only; clients never set sync_fn.
// Defined near the top so all Hud methods below can reference it.
static void emit_sync(Hud::Impl& s, const std::vector<u8>& pkt, u32 owner);

// Resolve the ability a given slot should display / fire. Defined later
// in the file; forward-declared here so action-bar keyboard dispatch
// (handle_action_bar_keys) can call it before the definition.
static const simulation::AbilityInstance*
resolve_slot_ability(u32 slot_index,
                     const ActionBarConfig& cfg,
                     const ActionBarRuntime& rt,
                     const WorldContext& ctx,
                     const simulation::AbilityDef*& out_def);

// Same-file forward decl for the affordability + cooldown gate, used by
// both click and keyboard dispatch. Defined alongside the render path.
static bool slot_castable_now(const WorldContext& ctx, u32 unit_id,
                              const simulation::AbilityInstance& inst,
                              const simulation::AbilityDef& def);

// Each batch = one draw call. Stage A uses a single implicit batch bound
// to the 1×1 white texture. Later stages add batch splits when the bound
// texture (icon, MSDF atlas) changes.
constexpr u32 MAX_QUADS = 4096;  // generous — each HUD frame has dozens, maybe hundreds
constexpr u32 MAX_VERTS = MAX_QUADS * 4;
constexpr u32 MAX_INDS  = MAX_QUADS * 6;

struct Vertex {
    f32 pos[2];
    u32 color;   // RGBA8, matches VK_FORMAT_R8G8B8A8_UNORM
    f32 uv[2];
};
static_assert(sizeof(Vertex) == 20, "HUD Vertex layout must stay tightly packed");

struct RingBuffer {
    VkBuffer      vb = VK_NULL_HANDLE;
    VmaAllocation vb_alloc = VK_NULL_HANDLE;
    void*         vb_mapped = nullptr;
    VkBuffer      ib = VK_NULL_HANDLE;
    VmaAllocation ib_alloc = VK_NULL_HANDLE;
    void*         ib_mapped = nullptr;
};

// Cached GPU resources for one loaded image asset. One per unique path;
// shared across every Image atom that references the same source.
struct HudImage {
    VkImage         image = VK_NULL_HANDLE;
    VmaAllocation   alloc = VK_NULL_HANDLE;
    VkImageView     view  = VK_NULL_HANDLE;
    VkDescriptorSet set   = VK_NULL_HANDLE;
    u32 w = 0;
    u32 h = 0;
};

// Which pipeline a batch uses. Added to as text / icons / other material
// variants land; for Stage C there's solid and text.
enum PipelineKind : u32 { PIPE_SOLID = 0, PIPE_TEXT = 1 };

struct Batch {
    PipelineKind    pipeline    = PIPE_SOLID;
    VkDescriptorSet desc_set    = VK_NULL_HANDLE;
    u32             index_start = 0;
    u32             index_count = 0;
};

struct Hud::Impl {
    rhi::VulkanRhi* rhi = nullptr;

    // Persistent pipeline + descriptor state. Both pipelines share the
    // descriptor layout (one combined image sampler) and pipeline layout
    // (one push constant for the mvp), differing only in frag shader.
    VkDescriptorSetLayout desc_layout   = VK_NULL_HANDLE;
    VkDescriptorPool      desc_pool     = VK_NULL_HANDLE;
    VkSampler             sampler       = VK_NULL_HANDLE;
    VkPipelineLayout      pipe_layout   = VK_NULL_HANDLE;
    VkPipeline            pipe_solid    = VK_NULL_HANDLE;
    VkPipeline            pipe_text     = VK_NULL_HANDLE;

    // 1×1 white texture — source for solid-color quads (vertex color × texel).
    VkImage         white_image = VK_NULL_HANDLE;
    VmaAllocation   white_alloc = VK_NULL_HANDLE;
    VkImageView     white_view  = VK_NULL_HANDLE;
    VkDescriptorSet white_set   = VK_NULL_HANDLE;

    // Per-frame-in-flight ring of mapped buffers. begin_frame() picks one
    // by current frame_index() — safe because the RHI waits on that frame's
    // in-flight fence before returning the command buffer.
    std::array<RingBuffer, rhi::MAX_FRAMES_IN_FLIGHT> rings{};

    // CPU-side draw list, rebuilt each frame. Batches split whenever the
    // pipeline or bound descriptor set changes (solid → text → solid etc.).
    std::vector<Vertex> verts;
    std::vector<u16>    inds;
    std::vector<Batch>  batches;
    u32 screen_w = 0;
    u32 screen_h = 0;
    bool frame_open = false;

    // Retained widget tree. Root Panel is always present; its rect is
    // updated to the current viewport each frame so percentage-style
    // positioning (future) can anchor to the full window.
    std::unique_ptr<Panel> root;

    // Default UI font — loaded from engine.uldpak at init. Inert if the
    // .ttf file is missing; draw_text then silently no-ops.
    std::unique_ptr<Font> font;

    // Image cache. Keyed by asset path; first access loads + uploads,
    // every later access returns the cached descriptor. Images live for
    // the Hud's lifetime (no eviction in v1). A null-entry value marks a
    // failed load so we don't retry every frame.
    std::unordered_map<std::string, std::unique_ptr<HudImage>> images;

    // World UI — config from hud.json, context from App at session start.
    WorldOverlayConfig  world_cfg{};
    const WorldContext* world_ctx = nullptr;

    // Node templates — populated from hud.json's `nodes` block at load.
    // Keyed by the top-level template id. Lua's CreateNode(id) copies the
    // matching JSON spec into a fresh node subtree under the root.
    std::unordered_map<std::string, nlohmann::json> node_templates;

    // Action-bar composite — slot layout + style from hud.json, slot
    // contents driven by the local selection each frame. Inert when
    // `config.enabled` is false (no composite declared in the map).
    ActionBarConfig  action_bar_cfg{};
    ActionBarRuntime action_bar_rt{};
    // Which slot the pointer is currently over / pressed on. -1 = none.
    // Needed across frames so a drag-out then release doesn't fire a
    // click, and so the hovered/pressed state updates lazily as the
    // cursor moves across slot boundaries.
    i32 action_bar_hover_slot   = -1;
    i32 action_bar_pressed_slot = -1;

    // Fired when a click on a slot resolves to an ability. App wires
    // this to the input preset's queue_ability.
    Hud::ActionBarCastFn action_bar_cast_fn;

    // Ability id the input preset is currently waiting for a target
    // on. Pushed by the app each frame; empty = no armed ability.
    // Drives the slot "held down" render treatment so the player can
    // see which ability they're about to commit.
    std::string action_bar_targeting_ability;

    // Minimap composite.
    MinimapConfig  minimap_cfg{};
    MinimapRuntime minimap_rt{};
    Hud::MinimapJumpFn minimap_jump_fn;

    // Network sync + input-event callbacks (host-side wiring).
    Hud::SyncFn         sync_fn;
    Hud::ButtonEventFn  button_event_fn;

    // Local player slot (UINT32_MAX = dedicated server, never used to render).
    u32 local_player = UINT32_MAX;

    // Text tag pool. Slot-based with per-slot generation counter for
    // handle validation. Destroyed tags leave alive=false and bump
    // generation; create_text_tag reuses the first dead slot it finds.
    struct TextTagEntry {
        bool               alive       = false;
        u32                generation  = 0;
        std::string        text;
        f32                px_size     = 14.0f;
        Color              color       = rgba(255, 255, 255);
        bool               visible     = true;
        glm::vec3          world_pos   {0.0f};
        u32                unit_id     = UINT32_MAX;   // attached unit id (UINT32_MAX = unattached)
        f32                z_offset    = 0.0f;
        f32                velocity_x  = 0.0f;         // screen px/sec
        f32                velocity_y  = 0.0f;
        f32                screen_dx   = 0.0f;         // accumulated screen offset from velocity
        f32                screen_dy   = 0.0f;
        f32                age         = 0.0f;
        f32                lifespan    = 0.0f;         // 0 → permanent
        f32                fadepoint   = 0.0f;         // seconds before end of lifespan to fade
        u32                owner_player = UINT32_MAX;  // broadcast by default
    };
    std::vector<TextTagEntry> text_tags;

    // Input state.
    f32   pointer_x = 0.0f;
    f32   pointer_y = 0.0f;
    bool  pointer_down_prev = false;
    Node* hover   = nullptr;   // widget currently under pointer
    Node* pressed = nullptr;   // widget pressed but not yet released
};

// ── Shader loading helper ─────────────────────────────────────────────────
static VkShaderModule load_shader(VkDevice device, std::string_view path) {
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) return VK_NULL_HANDLE;
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) {
        log::error(TAG, "HUD shader not found: '{}'", path);
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

// ── Setup ─────────────────────────────────────────────────────────────────

static bool create_descriptor_layout(Hud::Impl& s) {
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

static bool create_descriptor_pool(Hud::Impl& s) {
    // Stage A: one descriptor set (white). Stage B/C will grow this for
    // icon textures + MSDF atlases.
    VkDescriptorPoolSize sz{};
    sz.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sz.descriptorCount = 64;

    VkDescriptorPoolCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    ci.maxSets       = 64;
    ci.poolSizeCount = 1;
    ci.pPoolSizes    = &sz;
    return vkCreateDescriptorPool(s.rhi->device(), &ci, nullptr, &s.desc_pool) == VK_SUCCESS;
}

static bool create_sampler(Hud::Impl& s) {
    VkSamplerCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter    = VK_FILTER_LINEAR;
    ci.minFilter    = VK_FILTER_LINEAR;
    ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(s.rhi->device(), &ci, nullptr, &s.sampler) == VK_SUCCESS;
}

static bool create_pipeline_layout(Hud::Impl& s) {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo ci{};
    ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ci.setLayoutCount         = 1;
    ci.pSetLayouts            = &s.desc_layout;
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges    = &pc;
    return vkCreatePipelineLayout(s.rhi->device(), &ci, nullptr, &s.pipe_layout) == VK_SUCCESS;
}

static bool create_pipeline_variant(Hud::Impl& s, std::string_view frag_path, VkPipeline& out) {
    VkDevice device = s.rhi->device();
    VkShaderModule vert = load_shader(device, "engine/shaders/hud.vert.spv");
    VkShaderModule frag = load_shader(device, frag_path);
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
    binding.stride    = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0; attrs[0].binding = 0;
    attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset   = offsetof(Vertex, pos);
    attrs[1].location = 1; attrs[1].binding = 0;
    attrs[1].format   = VK_FORMAT_R8G8B8A8_UNORM;
    attrs[1].offset   = offsetof(Vertex, color);
    attrs[2].location = 2; attrs[2].binding = 0;
    attrs[2].format   = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset   = offsetof(Vertex, uv);

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
    ms.rasterizationSamples = s.rhi->msaa_samples();

    // HUD overlays the scene. We don't test or write depth — widgets sit on
    // top of everything and are ordered by draw submission.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    // Premultiplied-alpha blend. Vertex colors are authored non-premultiplied
    // and the shader multiplies by the (white or MSDF) texture sample. To
    // keep this shader single-path we premultiply on the CPU side when
    // building quad vertices.
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
    ci.layout              = s.pipe_layout;

    VkResult r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &out);
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    if (r != VK_SUCCESS) {
        log::error(TAG, "vkCreateGraphicsPipelines failed: {}", static_cast<int>(r));
        return false;
    }
    return true;
}

static bool create_ring_buffers(Hud::Impl& s) {
    for (auto& r : s.rings) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = MAX_VERTS * sizeof(Vertex);
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo info{};
        if (vmaCreateBuffer(s.rhi->allocator(), &bci, &aci, &r.vb, &r.vb_alloc, &info) != VK_SUCCESS) {
            log::error(TAG, "ring VB create failed");
            return false;
        }
        r.vb_mapped = info.pMappedData;

        bci.size  = MAX_INDS * sizeof(u16);
        bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (vmaCreateBuffer(s.rhi->allocator(), &bci, &aci, &r.ib, &r.ib_alloc, &info) != VK_SUCCESS) {
            log::error(TAG, "ring IB create failed");
            return false;
        }
        r.ib_mapped = info.pMappedData;
    }
    return true;
}

static bool create_white_texture(Hud::Impl& s) {
    const u8 pixel[4] = {255, 255, 255, 255};
    VkDeviceSize size = 4;

    // Staging.
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer      stage = VK_NULL_HANDLE;
    VmaAllocation stage_alloc = VK_NULL_HANDLE;
    VmaAllocationInfo stage_info{};
    if (vmaCreateBuffer(s.rhi->allocator(), &bci, &aci, &stage, &stage_alloc, &stage_info) != VK_SUCCESS) {
        return false;
    }
    std::memcpy(stage_info.pMappedData, pixel, size);

    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent        = { 1, 1, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_aci{};
    img_aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(s.rhi->allocator(), &ici, &img_aci, &s.white_image, &s.white_alloc, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(s.rhi->allocator(), stage, stage_alloc);
        return false;
    }

    VkCommandBuffer cmd = s.rhi->begin_oneshot();
    {
        VkImageMemoryBarrier to_xfer{};
        to_xfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_xfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        to_xfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_xfer.srcAccessMask       = 0;
        to_xfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_xfer.image               = s.white_image;
        to_xfer.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_xfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_xfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_xfer);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent      = { 1, 1, 1 };
        vkCmdCopyBufferToImage(cmd, stage, s.white_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        VkImageMemoryBarrier to_read{};
        to_read.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_read.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        to_read.image               = s.white_image;
        to_read.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_read);
    }
    s.rhi->end_oneshot(cmd);
    vmaDestroyBuffer(s.rhi->allocator(), stage, stage_alloc);

    VkImageViewCreateInfo vci{};
    vci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image            = s.white_image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(s.rhi->device(), &vci, nullptr, &s.white_view) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = s.desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &s.desc_layout;
    if (vkAllocateDescriptorSets(s.rhi->device(), &dsai, &s.white_set) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorImageInfo img{};
    img.sampler     = s.sampler;
    img.imageView   = s.white_view;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = s.white_set;
    w.dstBinding      = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &img;
    vkUpdateDescriptorSets(s.rhi->device(), 1, &w, 0, nullptr);
    return true;
}

// ── Image cache ───────────────────────────────────────────────────────────

// Create a GPU-sampled texture from an R8G8B8A8 pixel buffer. Mirrors
// create_white_texture but for user-sized images. Returns a populated
// HudImage on success; leaves fields VK_NULL_HANDLE on failure.
static bool create_hud_image(Hud::Impl& s, const u8* rgba, u32 w, u32 h, HudImage& out) {
    VkDevice device = s.rhi->device();
    VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * 4;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer      stage = VK_NULL_HANDLE;
    VmaAllocation stage_alloc = VK_NULL_HANDLE;
    VmaAllocationInfo stage_info{};
    if (vmaCreateBuffer(s.rhi->allocator(), &bci, &aci, &stage, &stage_alloc, &stage_info) != VK_SUCCESS) {
        return false;
    }
    std::memcpy(stage_info.pMappedData, rgba, size);

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
    if (vmaCreateImage(s.rhi->allocator(), &ici, &img_aci, &out.image, &out.alloc, nullptr) != VK_SUCCESS) {
        vmaDestroyBuffer(s.rhi->allocator(), stage, stage_alloc);
        return false;
    }

    VkCommandBuffer cmd = s.rhi->begin_oneshot();
    {
        VkImageMemoryBarrier to_xfer{};
        to_xfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_xfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        to_xfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_xfer.srcAccessMask       = 0;
        to_xfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_xfer.image               = out.image;
        to_xfer.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_xfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_xfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_xfer);

        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent      = { w, h, 1 };
        vkCmdCopyBufferToImage(cmd, stage, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        VkImageMemoryBarrier to_read{};
        to_read.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_read.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_read.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
        to_read.image               = out.image;
        to_read.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_read);
    }
    s.rhi->end_oneshot(cmd);
    vmaDestroyBuffer(s.rhi->allocator(), stage, stage_alloc);

    VkImageViewCreateInfo vci{};
    vci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image            = out.image;
    vci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    vci.format           = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vci, nullptr, &out.view) != VK_SUCCESS) {
        vmaDestroyImage(s.rhi->allocator(), out.image, out.alloc);
        out.image = VK_NULL_HANDLE;
        return false;
    }

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool     = s.desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts        = &s.desc_layout;
    if (vkAllocateDescriptorSets(device, &dsai, &out.set) != VK_SUCCESS) {
        vkDestroyImageView(device, out.view, nullptr);
        vmaDestroyImage(s.rhi->allocator(), out.image, out.alloc);
        out.image = VK_NULL_HANDLE;
        out.view  = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorImageInfo img{};
    img.sampler     = s.sampler;
    img.imageView   = out.view;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{};
    wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet          = out.set;
    wr.dstBinding      = 0;
    wr.descriptorCount = 1;
    wr.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo      = &img;
    vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);

    out.w = w;
    out.h = h;
    return true;
}

// Look up (or load + cache) a HudImage for `path`. On failure, caches a
// null entry so subsequent calls don't re-try. Returns nullptr if the
// asset is missing / fails to decode.
static HudImage* get_or_load_image(Hud::Impl& s, std::string_view path) {
    std::string key{path};
    auto it = s.images.find(key);
    if (it != s.images.end()) return it->second.get();  // may be null on prior failure

    auto* mgr = asset::AssetManager::instance();
    if (!mgr) { s.images.emplace(std::move(key), nullptr); return nullptr; }
    auto bytes = mgr->read_file_bytes(path);
    if (bytes.empty()) {
        log::warn(TAG, "image not found: '{}'", path);
        s.images.emplace(std::move(key), nullptr);
        return nullptr;
    }
    auto decoded = asset::load_texture_from_memory(bytes.data(), static_cast<u32>(bytes.size()));
    if (!decoded) {
        log::warn(TAG, "image decode failed '{}': {}", path, decoded.error());
        s.images.emplace(std::move(key), nullptr);
        return nullptr;
    }
    auto img = std::make_unique<HudImage>();
    if (!create_hud_image(s, decoded->pixels.data(), decoded->width, decoded->height, *img)) {
        log::warn(TAG, "image upload failed: '{}'", path);
        s.images.emplace(std::move(key), nullptr);
        return nullptr;
    }
    HudImage* raw = img.get();
    s.images.emplace(std::move(key), std::move(img));
    return raw;
}

// ── Hud facade ────────────────────────────────────────────────────────────

Hud::Hud() = default;
Hud::~Hud() { shutdown(); }

bool Hud::init(rhi::VulkanRhi& rhi) {
    m_impl = new Impl{};
    m_impl->rhi = &rhi;
    m_impl->verts.reserve(MAX_VERTS);
    m_impl->inds.reserve(MAX_INDS);
    m_impl->root = std::make_unique<Panel>();
    m_impl->root->bg = rgba(0, 0, 0, 0);  // invisible root — children draw their own backgrounds
    m_impl->root->hit_testable = false;   // let clicks on blank HUD areas fall through to world / gameplay

    if (!create_descriptor_layout(*m_impl)) { log::error(TAG, "desc layout create failed");   return false; }
    if (!create_descriptor_pool(*m_impl))   { log::error(TAG, "desc pool create failed");     return false; }
    if (!create_sampler(*m_impl))           { log::error(TAG, "sampler create failed");       return false; }
    if (!create_pipeline_layout(*m_impl))   { log::error(TAG, "pipeline layout create failed"); return false; }
    if (!create_pipeline_variant(*m_impl, "engine/shaders/hud.frag.spv",      m_impl->pipe_solid)) {
        log::error(TAG, "solid pipeline create failed"); return false;
    }
    if (!create_pipeline_variant(*m_impl, "engine/shaders/hud_text.frag.spv", m_impl->pipe_text)) {
        log::error(TAG, "text pipeline create failed"); return false;
    }
    if (!create_ring_buffers(*m_impl))      { log::error(TAG, "ring buffer create failed");   return false; }
    if (!create_white_texture(*m_impl))     { log::error(TAG, "white texture create failed"); return false; }

    // Load default font. Text rendering silently no-ops if this fails —
    // the HUD still draws solid widgets. We don't fail init() because a
    // missing font shouldn't bring down the whole engine.
    m_impl->font = std::make_unique<Font>();
    m_impl->font->init(rhi, "engine/fonts/Roboto-Regular.ttf",
                       m_impl->desc_layout, m_impl->sampler);

    log::info(TAG, "HUD initialized");
    return true;
}

void Hud::shutdown() {
    if (!m_impl) return;
    auto& s = *m_impl;
    s.root.reset();
    s.hover   = nullptr;
    s.pressed = nullptr;
    // Font shutdown happens first — it uses the RHI's one-shot cmd buffer
    // and must run before vkDeviceWaitIdle / pool destruction.
    if (s.font) { s.font->shutdown(); s.font.reset(); }
    VkDevice device = s.rhi ? s.rhi->device() : VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
        for (auto& r : s.rings) {
            if (r.vb) vmaDestroyBuffer(s.rhi->allocator(), r.vb, r.vb_alloc);
            if (r.ib) vmaDestroyBuffer(s.rhi->allocator(), r.ib, r.ib_alloc);
        }
        for (auto& [path, img] : s.images) {
            if (!img) continue;
            if (img->set)   vkFreeDescriptorSets(device, s.desc_pool, 1, &img->set);
            if (img->view)  vkDestroyImageView(device, img->view, nullptr);
            if (img->image) vmaDestroyImage(s.rhi->allocator(), img->image, img->alloc);
        }
        s.images.clear();
        if (s.white_set)   vkFreeDescriptorSets(device, s.desc_pool, 1, &s.white_set);
        if (s.white_view)  vkDestroyImageView(device, s.white_view, nullptr);
        if (s.white_image) vmaDestroyImage(s.rhi->allocator(), s.white_image, s.white_alloc);
        if (s.pipe_text)   vkDestroyPipeline(device, s.pipe_text, nullptr);
        if (s.pipe_solid)  vkDestroyPipeline(device, s.pipe_solid, nullptr);
        if (s.pipe_layout)   vkDestroyPipelineLayout(device, s.pipe_layout, nullptr);
        if (s.sampler)       vkDestroySampler(device, s.sampler, nullptr);
        if (s.desc_pool)     vkDestroyDescriptorPool(device, s.desc_pool, nullptr);
        if (s.desc_layout)   vkDestroyDescriptorSetLayout(device, s.desc_layout, nullptr);
    }
    delete m_impl;
    m_impl = nullptr;
}

void Hud::on_viewport_resized(u32 screen_w, u32 screen_h) {
    if (!m_impl) return;
    Rect viewport{ 0.0f, 0.0f,
                   static_cast<f32>(screen_w),
                   static_cast<f32>(screen_h) };

    // Action bar — bar rect anchors against viewport; each slot then
    // anchors against the new bar rect, so slot ordering matters.
    auto& ab = m_impl->action_bar_cfg;
    if (ab.enabled) {
        ab.rect = resolve(viewport, ab.placement);
        for (auto& slot : ab.slots) {
            slot.rect = resolve(ab.rect, slot.placement);
        }
    }

    // Minimap — single rect against viewport.
    auto& mm = m_impl->minimap_cfg;
    if (mm.enabled) {
        mm.rect = resolve(viewport, mm.placement);
    }

    // Node-tree (hud.json `nodes`) rects are resolved at CreateNode time
    // against the then-current viewport — they remain pinned to those
    // coordinates. Lua can DestroyNode + CreateNode to re-anchor if a
    // map cares; most tree content is positional rather than viewport-
    // anchored so this rarely matters in practice.
}

void Hud::begin_frame(u32 screen_w, u32 screen_h) {
    if (!m_impl) return;
    m_impl->verts.clear();
    m_impl->inds.clear();
    m_impl->batches.clear();
    m_impl->screen_w = screen_w;
    m_impl->screen_h = screen_h;
    m_impl->frame_open = true;
    // Root fills the viewport so children anchored to corners resolve
    // against the full window.
    if (m_impl->root) {
        m_impl->root->rect = { 0.0f, 0.0f,
                               static_cast<f32>(screen_w),
                               static_cast<f32>(screen_h) };
    }
}

// Start or continue a batch with the given pipeline + descriptor set. The
// previous batch (if any) has its index_count sealed so the draw loop
// knows its extent. Called before recording quads into verts / inds.
static void ensure_batch(Hud::Impl& s, PipelineKind pipe, VkDescriptorSet set) {
    if (!s.batches.empty()) {
        Batch& last = s.batches.back();
        if (last.pipeline == pipe && last.desc_set == set) return;
        last.index_count = static_cast<u32>(s.inds.size()) - last.index_start;
    }
    Batch nb{};
    nb.pipeline    = pipe;
    nb.desc_set    = set;
    nb.index_start = static_cast<u32>(s.inds.size());
    nb.index_count = 0;
    s.batches.push_back(nb);
}

// Build a u32 RGBA from a Color, premultiplying alpha on the CPU so the
// single-path shaders don't branch on blend modes.
static u32 premul_rgba(Color c) {
    u8 r = (c.rgba >>  0) & 0xFF;
    u8 g = (c.rgba >>  8) & 0xFF;
    u8 b = (c.rgba >> 16) & 0xFF;
    u8 a = (c.rgba >> 24) & 0xFF;
    r = static_cast<u8>((u32(r) * a) / 255);
    g = static_cast<u8>((u32(g) * a) / 255);
    b = static_cast<u8>((u32(b) * a) / 255);
    return u32(r) | (u32(g) << 8) | (u32(b) << 16) | (u32(a) << 24);
}

// Append a single triangle into the current batch. Used for pie sectors
// (cooldown radial) that don't fit the quad primitive. All three verts
// sample the same texel — callers pass a UV that lands inside the
// texture's opaque region (e.g. (0,0) on the 1×1 white texture).
static void append_triangle(Hud::Impl& s,
                            f32 x0, f32 y0,
                            f32 x1, f32 y1,
                            f32 x2, f32 y2,
                            f32 u,  f32 v,
                            u32 premul) {
    if (s.verts.size() + 3 > MAX_VERTS) return;
    u16 base = static_cast<u16>(s.verts.size());
    s.verts.push_back({ { x0, y0 }, premul, { u, v } });
    s.verts.push_back({ { x1, y1 }, premul, { u, v } });
    s.verts.push_back({ { x2, y2 }, premul, { u, v } });
    s.inds.push_back(base + 0);
    s.inds.push_back(base + 1);
    s.inds.push_back(base + 2);
}

// Append a textured quad into the current batch. Caller has already
// recorded an ensure_batch() with the desired pipeline + descriptor.
static void append_quad(Hud::Impl& s, const Rect& r,
                        f32 u0, f32 v0, f32 u1, f32 v1, u32 premul) {
    if (s.verts.size() + 4 > MAX_VERTS) return;
    u16 base = static_cast<u16>(s.verts.size());
    s.verts.push_back({ { r.x,       r.y       }, premul, { u0, v0 } });
    s.verts.push_back({ { r.x + r.w, r.y       }, premul, { u1, v0 } });
    s.verts.push_back({ { r.x + r.w, r.y + r.h }, premul, { u1, v1 } });
    s.verts.push_back({ { r.x,       r.y + r.h }, premul, { u0, v1 } });
    s.inds.push_back(base + 0);
    s.inds.push_back(base + 1);
    s.inds.push_back(base + 2);
    s.inds.push_back(base + 0);
    s.inds.push_back(base + 2);
    s.inds.push_back(base + 3);
}

Panel& Hud::root() {
    // init() always creates the root; this is safe after init.
    return *m_impl->root;
}

void Hud::clear_nodes() {
    if (!m_impl) return;
    if (m_impl->root) m_impl->root->clear_children();
    m_impl->hover   = nullptr;
    m_impl->pressed = nullptr;
}

void Hud::set_local_player(u32 player_id) {
    if (m_impl) m_impl->local_player = player_id;
}
u32 Hud::local_player() const {
    return m_impl ? m_impl->local_player : UINT32_MAX;
}

// Recursive search for a node with a matching id. Depth-first. Returns
// the first hit — ids are expected unique per HUD but not enforced.
static Node* find_node_recursive(Node* node, std::string_view id) {
    if (!node) return nullptr;
    if (node->id == id) return node;
    for (const auto& child : node->children()) {
        if (Node* hit = find_node_recursive(child.get(), id)) return hit;
    }
    return nullptr;
}

Node* Hud::find_node_by_id(std::string_view id) {
    if (!m_impl || !m_impl->root || id.empty()) return nullptr;
    return find_node_recursive(m_impl->root.get(), id);
}

// Walk the tree looking for a child whose id matches; on hit, swap-and-pop
// it out of its parent's children vector (unique_ptr cleans up the subtree
// automatically).
static bool remove_node_recursive(Node* parent, std::string_view id) {
    if (!parent) return false;
    // Can't use the public `children()` getter (returns const). Go through
    // add_child / clear_children? They don't offer mid-vector erase either.
    // We accept friendship-style access via a small helper method below.
    return parent->erase_child_by_id(id);
}

bool Hud::remove_node_by_id(std::string_view id) {
    if (!m_impl || !m_impl->root || id.empty()) return false;
    // Capture owner before the node is freed so we can route the sync to
    // the right peer; otherwise post-remove we'd have no way to tell.
    u32 owner = UINT32_MAX;
    if (auto* n = find_node_by_id(id)) owner = n->owner_player;
    // Clear transient hover / pressed references before we drop the node,
    // else we'd chase a freed pointer next input frame.
    if (m_impl->hover && m_impl->hover->id == id)   m_impl->hover = nullptr;
    if (m_impl->pressed && m_impl->pressed->id == id) m_impl->pressed = nullptr;
    bool ok = remove_node_recursive(m_impl->root.get(), id);
    if (ok) emit_sync(*m_impl, uldum::network::build_hud_destroy_node(id), owner);
    return ok;
}

// ── Template registry ────────────────────────────────────────────────────

void Hud::add_node_template(std::string id, const nlohmann::json& spec) {
    if (!m_impl || id.empty()) return;
    m_impl->node_templates[std::move(id)] = spec;
}

const nlohmann::json* Hud::get_node_template(std::string_view id) const {
    if (!m_impl) return nullptr;
    auto it = m_impl->node_templates.find(std::string{id});
    return it != m_impl->node_templates.end() ? &it->second : nullptr;
}

void Hud::clear_node_templates() {
    if (!m_impl) return;
    m_impl->node_templates.clear();
}

bool Hud::instantiate_template(std::string_view id, const Placement& placement) {
    if (!m_impl || !m_impl->rhi) return false;
    VkExtent2D ex = m_impl->rhi->extent();
    TemplatePlacement tp{};
    tp.anchor       = placement.anchor;
    tp.x            = placement.x;
    tp.y            = placement.y;
    tp.w            = placement.w;
    tp.h            = placement.h;
    tp.owner_player = placement.owner_player;
    bool ok = uldum::hud::instantiate_template(*this, id, ex.width, ex.height, tp);
    if (ok) {
        emit_sync(*m_impl,
                  uldum::network::build_hud_create_node(id, placement.anchor,
                                                         placement.x, placement.y,
                                                         placement.w, placement.h),
                  placement.owner_player);
    }
    return ok;
}

// Implemented in world.cpp — exposed here so hud.cpp stays free of sim /
// render / map includes. Called from draw_world_overlays() below.
void draw_entity_bars_impl(Hud& hud, u32 screen_w, u32 screen_h,
                           const WorldOverlayConfig& cfg,
                           const WorldContext& ctx,
                           f32 alpha);
void draw_unit_name_label_impl(Hud& hud, u32 screen_w, u32 screen_h,
                               const WorldOverlayConfig& cfg,
                               const WorldContext& ctx,
                               f32 alpha);

// Project a world-space point to screen pixels using a view-projection
// matrix. Returns false for points behind the camera; keeps points
// comfortably past the visible rect so newly-offscreen tags fade cleanly.
static bool project_world_for_tag(const glm::mat4& vp, const glm::vec3& world,
                                  u32 screen_w, u32 screen_h,
                                  f32& sx, f32& sy) {
    glm::vec4 clip = vp * glm::vec4(world, 1.0f);
    if (clip.w <= 0.001f) return false;
    f32 ndc_x = clip.x / clip.w;
    f32 ndc_y = clip.y / clip.w;
    sx = (ndc_x * 0.5f + 0.5f) * static_cast<f32>(screen_w);
    sy = (ndc_y * 0.5f + 0.5f) * static_cast<f32>(screen_h);
    return true;
}

static void draw_text_tags(Hud& hud, Hud::Impl& s, const WorldContext& ctx, f32 alpha) {
    if (!ctx.camera || !ctx.world) return;
    const glm::mat4 vp = ctx.camera->view_projection();
    const auto& world  = *ctx.world;
    const u32 sw = s.screen_w, sh = s.screen_h;
    for (auto& t : s.text_tags) {
        if (!t.alive || !t.visible || t.text.empty()) continue;
        if (t.owner_player != UINT32_MAX && t.owner_player != s.local_player) continue;

        // Anchor: attached unit's interpolated position (+ z_offset), or
        // the raw world_pos if unattached.
        glm::vec3 world_anchor{0.0f};
        if (t.unit_id != UINT32_MAX) {
            const auto* tf = world.transforms.get(t.unit_id);
            if (!tf) continue;
            world_anchor = tf->interp_position(alpha) + glm::vec3(0.0f, 0.0f, t.z_offset);
        } else {
            world_anchor = t.world_pos + glm::vec3(0.0f, 0.0f, t.z_offset);
        }

        f32 cx = 0.0f, cy = 0.0f;
        if (!project_world_for_tag(vp, world_anchor, sw, sh, cx, cy)) continue;

        // Apply accumulated screen-space velocity offset.
        cx += t.screen_dx;
        cy += t.screen_dy;

        // Fade alpha based on fadepoint / lifespan.
        f32 fade = 1.0f;
        if (t.lifespan > 0.0f && t.fadepoint > 0.0f) {
            f32 fade_start = t.lifespan - t.fadepoint;
            if (t.age > fade_start) {
                f32 f = 1.0f - (t.age - fade_start) / t.fadepoint;
                if (f < 0.0f) f = 0.0f;
                fade = f;
            }
        }

        // Combine color alpha with fade.
        u8 base_a = static_cast<u8>((t.color.rgba >> 24) & 0xFF);
        u8 out_a  = static_cast<u8>(base_a * fade);
        Color final_color{ (t.color.rgba & 0x00FFFFFFu) | (static_cast<u32>(out_a) << 24) };

        // Measure + draw centered horizontally; baseline positioned so text
        // is centered vertically around the projected point.
        f32 text_w    = hud.text_width_px(t.text, t.px_size);
        f32 line_h    = hud.text_line_height_px(t.px_size);
        f32 ascent    = hud.text_ascent_px(t.px_size);
        f32 x_left    = cx - text_w * 0.5f;
        f32 y_baseline = cy + ascent - line_h * 0.5f;
        hud.draw_text(x_left, y_baseline, t.text, final_color, t.px_size);
    }
}

void Hud::set_world_overlay_config(const WorldOverlayConfig& cfg) {
    if (!m_impl) return;
    m_impl->world_cfg = cfg;
}

void Hud::set_world_context(const WorldContext* ctx) {
    if (!m_impl) return;
    m_impl->world_ctx = ctx;
}

void Hud::draw_world_overlays(f32 alpha) {
    if (!m_impl || !m_impl->frame_open) return;
    if (!m_impl->world_ctx) return;
    draw_entity_bars_impl(*this,
                          m_impl->screen_w, m_impl->screen_h,
                          m_impl->world_cfg,
                          *m_impl->world_ctx,
                          alpha);
    draw_unit_name_label_impl(*this,
                              m_impl->screen_w, m_impl->screen_h,
                              m_impl->world_cfg,
                              *m_impl->world_ctx,
                              alpha);
    draw_text_tags(*this, *m_impl, *m_impl->world_ctx, alpha);
}

// ── Text tags ─────────────────────────────────────────────────────────────

void Hud::update_text_tags(f32 dt) {
    if (!m_impl) return;
    for (auto& t : m_impl->text_tags) {
        if (!t.alive) continue;
        if (t.lifespan > 0.0f) {
            t.age += dt;
            if (t.age >= t.lifespan) {
                // Expire — mirror destroy_text_tag's bookkeeping.
                t.alive = false;
                ++t.generation;
                t.text.clear();
                continue;
            }
        }
        t.screen_dx += t.velocity_x * dt;
        t.screen_dy += t.velocity_y * dt;
    }
}

TextTagId Hud::create_text_tag(const TextTagCreateInfo& info) {
    if (!m_impl) return {};
    auto& pool = m_impl->text_tags;
    // Find a dead slot, else grow.
    u32 idx = UINT32_MAX;
    for (u32 i = 0; i < pool.size(); ++i) {
        if (!pool[i].alive) { idx = i; break; }
    }
    if (idx == UINT32_MAX) {
        pool.emplace_back();
        idx = static_cast<u32>(pool.size() - 1);
    }
    auto& t = pool[idx];
    ++t.generation;  // generation 0 reserved for "invalid"; first real gen = 1
    t.alive      = true;
    t.text       = info.text;
    t.px_size    = info.px_size;
    t.color      = info.color;
    t.visible    = true;
    t.world_pos  = info.pos;
    t.unit_id    = info.unit.id;  // UINT32_MAX means not attached (Unit default)
    t.z_offset   = info.z_offset;
    t.velocity_x = info.velocity_x;
    t.velocity_y = info.velocity_y;
    t.screen_dx  = 0.0f;
    t.screen_dy  = 0.0f;
    t.age        = 0.0f;
    t.lifespan     = info.lifespan;
    t.fadepoint    = info.fadepoint;
    t.owner_player = info.owner_player;

    // MP sync: fire-and-forget creation. Clients run the animation
    // locally from identical params (same lifespan / velocity / fadepoint).
    // No mid-life setters are synced in v1 — setters apply locally only.
    emit_sync(*m_impl,
              uldum::network::build_hud_create_text_tag(
                  info.text, info.px_size,
                  info.pos.x, info.pos.y, info.pos.z,
                  info.unit.id, info.z_offset,
                  info.color.rgba,
                  info.velocity_x, info.velocity_y,
                  info.lifespan, info.fadepoint),
              info.owner_player);

    return TextTagId{ idx, t.generation };
}

void Hud::destroy_text_tag(TextTagId id) {
    if (!m_impl || !id.valid()) return;
    auto& pool = m_impl->text_tags;
    if (id.index >= pool.size()) return;
    auto& t = pool[id.index];
    if (!t.alive || t.generation != id.generation) return;
    t.alive = false;
    ++t.generation;
    t.text.clear();
}

bool Hud::text_tag_alive(TextTagId id) const {
    if (!m_impl || !id.valid()) return false;
    const auto& pool = m_impl->text_tags;
    if (id.index >= pool.size()) return false;
    const auto& t = pool[id.index];
    return t.alive && t.generation == id.generation;
}

// Helpers for the various setters — look up the entry if the handle is
// live, else return nullptr (silently).
static Hud::Impl::TextTagEntry* lookup_tag(Hud::Impl& s, TextTagId id) {
    if (!id.valid() || id.index >= s.text_tags.size()) return nullptr;
    auto& t = s.text_tags[id.index];
    if (!t.alive || t.generation != id.generation) return nullptr;
    return &t;
}

void Hud::set_text_tag_text(TextTagId id, std::string_view text) {
    if (!m_impl) return;
    if (auto* t = lookup_tag(*m_impl, id)) t->text.assign(text);
}
void Hud::set_text_tag_pos(TextTagId id, f32 x, f32 y, f32 z) {
    if (!m_impl) return;
    if (auto* t = lookup_tag(*m_impl, id)) {
        t->world_pos = { x, y, z };
        t->unit_id   = UINT32_MAX;
    }
}
void Hud::set_text_tag_pos_unit(TextTagId id, u32 unit_id, f32 z_offset) {
    if (!m_impl) return;
    if (auto* t = lookup_tag(*m_impl, id)) {
        t->unit_id  = unit_id;
        t->z_offset = z_offset;
    }
}
void Hud::set_text_tag_color(TextTagId id, Color color) {
    if (!m_impl) return;
    if (auto* t = lookup_tag(*m_impl, id)) t->color = color;
}
void Hud::set_text_tag_velocity(TextTagId id, f32 vx, f32 vy) {
    if (!m_impl) return;
    if (auto* t = lookup_tag(*m_impl, id)) { t->velocity_x = vx; t->velocity_y = vy; }
}
void Hud::set_text_tag_visible(TextTagId id, bool visible) {
    if (!m_impl) return;
    if (auto* t = lookup_tag(*m_impl, id)) t->visible = visible;
}

// ── Action-bar composite ──────────────────────────────────────────────────
// Phase A: static slot rendering (colored rects + hotkey label). Icons,
// cooldown radial, and click/hotkey input arrive in later phases.

void Hud::set_action_bar_config(const ActionBarConfig& cfg) {
    if (!m_impl) return;
    m_impl->action_bar_cfg = cfg;
    // Reset transient runtime state. Visibility defaults to "shown" so a
    // declared bar renders immediately once a unit is selected — nothing
    // else to prime.
    m_impl->action_bar_rt = ActionBarRuntime{};
}

void Hud::action_bar_set_visible(bool visible) {
    if (!m_impl) return;
    m_impl->action_bar_rt.visible = visible;
}

void Hud::action_bar_set_slot_visible(u32 slot, bool visible) {
    if (!m_impl) return;
    auto& slots = m_impl->action_bar_cfg.slots;
    if (slot >= slots.size()) return;
    slots[slot].visible = visible;
}

void Hud::action_bar_set_slot(u32 slot, std::string_view ability_id) {
    if (!m_impl) return;
    auto& slots = m_impl->action_bar_cfg.slots;
    if (slot >= slots.size()) return;
    slots[slot].bound_ability.assign(ability_id);
}

void Hud::action_bar_clear_slot(u32 slot) {
    if (!m_impl) return;
    auto& slots = m_impl->action_bar_cfg.slots;
    if (slot >= slots.size()) return;
    slots[slot].bound_ability.clear();
}

void Hud::set_action_bar_cast_fn(ActionBarCastFn fn) {
    if (m_impl) m_impl->action_bar_cast_fn = std::move(fn);
}

void Hud::action_bar_set_hotkey_mode(ActionBarHotkeyMode mode) {
    if (!m_impl) return;
    m_impl->action_bar_rt.hotkey_mode = mode;
    // Reset rising-edge tracking — the key may be held at the moment the
    // mode flips, and we don't want that to immediately fire a cast in
    // the new resolution.
    for (auto& slot : m_impl->action_bar_cfg.slots) slot.hotkey_prev_down = false;
}

void Hud::action_bar_set_targeting_ability(std::string_view ability_id) {
    if (!m_impl) return;
    m_impl->action_bar_targeting_ability.assign(ability_id);
}

// ── Minimap composite ────────────────────────────────────────────────────

void Hud::set_minimap_config(const MinimapConfig& cfg) {
    if (!m_impl) return;
    m_impl->minimap_cfg = cfg;
    m_impl->minimap_rt  = MinimapRuntime{};
}

void Hud::minimap_set_visible(bool visible) {
    if (m_impl) m_impl->minimap_rt.visible = visible;
}

void Hud::set_minimap_jump_fn(MinimapJumpFn fn) {
    if (m_impl) m_impl->minimap_jump_fn = std::move(fn);
}

// True if (x, y) is inside the minimap panel AND the panel is enabled +
// visible. Unlike the action bar, the minimap is a single rect so the
// hit-test returns bool rather than an index.
static bool minimap_hit_test(const Hud::Impl& s, f32 x, f32 y) {
    const auto& cfg = s.minimap_cfg;
    if (!cfg.enabled || !s.minimap_rt.visible) return false;
    const Rect& r = cfg.rect;
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Convert a point inside the minimap rect into a world-space (x, y)
// position on the ground plane. World coords are centered on (0, 0);
// terrain extents come from WorldContext::terrain.
static void minimap_point_to_world(const Rect& mm, const map::TerrainData& td,
                                    f32 sx, f32 sy, f32& wx, f32& wy) {
    f32 fx = (sx - mm.x) / mm.w;   // 0..1 across minimap
    f32 fy = (sy - mm.y) / mm.h;
    wx = td.origin_x() + fx * td.world_width();
    wy = td.origin_y() + fy * td.world_height();
}

void Hud::handle_action_bar_keys(const platform::InputState& input) {
    if (!m_impl) return;
    auto& s = *m_impl;
    const auto& cfg = s.action_bar_cfg;
    if (!cfg.enabled || !s.action_bar_rt.visible) return;
    if (!s.world_ctx || !s.action_bar_cast_fn) return;

    for (u32 i = 0; i < cfg.slots.size(); ++i) {
        auto& slot = s.action_bar_cfg.slots[i];
        if (!slot.visible || slot.hotkey.empty()) continue;

        bool down = input::InputBindings::resolve_key(slot.hotkey, input);
        bool rising = down && !slot.hotkey_prev_down;
        slot.hotkey_prev_down = down;
        if (!rising) continue;

        const simulation::AbilityDef* def = nullptr;
        const simulation::AbilityInstance* inst =
            resolve_slot_ability(i, cfg, s.action_bar_rt,
                                 *s.world_ctx, def);
        if (!inst || !def) continue;

        // Same gate as the click path — refuse to fire while on cooldown
        // or unaffordable so the sim doesn't have to reject the command.
        u32 unit_id = s.world_ctx->selection
                        ? s.world_ctx->selection->selected().front().id
                        : UINT32_MAX;
        if (unit_id == UINT32_MAX) continue;
        if (!slot_castable_now(*s.world_ctx, unit_id, *inst, *def)) continue;

        s.action_bar_cast_fn(inst->ability_id);
    }
}

// Return the slot index under the given pointer coords, or -1 if none.
// Only enabled, visible slots participate — invisible slots (Lua
// ActionBarSetSlotVisible(false)) and a disabled bar itself are skipped
// so clicks fall through to the world underneath.
static i32 action_bar_hit_test(const Hud::Impl& s, f32 x, f32 y) {
    const auto& cfg = s.action_bar_cfg;
    if (!cfg.enabled || !s.action_bar_rt.visible) return -1;
    for (u32 i = 0; i < cfg.slots.size(); ++i) {
        const auto& slot = cfg.slots[i];
        if (!slot.visible) continue;
        const Rect& r = slot.rect;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
            return static_cast<i32>(i);
        }
    }
    return -1;
}

// Pick the ability the given slot should display, based on the local
// player's selection, the bar's binding mode, and (in Auto mode) the
// user's hotkey-mode preference.
//
// Manual mode: the slot has an `bound_ability` id set by Lua. Return
// the instance of that ability on the selected unit if it owns it,
// else nullptr — the slot renders as "ability not available" (empty).
//
// Auto + Ability hotkey mode: first selected unit's non-hidden
// abilities are searched for one whose def `hotkey` matches the slot's
// hotkey letter.
//
// Auto + Positional hotkey mode: the slot's index selects the Nth
// non-hidden ability in registration order; slot.hotkey is purely the
// keybind.
//
// Returns nullptr when there's no selection, no ability fills that
// slot, or the registry lookup fails.
static const simulation::AbilityInstance*
resolve_slot_ability(u32 slot_index,
                     const ActionBarConfig& cfg,
                     const ActionBarRuntime& rt,
                     const WorldContext& ctx,
                     const simulation::AbilityDef*& out_def) {
    out_def = nullptr;
    if (slot_index >= cfg.slots.size()) return nullptr;
    if (!ctx.selection || !ctx.world || !ctx.abilities) return nullptr;
    const auto& sel = ctx.selection->selected();
    if (sel.empty()) return nullptr;
    const auto* aset = ctx.world->ability_sets.get(sel.front().id);
    if (!aset) return nullptr;

    const auto& slot = cfg.slots[slot_index];

    if (cfg.binding_mode == ActionBarBindingMode::Manual) {
        if (slot.bound_ability.empty()) return nullptr;
        for (const auto& inst : aset->abilities) {
            if (inst.ability_id == slot.bound_ability) {
                const auto* def = ctx.abilities->get(inst.ability_id);
                if (!def) return nullptr;
                out_def = def;
                return &inst;
            }
        }
        return nullptr;
    }

    if (rt.hotkey_mode == ActionBarHotkeyMode::Positional) {
        // Nth non-hidden ability in registration order.
        u32 nth = 0;
        for (const auto& inst : aset->abilities) {
            const auto* def = ctx.abilities->get(inst.ability_id);
            if (!def || def->hidden) continue;
            if (nth == slot_index) {
                out_def = def;
                return &inst;
            }
            ++nth;
        }
        return nullptr;
    }

    // Ability mode — match by ability def's hotkey letter.
    if (slot.hotkey.empty()) return nullptr;
    for (const auto& inst : aset->abilities) {
        const auto* def = ctx.abilities->get(inst.ability_id);
        if (!def || def->hidden) continue;
        if (def->hotkey == slot.hotkey) {
            out_def = def;
            return &inst;
        }
    }
    return nullptr;
}

// Map an angle (0 = 12 o'clock, grows clockwise) to a point on the
// rectangle's perimeter — the ray from rect center at that angle hits
// the nearest axis-aligned edge. Used to build a cooldown pie whose
// outer boundary matches the slot's square outline instead of a circle
// that would bulge past or fall short of the corners.
static void perimeter_point(f32 cx, f32 cy, f32 hw, f32 hh,
                             f32 theta, f32& out_x, f32& out_y) {
    f32 dx = std::sin(theta);
    f32 dy = -std::cos(theta);
    f32 tx = (std::abs(dx) > 1e-5f) ? hw / std::abs(dx) : 1e9f;
    f32 ty = (std::abs(dy) > 1e-5f) ? hh / std::abs(dy) : 1e9f;
    f32 t  = std::min(tx, ty);
    out_x = cx + dx * t;
    out_y = cy + dy * t;
}

// Dark overlay covering the still-on-cooldown fraction of a slot.
// Sweep grows clockwise from 12 o'clock as the ability ticks toward
// ready; at fraction=1 the whole slot is covered, at 0 nothing draws.
// Geometry is a triangle fan from the slot center through perimeter
// points, so the shape hugs the rectangle even at corners.
static void draw_cooldown_pie(Hud::Impl& s, const Rect& r, f32 fraction, Color overlay) {
    if (fraction <= 0.0f) return;
    if (fraction > 1.0f)  fraction = 1.0f;
    ensure_batch(s, PIPE_SOLID, s.white_set);
    u32 premul = premul_rgba(overlay);

    f32 cx = r.x + r.w * 0.5f;
    f32 cy = r.y + r.h * 0.5f;
    f32 hw = r.w * 0.5f;
    f32 hh = r.h * 0.5f;

    // 48 segments around a full circle = 7.5° per segment — corner
    // clipping is a sub-pixel chord under typical slot sizes.
    constexpr u32 kSegmentsFull = 48;
    u32 n = static_cast<u32>(std::ceil(kSegmentsFull * fraction));
    if (n < 1) n = 1;

    constexpr f32 TWO_PI = 6.2831853f;
    f32 start = (1.0f - fraction) * TWO_PI;
    f32 step  = (TWO_PI - start) / static_cast<f32>(n);

    f32 px0, py0;
    perimeter_point(cx, cy, hw, hh, start, px0, py0);
    for (u32 i = 0; i < n; ++i) {
        f32 a = start + step * static_cast<f32>(i + 1);
        f32 px1, py1;
        perimeter_point(cx, cy, hw, hh, a, px1, py1);
        append_triangle(s, cx, cy, px0, py0, px1, py1, 0.0f, 0.0f, premul);
        px0 = px1;
        py0 = py1;
    }
}

// True if `unit_id` has every cost of this ability level paid in full.
// Cost keys map to `Health` (for "health") or to `StateBlock::states`
// for map-defined resources (mana / energy / rage etc.). Missing state
// entries fail closed — an ability that costs a resource the unit
// doesn't have simply can't be cast.
static bool can_afford(const simulation::World& world, u32 unit_id,
                       const simulation::AbilityLevelDef& lvl) {
    if (lvl.cost.empty()) return true;
    for (const auto& [state_name, amount] : lvl.cost) {
        if (amount <= 0.0f) continue;
        if (state_name == "health") {
            const auto* hp = world.healths.get(unit_id);
            if (!hp || hp->current < amount) return false;
            continue;
        }
        const auto* sb = world.state_blocks.get(unit_id);
        if (!sb) return false;
        auto it = sb->states.find(state_name);
        if (it == sb->states.end() || it->second.current < amount) return false;
    }
    return true;
}

// True when this ability form can actually be triggered from the
// action bar (as opposed to passives / auras that just exist on the
// unit). Used to decide whether to apply the affordability gate — a
// passive icon shouldn't render dimmed just because the unit is out of
// mana it would never spend anyway.
static bool is_castable_form(simulation::AbilityForm f) {
    using F = simulation::AbilityForm;
    return f == F::Instant || f == F::Toggle
        || f == F::TargetUnit || f == F::TargetPoint
        || f == F::Channel;
}

// Can the selected unit trigger this slot's ability right now? Combines
// the cooldown-remaining check with the affordability check. A slot
// whose ability isn't a castable form always returns true here — we
// don't want to suppress passive/aura icons.
static bool slot_castable_now(const WorldContext& ctx, u32 unit_id,
                              const simulation::AbilityInstance& inst,
                              const simulation::AbilityDef& def) {
    if (!is_castable_form(def.form)) return true;
    if (inst.cooldown_remaining > 0.0f) return false;
    if (!ctx.world) return false;
    return can_afford(*ctx.world, unit_id, def.level_data(inst.level));
}

// Format "seconds remaining" for the cooldown text. Integer above 1s
// (e.g. "9", "3") — matches RTS convention where the small decimal is
// noise. Sub-second values show one decimal ("0.6") so the last tick
// isn't jarring.
static void format_cooldown_secs(f32 remaining, char* buf, size_t buf_size) {
    if (remaining >= 1.0f) {
        int secs = static_cast<int>(std::ceil(remaining));
        std::snprintf(buf, buf_size, "%d", secs);
    } else if (remaining > 0.0f) {
        std::snprintf(buf, buf_size, "%.1f", remaining);
    } else {
        buf[0] = '\0';
    }
}

// ── classic_rts render variant ───────────────────────────────────────────
// WC3-style: flat-colored square slots, square icon inset by the border,
// dark pie overlay sweeping clockwise from 12 o'clock during cooldown,
// remaining-seconds number centered over the overlay, and hotkey badge
// in the top-right. Parameters come from ActionBarStyle.
static void draw_action_bar_classic_rts(Hud& hud, Hud::Impl& s) {
    const auto& cfg = s.action_bar_cfg;
    const auto& rt  = s.action_bar_rt;

    // When any slot is armed (targeting mode), non-armed slots get a
    // dim wash so the armed slot reads as the single spotlighted
    // command. Compute once — the check is identical for every slot.
    bool any_armed = !s.action_bar_targeting_ability.empty();

    for (u32 i = 0; i < cfg.slots.size(); ++i) {
        const auto& slot = cfg.slots[i];
        if (!slot.visible) continue;

        const simulation::AbilityDef* def = nullptr;
        const simulation::AbilityInstance* inst = nullptr;
        if (s.world_ctx) inst = resolve_slot_ability(i, cfg, rt, *s.world_ctx, def);

        bool armed = any_armed && def && inst
                  && inst->ability_id == s.action_bar_targeting_ability;

        // Slot background. Mouse press wins; otherwise armed uses press_bg
        // (the held-down look), else hover, else idle.
        Color bg = slot.style.bg;
        if (slot.pressed)      bg = slot.style.press_bg;
        else if (armed)        bg = slot.style.press_bg;
        else if (slot.hovered) bg = slot.style.hover_bg;
        hud.draw_rect(slot.rect, bg);

        // Icon — inset by the slot's normal border width so the icon
        // and border don't fight for the same pixels.
        f32 bw = (slot.style.border_width > 0.0f) ? slot.style.border_width : 0.0f;
        Rect icon_rect{ slot.rect.x + bw, slot.rect.y + bw,
                        slot.rect.w - bw * 2.0f, slot.rect.h - bw * 2.0f };
        if (def && !def->icon.empty()) {
            hud.draw_image(icon_rect, def->icon);
        }

        if (armed) {
            // Armed slot: skip cooldown + affordability overlays. The
            // player has committed to this ability; extra state
            // overlays would muddy the spotlight.
        } else if (any_armed) {
            // Non-armed slot while targeting — wash it out so the
            // spotlighted slot stands alone.
            hud.draw_rect(icon_rect, cfg.style.disabled_tint);
        } else {
            // Normal state: cooldown radial + seconds text, or
            // affordability dim if unaffordable and castable.
            bool on_cooldown = def && inst && inst->cooldown_remaining > 0.05f
                            && def->level_data(inst->level).cooldown > 0.0f;
            if (on_cooldown) {
                f32 total = def->level_data(inst->level).cooldown;
                f32 frac  = inst->cooldown_remaining / total;
                draw_cooldown_pie(s, icon_rect, frac, cfg.style.cooldown_overlay);

                char buf[16];
                format_cooldown_secs(inst->cooldown_remaining, buf, sizeof(buf));
                if (buf[0] != '\0') {
                    f32 px = cfg.style.cooldown_text_size;
                    f32 tw = hud.text_width_px(buf, px);
                    f32 line_h = hud.text_line_height_px(px);
                    f32 ascent = hud.text_ascent_px(px);
                    f32 tx = icon_rect.x + (icon_rect.w - tw) * 0.5f;
                    f32 ty = icon_rect.y + (icon_rect.h - line_h) * 0.5f + ascent;
                    hud.draw_text(tx, ty, buf, cfg.style.cooldown_text_color, px);
                }
            } else if (def && inst && s.world_ctx && is_castable_form(def->form)) {
                u32 unit_id = s.world_ctx->selection
                                ? s.world_ctx->selection->selected().front().id
                                : UINT32_MAX;
                if (unit_id != UINT32_MAX && s.world_ctx->world) {
                    if (!can_afford(*s.world_ctx->world, unit_id,
                                    def->level_data(inst->level))) {
                        hud.draw_rect(icon_rect, cfg.style.disabled_tint);
                    }
                }
            }
        }

        // Border. Armed slot gets the accent color at its thicker width
        // (overriding the slot's normal border). Non-armed slots keep
        // their normal border whether or not another slot is armed —
        // the dim wash already communicates the inactive state.
        Color border_color = slot.style.border_color;
        f32   border_w     = bw;
        if (armed) {
            border_color = cfg.style.armed_border_color;
            border_w     = cfg.style.armed_border_width;
        }
        if (border_w > 0.0f && (border_color.rgba >> 24) != 0) {
            Rect r = slot.rect;
            hud.draw_rect({ r.x, r.y, r.w, border_w }, border_color);
            hud.draw_rect({ r.x, r.y + r.h - border_w, r.w, border_w }, border_color);
            hud.draw_rect({ r.x, r.y, border_w, r.h }, border_color);
            hud.draw_rect({ r.x + r.w - border_w, r.y, border_w, r.h }, border_color);
        }

        // Hotkey badge — only drawn when the slot resolves to a castable
        // ability. Empty slots and passive / aura bindings both hide
        // the label because pressing the key wouldn't do anything.
        // A small dark pill sits under the letter so it stays legible
        // against bright / busy icons (author-controlled via
        // `hotkey_badge_bg` in style_params; transparent disables it).
        bool show_hotkey = def && is_castable_form(def->form);
        if (!slot.hotkey.empty() && show_hotkey) {
            f32 px_size = slot.rect.h * 0.28f;
            if (px_size < 10.0f) px_size = 10.0f;
            f32 text_w = hud.text_width_px(slot.hotkey, px_size);
            f32 x_left = slot.rect.x + slot.rect.w - text_w - 4.0f;
            f32 ascent = hud.text_ascent_px(px_size);
            f32 y_base = slot.rect.y + ascent + 2.0f;

            // Backing pill. ~2px padding all around the glyph; stays
            // inside the slot so it never hangs off the edge.
            if ((cfg.style.hotkey_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f;
                f32 pad_y = 1.0f;
                Rect bg{
                    x_left - pad_x,
                    y_base - ascent - pad_y,
                    text_w + pad_x * 2.0f,
                    ascent + pad_y * 2.0f,
                };
                hud.draw_rect(bg, cfg.style.hotkey_badge_bg);
            }

            hud.draw_text(x_left, y_base, slot.hotkey, cfg.style.hotkey_color, px_size);
        }
    }
}

// Classification of a unit for minimap-dot coloring relative to the
// local player. Order matters: own > ally > enemy > neutral.
static Color minimap_dot_color(const WorldContext& ctx, u32 unit_id,
                               const MinimapStyle& style) {
    if (!ctx.world) return style.neutral_dot_color;
    const auto* owner = ctx.world->owners.get(unit_id);
    if (!owner) return style.neutral_dot_color;
    simulation::Player p = owner->player;
    if (p == ctx.local_player) return style.own_dot_color;
    // Need the simulation for the alliance lookup. WorldContext doesn't
    // carry it directly, but the Sim lives behind the AbilityRegistry
    // we already ship through — that's a coincidence, so for color
    // resolution we fall back to the raw comparison if we can't reach
    // the sim. In practice the ally/enemy split will come from the sim
    // once a minimap-owned `sim` pointer is added; keeping this gated.
    // For v1: same player → own, else enemy. Allies get enemy color
    // until the hook lands.
    return style.enemy_dot_color;
}

// Render the minimap for the current frame. v1: bg, border, unit dots
// (fog-filtered), hotkey label-less. No viewport outline yet — added in
// v2 once frustum-to-ground projection math lands.
static void draw_minimap(Hud& hud, Hud::Impl& s) {
    const auto& cfg = s.minimap_cfg;
    if (!cfg.enabled || !s.minimap_rt.visible) return;

    // Background + border.
    hud.draw_rect(cfg.rect, cfg.style.bg);
    f32 bw = cfg.style.border_width;
    if (bw > 0.0f && (cfg.style.border_color.rgba >> 24) != 0) {
        const Rect& r = cfg.rect;
        hud.draw_rect({ r.x, r.y, r.w, bw }, cfg.style.border_color);
        hud.draw_rect({ r.x, r.y + r.h - bw, r.w, bw }, cfg.style.border_color);
        hud.draw_rect({ r.x, r.y, bw, r.h }, cfg.style.border_color);
        hud.draw_rect({ r.x + r.w - bw, r.y, bw, r.h }, cfg.style.border_color);
    }

    // Unit dots. Need the world, terrain (for bounds mapping), and fog
    // (for visibility gating). Units the local player can't see are
    // skipped so the minimap doesn't leak enemy positions.
    if (!s.world_ctx || !s.world_ctx->world || !s.world_ctx->terrain) return;
    const auto& world = *s.world_ctx->world;
    const auto& td    = *s.world_ctx->terrain;
    const auto* fog   = s.world_ctx->fog;

    f32 inv_w = (td.world_width()  > 0.0f) ? (cfg.rect.w / td.world_width())  : 0.0f;
    f32 inv_h = (td.world_height() > 0.0f) ? (cfg.rect.h / td.world_height()) : 0.0f;

    for (u32 i = 0; i < world.transforms.count(); ++i) {
        u32 id = world.transforms.ids()[i];
        const auto& tf = world.transforms.data()[i];

        // Only real units with handle_info. Skip dead units so corpses
        // don't clutter the map.
        const auto* info = world.handle_infos.get(id);
        if (!info || info->category != simulation::Category::Unit) continue;
        if (const auto* hp = world.healths.get(id); hp && hp->current <= 0.0f) continue;

        // Fog gate — tile lookup in terrain space.
        if (fog) {
            i32 tx = static_cast<i32>((tf.position.x - td.origin_x()) / td.tile_size);
            i32 ty = static_cast<i32>((tf.position.y - td.origin_y()) / td.tile_size);
            if (tx >= 0 && ty >= 0 &&
                static_cast<u32>(tx) < td.tiles_x &&
                static_cast<u32>(ty) < td.tiles_y) {
                if (!fog->is_visible(s.world_ctx->local_player,
                                     static_cast<u32>(tx), static_cast<u32>(ty))) {
                    continue;
                }
            }
        }

        // Project to minimap pixels. Dot is a small centered square.
        f32 sx = cfg.rect.x + (tf.position.x - td.origin_x()) * inv_w;
        f32 sy = cfg.rect.y + (tf.position.y - td.origin_y()) * inv_h;
        f32 half = cfg.style.dot_size * 0.5f;
        Rect dot{ sx - half, sy - half, cfg.style.dot_size, cfg.style.dot_size };
        hud.draw_rect(dot, minimap_dot_color(*s.world_ctx, id, cfg.style));
    }
}

// Render the action bar for the current frame. Called from draw_tree()
// so composites participate in the same frame-build as atom nodes.
// Dispatches to the matching `style_id` render variant — each variant
// has its own fixed layer order and parameter block (see action_bar.h).
static void draw_action_bar(Hud& hud, Hud::Impl& s) {
    const auto& cfg = s.action_bar_cfg;
    if (!cfg.enabled) return;
    if (!s.action_bar_rt.visible) return;

    // Optional bar-level background fill. Transparent by default.
    if ((cfg.style.bg.rgba >> 24) != 0) {
        hud.draw_rect(cfg.rect, cfg.style.bg);
    }

    switch (cfg.style_id) {
        case ActionBarStyleId::ClassicRts:
            draw_action_bar_classic_rts(hud, s);
            break;
    }
}

void Hud::draw_tree() {
    if (!m_impl || !m_impl->frame_open || !m_impl->root) return;
    m_impl->root->draw(*this);
    draw_action_bar(*this, *m_impl);
    draw_minimap(*this, *m_impl);
}

// Hit-test helper. Walks children back-to-front (reverse iteration) so a
// later-drawn child that sits on top wins over earlier siblings. Invisible
// subtrees and non-owned-by-local ones are skipped entirely — clicks fall
// through to the world just as if the foreign-player UI weren't drawn.
static Node* hit_test_tree(Node* node, f32 x, f32 y, u32 local_player) {
    if (!node || !node->visible) return nullptr;
    if (!node->is_owned_by(local_player)) return nullptr;
    // Children first — they draw on top.
    const auto& kids = node->children();
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        if (Node* hit = hit_test_tree(it->get(), x, y, local_player)) return hit;
    }
    // Then self.
    if (node->hit_test(x, y)) return node;
    return nullptr;
}

void Hud::handle_pointer(f32 x, f32 y, bool button_down) {
    if (!m_impl) return;
    auto& s = *m_impl;
    s.pointer_x = x;
    s.pointer_y = y;

    // Composites sit on top of the node tree (drawn last in draw_tree);
    // hit-test them first so clicks beat anything underneath. Either
    // capture suppresses the tree hit-test entirely.
    i32  bar_slot    = action_bar_hit_test(s, x, y);
    bool on_minimap  = (bar_slot < 0) && minimap_hit_test(s, x, y);

    // Hover tracking for the bar. `slot.hovered` drives the classic_rts
    // style's hover_bg swap; clearing the old slot before setting the
    // new one avoids two slots visually hovered simultaneously.
    if (bar_slot != s.action_bar_hover_slot) {
        if (s.action_bar_hover_slot >= 0 &&
            static_cast<u32>(s.action_bar_hover_slot) < s.action_bar_cfg.slots.size()) {
            s.action_bar_cfg.slots[s.action_bar_hover_slot].hovered = false;
        }
        if (bar_slot >= 0) {
            s.action_bar_cfg.slots[bar_slot].hovered = true;
        }
        s.action_bar_hover_slot = bar_slot;
    }

    Node* under = nullptr;
    if (bar_slot < 0 && !on_minimap) {
        under = hit_test_tree(s.root.get(), x, y, s.local_player);
    }

    // Hover transitions for the node tree. Widgets don't track hover
    // across frames themselves — the HUD tells them when they enter /
    // leave the pointer. When the bar captures the pointer, `under` is
    // null, which naturally clears any previously-hovered tree node.
    if (under != s.hover) {
        if (s.hover)   s.hover->on_hover_change(false);
        s.hover = under;
        if (s.hover)   s.hover->on_hover_change(true);
    }

    // Press edge: down on this frame, was up last frame.
    if (button_down && !s.pointer_down_prev) {
        if (bar_slot >= 0) {
            s.action_bar_pressed_slot = bar_slot;
            s.action_bar_cfg.slots[bar_slot].pressed = true;
        } else if (on_minimap) {
            // Minimap click — jump the camera. The terrain is needed to
            // convert screen point to world point; silently ignore the
            // click if there's no terrain (e.g. before session start).
            if (s.world_ctx && s.world_ctx->terrain && s.minimap_jump_fn) {
                f32 wx = 0.0f, wy = 0.0f;
                minimap_point_to_world(s.minimap_cfg.rect, *s.world_ctx->terrain,
                                       x, y, wx, wy);
                s.minimap_jump_fn(wx, wy);
            }
        } else {
            s.pressed = under;
            if (s.pressed) s.pressed->on_press();
        }
    }
    // Release edge: up on this frame, was down last frame.
    if (!button_down && s.pointer_down_prev) {
        if (s.action_bar_pressed_slot >= 0) {
            // "Clicked" = released while still over the slot that was
            // pressed. Resolve the current selection → ability and fire
            // the cast callback; app routes it to the input preset so
            // the click behaves identically to pressing the slot's
            // hotkey letter.
            u32 idx = static_cast<u32>(s.action_bar_pressed_slot);
            bool over = (bar_slot == s.action_bar_pressed_slot);
            if (idx < s.action_bar_cfg.slots.size()) {
                auto& slot = s.action_bar_cfg.slots[idx];
                slot.pressed = false;
                if (over && s.world_ctx && s.action_bar_cast_fn) {
                    const simulation::AbilityDef* def = nullptr;
                    const simulation::AbilityInstance* inst =
                        resolve_slot_ability(idx, s.action_bar_cfg, s.action_bar_rt,
                                             *s.world_ctx, def);
                    if (inst && def) {
                        u32 unit_id = s.world_ctx->selection
                                        ? s.world_ctx->selection->selected().front().id
                                        : UINT32_MAX;
                        if (unit_id != UINT32_MAX &&
                            slot_castable_now(*s.world_ctx, unit_id, *inst, *def)) {
                            s.action_bar_cast_fn(inst->ability_id);
                        }
                    }
                }
            }
            s.action_bar_pressed_slot = -1;
        } else if (s.pressed) {
            bool over = (under == s.pressed);
            std::string clicked_id = s.pressed->id;
            bool clicked = s.pressed->on_release(over);
            s.pressed = nullptr;
            if (clicked && !clicked_id.empty()) fire_button_event(clicked_id);
        }
    }

    s.pointer_down_prev = button_down;
}

bool Hud::input_captured() const {
    if (!m_impl) return false;
    // Pointer over (or holding) any HUD surface that takes pointer input.
    return m_impl->hover != nullptr || m_impl->pressed != nullptr
        || m_impl->action_bar_hover_slot   >= 0
        || m_impl->action_bar_pressed_slot >= 0
        || minimap_hit_test(*m_impl, m_impl->pointer_x, m_impl->pointer_y);
}

f32 Hud::pointer_x() const { return m_impl ? m_impl->pointer_x : 0.0f; }
f32 Hud::pointer_y() const { return m_impl ? m_impl->pointer_y : 0.0f; }

void Hud::set_sync_fn(SyncFn fn) { if (m_impl) m_impl->sync_fn = std::move(fn); }

void Hud::set_button_event_fn(ButtonEventFn fn) {
    if (m_impl) m_impl->button_event_fn = std::move(fn);
}

void Hud::fire_button_event(const std::string& node_id) {
    if (!m_impl || !m_impl->button_event_fn) return;
    m_impl->button_event_fn(node_id);
}

// Definition (matching the forward decl near the top of the file).
static void emit_sync(Hud::Impl& s, const std::vector<u8>& pkt, u32 owner) {
    if (s.sync_fn) s.sync_fn(pkt, owner);
}

void Hud::set_label_text(std::string_view id, std::string_view text) {
    if (!m_impl) return;
    auto* n = find_node_by_id(id);
    if (!n) return;
    if (auto* l = dynamic_cast<hud::Label*>(n)) {
        l->text.assign(text);
        emit_sync(*m_impl,
                  uldum::network::build_hud_set_label_text(id, text),
                  n->owner_player);
    }
}

void Hud::set_bar_fill(std::string_view id, f32 fill) {
    if (!m_impl) return;
    auto* n = find_node_by_id(id);
    if (!n) return;
    if (auto* b = dynamic_cast<hud::Bar*>(n)) {
        b->fill = fill;
        emit_sync(*m_impl,
                  uldum::network::build_hud_set_bar_fill(id, fill),
                  n->owner_player);
    }
}

void Hud::set_node_visible(std::string_view id, bool visible) {
    if (!m_impl) return;
    auto* n = find_node_by_id(id);
    if (!n) return;
    n->visible = visible;
    emit_sync(*m_impl,
              uldum::network::build_hud_set_node_visible(id, visible),
              n->owner_player);
}

void Hud::set_image_source(std::string_view id, std::string_view source) {
    if (!m_impl) return;
    auto* n = find_node_by_id(id);
    if (!n) return;
    if (auto* im = dynamic_cast<hud::Image*>(n)) {
        im->source.assign(source);
        emit_sync(*m_impl,
                  uldum::network::build_hud_set_image_source(id, source),
                  n->owner_player);
    }
}

void Hud::set_button_enabled(std::string_view id, bool enabled) {
    if (!m_impl) return;
    auto* n = find_node_by_id(id);
    if (!n) return;
    if (auto* btn = dynamic_cast<hud::Button*>(n)) {
        btn->enabled = enabled;
        emit_sync(*m_impl,
                  uldum::network::build_hud_set_button_enabled(id, enabled),
                  n->owner_player);
    }
}

void Hud::draw_rect(const Rect& r, Color color) {
    if (!m_impl || !m_impl->frame_open) return;
    ensure_batch(*m_impl, PIPE_SOLID, m_impl->white_set);
    append_quad(*m_impl, r, 0.0f, 0.0f, 1.0f, 1.0f, premul_rgba(color));
}

void Hud::draw_image(const Rect& r, std::string_view asset_path, Color tint) {
    if (!m_impl || !m_impl->frame_open) return;
    HudImage* img = get_or_load_image(*m_impl, asset_path);
    if (!img) return;
    ensure_batch(*m_impl, PIPE_SOLID, img->set);
    append_quad(*m_impl, r, 0.0f, 0.0f, 1.0f, 1.0f, premul_rgba(tint));
}

// Minimal UTF-8 decoder. Advances `p` past one codepoint; returns 0 at
// end of input and U+FFFD for invalid sequences. Good enough for BMP +
// supplementary planes; doesn't validate overlongs.
static u32 utf8_next(const char*& p, const char* end) {
    if (p >= end) return 0;
    u8 b0 = static_cast<u8>(*p++);
    if (b0 < 0x80) return b0;
    int extra = 0;
    u32 cp = 0;
    if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1Fu; extra = 1; }
    else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0Fu; extra = 2; }
    else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07u; extra = 3; }
    else return 0xFFFDu;
    for (int i = 0; i < extra; ++i) {
        if (p >= end) return 0xFFFDu;
        u8 bx = static_cast<u8>(*p++);
        if ((bx & 0xC0) != 0x80) return 0xFFFDu;
        cp = (cp << 6) | (bx & 0x3Fu);
    }
    return cp;
}

void Hud::draw_text(f32 x_left, f32 y_baseline, std::string_view utf8,
                    Color color, f32 px_size) {
    if (!m_impl || !m_impl->frame_open) return;
    auto& s = *m_impl;
    if (!s.font || !s.font->valid()) return;

    ensure_batch(s, PIPE_TEXT, s.font->atlas_descriptor());
    u32 premul = premul_rgba(color);
    f32 pen = x_left;

    const char* p   = utf8.data();
    const char* end = p + utf8.size();
    while (p < end) {
        u32 cp = utf8_next(p, end);
        if (cp == 0) break;
        // Horizontal tab / newline are not handled here — single-line draw
        // for Stage C. Collapse tab to a space's advance; skip newlines.
        if (cp == '\n') continue;
        if (cp == '\t') cp = ' ';

        const Font::Glyph* g = s.font->get_glyph(cp);
        if (!g) continue;  // atlas full / unknown codepoint / whitespace
        if (g->plane_w > 0.0f && g->plane_h > 0.0f) {
            // em → on-screen pixel scale. bearing_y is ascent-positive so
            // the glyph's top edge in screen coords is baseline - bearing_y.
            f32 x0 = pen + g->bearing_x * px_size;
            f32 y0 = y_baseline - g->bearing_y * px_size;
            Rect qr{ x0, y0, g->plane_w * px_size, g->plane_h * px_size };
            append_quad(s, qr, g->uv0[0], g->uv0[1], g->uv1[0], g->uv1[1], premul);
        }
        pen += g->advance * px_size;
    }
}

void Hud::render(VkCommandBuffer cmd) {
    if (!m_impl || !m_impl->frame_open) return;
    m_impl->frame_open = false;

    auto& s = *m_impl;
    if (s.verts.empty() || s.inds.empty() || s.batches.empty()) return;
    if (s.screen_w == 0 || s.screen_h == 0) return;

    // Close the final batch so the draw loop knows its extent.
    s.batches.back().index_count = static_cast<u32>(s.inds.size()) - s.batches.back().index_start;

    u32 slot = s.rhi->frame_index();
    RingBuffer& ring = s.rings[slot];
    std::memcpy(ring.vb_mapped, s.verts.data(), s.verts.size() * sizeof(Vertex));
    std::memcpy(ring.ib_mapped, s.inds.data(),  s.inds.size()  * sizeof(u16));

    VkViewport vp{};
    vp.x        = 0.0f;
    vp.y        = 0.0f;
    vp.width    = static_cast<f32>(s.screen_w);
    vp.height   = static_cast<f32>(s.screen_h);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = { s.screen_w, s.screen_h };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Ortho mapping screen-space (0,0) top-left → (w,h) bottom-right into
    // Vulkan NDC. Vulkan's NDC is +Y down; glm::ortho is OpenGL convention
    // (+Y up), so we pass bottom=0, top=h to flip Y into Vulkan's space.
    glm::mat4 mvp = glm::ortho(0.0f, static_cast<f32>(s.screen_w),
                               0.0f, static_cast<f32>(s.screen_h),
                               -1.0f, 1.0f);
    vkCmdPushConstants(cmd, s.pipe_layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(glm::mat4), glm::value_ptr(mvp));

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &ring.vb, &offset);
    vkCmdBindIndexBuffer(cmd, ring.ib, 0, VK_INDEX_TYPE_UINT16);

    // Walk batches, re-binding pipeline + descriptor when they change.
    // Track last bound values so back-to-back batches with the same kind
    // (shouldn't happen, but harmless) don't issue redundant binds.
    VkPipeline      last_pipe = VK_NULL_HANDLE;
    VkDescriptorSet last_set  = VK_NULL_HANDLE;
    for (const Batch& b : s.batches) {
        if (b.index_count == 0) continue;
        VkPipeline pipe = (b.pipeline == PIPE_TEXT) ? s.pipe_text : s.pipe_solid;
        if (pipe != last_pipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            last_pipe = pipe;
        }
        if (b.desc_set != last_set) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    s.pipe_layout, 0, 1, &b.desc_set, 0, nullptr);
            last_set = b.desc_set;
        }
        vkCmdDrawIndexed(cmd, b.index_count, 1, b.index_start, 0, 0);
    }
}

f32 Hud::text_ascent_px(f32 px_size) const {
    if (!m_impl || !m_impl->font || !m_impl->font->valid()) return 0;
    return m_impl->font->ascent() * px_size;
}

f32 Hud::text_line_height_px(f32 px_size) const {
    if (!m_impl || !m_impl->font || !m_impl->font->valid()) return 0;
    return m_impl->font->line_height() * px_size;
}

f32 Hud::text_width_px(std::string_view utf8, f32 px_size) const {
    if (!m_impl || !m_impl->font || !m_impl->font->valid()) return 0.0f;
    auto& font = *m_impl->font;
    f32 width = 0.0f;
    const char* p   = utf8.data();
    const char* end = p + utf8.size();
    while (p < end) {
        u32 cp = utf8_next(p, end);
        if (cp == 0 || cp == '\n') break;
        if (cp == '\t') cp = ' ';
        const Font::Glyph* g = font.get_glyph(cp);
        if (!g) continue;
        width += g->advance * px_size;
    }
    return width;
}

} // namespace uldum::hud
