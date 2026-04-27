#include "hud/hud.h"
#include "hud/node.h"
#include "hud/font.h"
#include "hud/world.h"
#include "hud/text_tag.h"
#include "hud/hud_loader.h"
#include "hud/action_bar.h"
#include "hud/minimap.h"
#include "hud/command_bar.h"
#include "hud/joystick.h"
#include "hud/cast_indicator.h"
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
#include "input/picking.h"
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
#include <unordered_set>
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
                     const WorldContext& ctx,
                     const simulation::AbilityDef*& out_def);

// Same-file forward decl for the affordability + cooldown gate, used by
// both click and keyboard dispatch. Defined alongside the render path.
static bool slot_castable_now(const WorldContext& ctx, u32 unit_id,
                              const simulation::AbilityInstance& inst,
                              const simulation::AbilityDef& def);
static bool is_castable_form(simulation::AbilityForm f);

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
    // `screen_w/h` = logical (dp) HUD dimensions — what author-facing
    // coordinates live in. Computed each frame as physical / ui_scale.
    // `physical_w/h` = raw framebuffer extent for the Vulkan viewport.
    // `ui_scale` = physical pixels per dp, set from the platform layer.
    u32 screen_w = 0;
    u32 screen_h = 0;
    u32 physical_w = 0;
    u32 physical_h = 0;
    f32 ui_scale = 1.0f;
    bool is_mobile = false;
    Hud::SafeInsets safe_insets{};
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
    Hud::ActionBarCastAtTargetFn action_bar_cast_at_target_fn;

    // Mobile drag-cast gesture state. One slot owns the gesture at a
    // time (mobile = single-finger drag). All fields meaningful only
    // while phase != Idle. Coordinates are in dp (HUD logical space).
    enum class DragCastPhase : u8 { Idle, Pressed, Aiming, Cancelling };
    struct DragCastState {
        DragCastPhase phase = DragCastPhase::Idle;
        i32          slot_index = -1;
        f32          press_x = 0, press_y = 0;
        f32          current_x = 0, current_y = 0;
        // Caster handle snapshotted on press; the *position* is read
        // live each frame from the world so a unit moving mid-drag
        // drags its range ring along (and the drag-point's world
        // origin updates accordingly).
        simulation::Unit caster{};
        f32          caster_x = 0, caster_y = 0, caster_z = 0;
        // Drag point in world space, recomputed each frame.
        f32          drag_world_x = 0, drag_world_y = 0, drag_world_z = 0;
        // Ability snapshotted on press (id + range + form). Snapshot
        // because the unit's ability list can mutate mid-drag.
        std::string  ability_id;
        f32          range = 0;
        simulation::AbilityForm form = simulation::AbilityForm::Passive;
        simulation::IndicatorShape shape = simulation::IndicatorShape::Point;
        f32          area_radius = 0;
        f32          area_width  = 0;   // Line
        f32          area_angle  = 0;   // Cone, degrees
        // Snapped target unit (target_unit form only). Invalid when
        // not snapped.
        simulation::Unit snapped_target{};
    };
    DragCastState drag_cast;

    // Ability id the input preset is currently waiting for a target
    // on. Pushed by the app each frame; empty = no armed ability.
    // Drives the slot "held down" render treatment so the player can
    // see which ability they're about to commit.
    std::string action_bar_targeting_ability;

    // Minimap composite.
    MinimapConfig  minimap_cfg{};
    MinimapRuntime minimap_rt{};
    Hud::MinimapJumpFn minimap_jump_fn;
    // True between press-on-minimap and the matching release. While set,
    // pointer moves keep firing minimap_jump_fn so the camera follows
    // the finger across the minimap (RTS convention). Latches across
    // pointer leaves so dragging off the minimap edge still scrolls
    // the world to the projected position.
    bool minimap_dragging = false;

    // Command-bar composite. Same hit-test machinery pattern as
    // action_bar (per-slot hovered/pressed + -1-indexed latches).
    CommandBarConfig  command_bar_cfg{};
    CommandBarRuntime command_bar_rt{};
    i32 command_bar_hover_slot   = -1;
    i32 command_bar_pressed_slot = -1;
    Hud::CommandFn command_bar_fn;
    std::string command_bar_armed_command;

    // Joystick composite. Captured touch slot + last knob offset live
    // in JoystickRuntime; `joystick_update` re-evaluates them each
    // frame against the current InputState.
    JoystickConfig  joystick_cfg{};
    JoystickRuntime joystick_rt{};

    // Cast indicator style — applied to range ring, arrow, reticle,
    // AoE indicator, target-unit ring, and per-phase tints. Drives
    // AbilityIndicators calls in the app each frame.
    CastIndicatorConfig cast_indicator_cfg{};

    // Rising-edge tracking for hidden-ability hotkeys (abilities
    // authored with `hidden: true` that don't live in any bar slot
    // but are still keyboard-triggerable).
    std::unordered_map<std::string, bool> hidden_hotkey_prev;

    // Box-select marquee — per-map style, authored in hud.json.
    Hud::MarqueeStyle marquee_style{};

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
    // Icon PNGs are authored in sRGB; the swapchain is sRGB too. Using an
    // _SRGB image view lets the sampler de-gamma on read so the shader sees
    // linear values, which the framebuffer then re-encodes correctly. Using
    // _UNORM here would feed sRGB-encoded bytes through as "linear", which
    // the sRGB framebuffer re-encodes a second time — washed-out mid-tones.
    ici.format        = VK_FORMAT_R8G8B8A8_SRGB;
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
    vci.format           = VK_FORMAT_R8G8B8A8_SRGB;
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
    // Caller supplies physical framebuffer dims; convert to logical
    // (dp) before re-resolving composite rects so anchors match what
    // begin_frame will use next frame.
    f32 s = m_impl->ui_scale;
    if (s <= 0.0f) s = 1.0f;
    f32 view_w_dp = static_cast<f32>(screen_w) / s;
    f32 view_h_dp = static_cast<f32>(screen_h) / s;

    // Safe-area insets come from the platform in physical pixels
    // (Android's GameActivity insets are px-units, Windows reports zero).
    // Convert to dp and shrink the viewport so composites anchored `tr`
    // don't slide under the status bar / notch, and `br` anchors don't
    // slide under the navigation bar. Clamp the resulting interior to
    // non-negative in case insets somehow exceed the framebuffer.
    const auto& ins = m_impl->safe_insets;
    f32 left   = ins.left   / s;
    f32 top    = ins.top    / s;
    f32 right  = ins.right  / s;
    f32 bottom = ins.bottom / s;
    f32 inner_w = view_w_dp - left - right;
    f32 inner_h = view_h_dp - top  - bottom;
    if (inner_w < 0.0f) inner_w = 0.0f;
    if (inner_h < 0.0f) inner_h = 0.0f;
    Rect viewport{ left, top, inner_w, inner_h };

    // Action bar — bar rect anchors against viewport; each slot then
    // anchors against the new bar rect, so slot ordering matters.
    auto& ab = m_impl->action_bar_cfg;
    if (ab.enabled) {
        ab.rect = resolve(viewport, ab.placement);
        for (auto& slot : ab.slots) {
            slot.rect = resolve(ab.rect, slot.placement);
        }
        // Cancel zone anchors against the viewport (NOT the bar rect)
        // — its job is to be reachable from anywhere on screen during
        // a drag, so it shouldn't shrink with the bar's footprint.
        ab.cancel_zone_rect = resolve(viewport, ab.cancel_zone_placement);
    }

    // Minimap — single rect against viewport.
    auto& mm = m_impl->minimap_cfg;
    if (mm.enabled) {
        mm.rect = resolve(viewport, mm.placement);
    }

    // Command bar — same structure as action_bar: bar anchors against
    // viewport, slots anchor against the new bar rect.
    auto& cb = m_impl->command_bar_cfg;
    if (cb.enabled) {
        cb.rect = resolve(viewport, cb.placement);
        for (auto& slot : cb.slots) {
            slot.rect = resolve(cb.rect, slot.placement);
        }
    }

    // Joystick — base rect + optional larger activation rect. Both
    // anchor against the viewport.
    auto& js = m_impl->joystick_cfg;
    if (js.enabled) {
        js.rect = resolve(viewport, js.placement);
        if (js.has_activation) {
            js.activation_rect = resolve(viewport, js.activation_placement);
        } else {
            js.activation_rect = js.rect;
        }
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
    // Caller passes physical framebuffer pixels. Divide by the
    // platform-provided px-per-dp to get logical dims (dp) — that's
    // the space every HUD coordinate and hit-test lives in.
    f32 s = m_impl->ui_scale;
    m_impl->physical_w = screen_w;
    m_impl->physical_h = screen_h;
    m_impl->screen_w   = static_cast<u32>(static_cast<f32>(screen_w) / s);
    m_impl->screen_h   = static_cast<u32>(static_cast<f32>(screen_h) / s);
    m_impl->frame_open = true;
    if (m_impl->root) {
        m_impl->root->rect = { 0.0f, 0.0f,
                               static_cast<f32>(m_impl->screen_w),
                               static_cast<f32>(m_impl->screen_h) };
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

void Hud::reset_session_state() {
    if (!m_impl) return;
    auto& s = *m_impl;

    // Widget tree + transient input pointers (uses clear_nodes path).
    if (s.root) s.root->clear_children();
    s.hover   = nullptr;
    s.pressed = nullptr;

    // Text tags — the user-reported leak. Without this, tags created
    // by session A's Lua scripts kept rendering at session B's start
    // because the pool just kept its TextTagEntry vector intact.
    s.text_tags.clear();

    // Mobile drag-cast: whatever was in flight at session end is gone.
    s.drag_cast = Impl::DragCastState{};

    // Composite slot interaction state (hover/pressed/armed).
    s.action_bar_hover_slot      = -1;
    s.action_bar_pressed_slot    = -1;
    s.action_bar_targeting_ability.clear();
    s.command_bar_hover_slot     = -1;
    s.command_bar_pressed_slot   = -1;
    s.command_bar_armed_command.clear();
    s.minimap_dragging           = false;

    // Composite configs + runtime. The next map's hud.json reload
    // refills any composite it declares; clearing here ensures a
    // map that omits a composite doesn't inherit the previous map's
    // config.
    s.action_bar_cfg     = {};
    s.action_bar_rt      = {};
    s.command_bar_cfg    = {};
    s.command_bar_rt     = {};
    s.minimap_cfg        = {};
    s.minimap_rt         = {};
    s.joystick_cfg       = {};
    s.joystick_rt        = {};
    s.cast_indicator_cfg = {};
    s.world_cfg          = {};
    s.marquee_style      = {};

    // Node templates from previous map's `nodes` block.
    s.node_templates.clear();

    // Edge-tracking for hidden-ability hotkeys (rising-edge map keyed
    // by ability id). Stale entries from session A would mis-fire
    // (or fail to fire) on session B's first frame.
    s.hidden_hotkey_prev.clear();

    // Pointer state — fresh session, no in-progress press.
    s.pointer_x = 0;
    s.pointer_y = 0;
    s.pointer_down_prev = false;

    // Local player will be set again by App on session start.
    s.local_player = UINT32_MAX;

    // Callbacks (sync_fn / button_event_fn / action_bar_cast_*) are
    // re-installed by App in start_session(); leaving stale ones here
    // could fire into a dead App state in the gap. Clear them.
    s.sync_fn = {};
    s.button_event_fn = {};
    s.action_bar_cast_fn = {};
    s.action_bar_cast_at_target_fn = {};
    s.minimap_jump_fn = {};
    s.command_bar_fn = {};
}

void Hud::set_local_player(u32 player_id) {
    if (m_impl) m_impl->local_player = player_id;
}
u32 Hud::local_player() const {
    return m_impl ? m_impl->local_player : UINT32_MAX;
}

void Hud::set_ui_scale(f32 px_per_dp) {
    if (!m_impl) return;
    // Guard against non-positive values from misbehaving platform code —
    // dividing by zero later would zero the whole HUD into a point.
    m_impl->ui_scale = (px_per_dp > 0.0f) ? px_per_dp : 1.0f;
}
f32 Hud::ui_scale() const { return m_impl ? m_impl->ui_scale : 1.0f; }

void Hud::set_is_mobile(bool mobile) {
    if (m_impl) m_impl->is_mobile = mobile;
}
bool Hud::is_mobile() const { return m_impl ? m_impl->is_mobile : false; }

void Hud::set_safe_insets(const SafeInsets& insets) {
    if (!m_impl) return;
    m_impl->safe_insets = insets;
    // If a viewport has already been established, re-resolve composite
    // rects immediately so the new insets take effect this frame. (Init
    // order: on desktop app pushes insets before the first begin_frame,
    // so physical_w/h are still 0 and the next begin_frame picks them
    // up for free. On Android's insets-change-without-resize path we
    // need the re-resolve.)
    if (m_impl->physical_w > 0 && m_impl->physical_h > 0) {
        on_viewport_resized(m_impl->physical_w, m_impl->physical_h);
    }
}

Hud::SafeInsets Hud::safe_insets() const {
    return m_impl ? m_impl->safe_insets : SafeInsets{};
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

void Hud::set_action_bar_cast_at_target_fn(ActionBarCastAtTargetFn fn) {
    if (m_impl) m_impl->action_bar_cast_at_target_fn = std::move(fn);
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
    // Minimap Y is flipped (north at top, south at bottom) to match WC3.
    // Invert fy so the click lands at the world position the dot sat on.
    wy = td.origin_y() + (1.0f - fy) * td.world_height();
}

// ── Command-bar composite ────────────────────────────────────────────────

void Hud::set_command_bar_config(const CommandBarConfig& cfg) {
    if (!m_impl) return;
    m_impl->command_bar_cfg = cfg;
    m_impl->command_bar_rt  = CommandBarRuntime{};
    m_impl->command_bar_hover_slot   = -1;
    m_impl->command_bar_pressed_slot = -1;
}

void Hud::command_bar_set_visible(bool visible) {
    if (m_impl) m_impl->command_bar_rt.visible = visible;
}

void Hud::set_command_bar_fn(CommandFn fn) {
    if (m_impl) m_impl->command_bar_fn = std::move(fn);
}

void Hud::command_bar_set_armed_command(std::string_view command_id) {
    if (m_impl) m_impl->command_bar_armed_command.assign(command_id);
}

// Return the command-bar slot index under (x, y), or -1 if none.
// Mirrors action_bar_hit_test — same shape, different struct.
static i32 command_bar_hit_test(const Hud::Impl& s, f32 x, f32 y) {
    const auto& cfg = s.command_bar_cfg;
    if (!cfg.enabled || !s.command_bar_rt.visible) return -1;
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

// Render the command bar for the current frame. Reuses the classic_rts
// look the action_bar uses. Armed state (slot whose command matches
// the preset's current targeting mode) renders with press_bg + the
// accent border, identical to the action_bar's armed ability slot.
static void draw_command_bar(Hud& hud, Hud::Impl& s) {
    const auto& cfg = s.command_bar_cfg;
    if (!cfg.enabled || !s.command_bar_rt.visible) return;

    if ((cfg.style.bg.rgba >> 24) != 0) hud.draw_rect(cfg.rect, cfg.style.bg);

    const std::string& armed = s.command_bar_armed_command;

    for (const auto& slot : cfg.slots) {
        if (!slot.visible) continue;
        bool is_armed = (!armed.empty() && slot.command == armed);

        Color bg = slot.style.bg;
        if (slot.pressed)      bg = slot.style.press_bg;
        else if (is_armed)     bg = slot.style.press_bg;   // "held down" look
        else if (slot.hovered) bg = slot.style.hover_bg;
        hud.draw_rect(slot.rect, bg);

        f32 bw = (slot.style.border_width > 0.0f) ? slot.style.border_width : 0.0f;
        Rect icon_rect{ slot.rect.x + bw, slot.rect.y + bw,
                        slot.rect.w - bw * 2.0f, slot.rect.h - bw * 2.0f };
        if (!slot.icon.empty()) {
            hud.draw_image(icon_rect, slot.icon);
        }

        // Border — armed gets the accent color + thicker stroke.
        Color border_color = slot.style.border_color;
        f32   border_w     = bw;
        if (is_armed) {
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

        if (!slot.hotkey.empty()) {
            f32 px_size = slot.rect.h * 0.28f;
            if (px_size < 10.0f) px_size = 10.0f;
            f32 text_w = hud.text_width_px(slot.hotkey, px_size);
            f32 x_left = slot.rect.x + slot.rect.w - text_w - 4.0f;
            f32 ascent = hud.text_ascent_px(px_size);
            f32 y_base = slot.rect.y + ascent + 2.0f;
            if ((cfg.style.hotkey_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f, pad_y = 1.0f;
                Rect bg_pill{
                    x_left - pad_x, y_base - ascent - pad_y,
                    text_w + pad_x * 2.0f, ascent + pad_y * 2.0f,
                };
                hud.draw_rect(bg_pill, cfg.style.hotkey_badge_bg);
            }
            hud.draw_text(x_left, y_base, slot.hotkey, cfg.style.hotkey_color, px_size);
        }
    }
}

// ── Joystick composite ───────────────────────────────────────────────────

static void draw_filled_circle(Hud::Impl& s, f32 cx, f32 cy, f32 r, Color color) {
    if (r <= 0.0f) return;
    if ((color.rgba >> 24) == 0) return;
    ensure_batch(s, PIPE_SOLID, s.white_set);
    u32 premul = premul_rgba(color);

    constexpr u32 kSegments = 32;
    constexpr f32 TWO_PI = 6.2831853f;
    f32 step = TWO_PI / static_cast<f32>(kSegments);

    f32 px0 = cx + r;
    f32 py0 = cy;
    for (u32 i = 0; i < kSegments; ++i) {
        f32 a = step * static_cast<f32>(i + 1);
        f32 px1 = cx + std::cos(a) * r;
        f32 py1 = cy + std::sin(a) * r;
        append_triangle(s, cx, cy, px0, py0, px1, py1, 0.0f, 0.0f, premul);
        px0 = px1;
        py0 = py1;
    }
}

// Border as a ring of quads between radius r_outer and r_inner. Simpler
// than a stroked circle and uses the same PIPE_SOLID batch.
static void draw_ring(Hud::Impl& s, f32 cx, f32 cy, f32 r_outer, f32 r_inner, Color color) {
    if (r_outer <= r_inner || r_outer <= 0.0f) return;
    if ((color.rgba >> 24) == 0) return;
    ensure_batch(s, PIPE_SOLID, s.white_set);
    u32 premul = premul_rgba(color);

    constexpr u32 kSegments = 32;
    constexpr f32 TWO_PI = 6.2831853f;
    f32 step = TWO_PI / static_cast<f32>(kSegments);

    for (u32 i = 0; i < kSegments; ++i) {
        f32 a0 = step * static_cast<f32>(i);
        f32 a1 = step * static_cast<f32>(i + 1);
        f32 c0 = std::cos(a0), s0 = std::sin(a0);
        f32 c1 = std::cos(a1), s1 = std::sin(a1);
        // Quad: outer(i), outer(i+1), inner(i+1), inner(i) — emitted as
        // two triangles.
        f32 ox0 = cx + c0 * r_outer, oy0 = cy + s0 * r_outer;
        f32 ox1 = cx + c1 * r_outer, oy1 = cy + s1 * r_outer;
        f32 ix0 = cx + c0 * r_inner, iy0 = cy + s0 * r_inner;
        f32 ix1 = cx + c1 * r_inner, iy1 = cy + s1 * r_inner;
        append_triangle(s, ox0, oy0, ox1, oy1, ix1, iy1, 0.0f, 0.0f, premul);
        append_triangle(s, ox0, oy0, ix1, iy1, ix0, iy0, 0.0f, 0.0f, premul);
    }
}

void Hud::set_joystick_config(const JoystickConfig& cfg) {
    if (!m_impl) return;
    m_impl->joystick_cfg = cfg;
    m_impl->joystick_rt  = JoystickRuntime{};
    // Seed the runtime's base center at the home rect center so the
    // first frame's render doesn't snap from (0, 0).
    m_impl->joystick_rt.base_cx = cfg.rect.x + cfg.rect.w * 0.5f;
    m_impl->joystick_rt.base_cy = cfg.rect.y + cfg.rect.h * 0.5f;
}

void Hud::set_cast_indicator_config(const CastIndicatorConfig& cfg) {
    if (!m_impl) return;
    m_impl->cast_indicator_cfg = cfg;
}

const CastIndicatorStyle& Hud::cast_indicator_style() const {
    static const CastIndicatorStyle kDefaultFallback{};
    return m_impl ? m_impl->cast_indicator_cfg.style : kDefaultFallback;
}

void Hud::joystick_set_visible(bool visible) {
    if (m_impl) m_impl->joystick_rt.visible = visible;
}

void Hud::joystick_vector(f32& dx, f32& dy) const {
    if (!m_impl) { dx = 0; dy = 0; return; }
    dx = m_impl->joystick_rt.out_x;
    dy = m_impl->joystick_rt.out_y;
}

bool Hud::joystick_active() const {
    return m_impl && m_impl->joystick_rt.captured_slot >= 0;
}

// Does (x, y) fall inside the joystick's activation region? That's the
// area where a press captures the stick (v2: optionally larger than the
// visible base so the player doesn't have to aim). Uses a rect — not a
// circle — so authors can cover an entire screen corner.
static bool joystick_hit_test_point(const JoystickConfig& cfg, f32 x, f32 y) {
    if (!cfg.enabled) return false;
    const Rect& r = cfg.activation_rect;
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

void Hud::joystick_update(const platform::InputState& input) {
    if (!m_impl) return;
    auto& s = *m_impl;
    auto& cfg = s.joystick_cfg;
    auto& rt  = s.joystick_rt;

    // Default: no output. Set below if a finger is driving the stick.
    rt.out_x = 0.0f;
    rt.out_y = 0.0f;

    // Home position — where the base rests when idle.
    f32 home_cx = cfg.rect.x + cfg.rect.w * 0.5f;
    f32 home_cy = cfg.rect.y + cfg.rect.h * 0.5f;

    if (!cfg.enabled || !rt.visible) {
        rt.captured_slot = -1;
        rt.knob_dx = rt.knob_dy = 0.0f;
        rt.base_cx = home_cx;
        rt.base_cy = home_cy;
        return;
    }

    f32 base_r = std::min(cfg.rect.w, cfg.rect.h) * 0.5f;
    f32 knob_r = base_r * cfg.style.knob_size_frac * 0.5f;
    // Travel radius: how far the knob center can move from the base
    // center. Keep it >= 1 px to avoid divide-by-zero in normalization.
    f32 travel = base_r - knob_r;
    if (travel < 1.0f) travel = 1.0f;

    // Translate input from physical pixels (what Platform::input delivers)
    // to the logical dp space the HUD lives in.
    f32 scale = s.ui_scale > 0.0f ? s.ui_scale : 1.0f;
    auto map_x = [scale](f32 px) { return px / scale; };
    auto map_y = [scale](f32 px) { return px / scale; };

    // Find the driver of the stick. Priority: if we already captured a
    // slot, keep following that slot until it releases. Otherwise, on a
    // fresh press look for any touch (or mouse, if no touches) inside
    // the activation region and capture it.
    auto finger_pos = [&](i32 slot, f32& fx, f32& fy) -> bool {
        if (slot < 0) return false;
        if (slot == 0 && input.touch_count == 0) {
            // Mouse path — primary pointer.
            fx = map_x(input.mouse_x);
            fy = map_y(input.mouse_y);
            return input.mouse_left;
        }
        if (static_cast<u32>(slot) < input.touch_count) {
            fx = map_x(input.touch_x[slot]);
            fy = map_y(input.touch_y[slot]);
            return true;
        }
        return false;
    };

    if (rt.captured_slot >= 0) {
        f32 fx, fy;
        if (!finger_pos(rt.captured_slot, fx, fy)) {
            // Finger released (or touch slot re-used by a different
            // gesture — safer to drop than risk a jump). Return the
            // base to its home position so the next idle render shows
            // the configured anchor, not the last press point.
            rt.captured_slot = -1;
            rt.knob_dx = rt.knob_dy = 0.0f;
            rt.base_cx = home_cx;
            rt.base_cy = home_cy;
            return;
        }
        // Knob offset is relative to the re-anchored base center, not
        // the home center. That way the resting-finger position is the
        // knob's neutral.
        f32 ex = fx - rt.base_cx;
        f32 ey = fy - rt.base_cy;
        f32 mag2 = ex * ex + ey * ey;
        if (mag2 > travel * travel) {
            f32 mag = std::sqrt(mag2);
            ex = ex / mag * travel;
            ey = ey / mag * travel;
        }
        rt.knob_dx = ex;
        rt.knob_dy = ey;

        // Normalize + deadzone. Y is NOT flipped here: positive screen-Y
        // means "pull stick downward" = pan camera south. The app /
        // preset decides what that means for the world.
        f32 nx = ex / travel;
        f32 ny = ey / travel;
        f32 mag = std::sqrt(nx * nx + ny * ny);
        if (mag < cfg.style.deadzone_frac) {
            rt.out_x = rt.out_y = 0.0f;
        } else {
            // Rescale so output crosses 0 at the deadzone edge instead
            // of snapping. Keeps fine control near center.
            f32 scale_out = (mag - cfg.style.deadzone_frac)
                          / (1.0f - cfg.style.deadzone_frac) / mag;
            rt.out_x = nx * scale_out;
            rt.out_y = ny * scale_out;
        }
        return;
    }

    // No capture yet. Keep the base pinned at home while idle. Scan for
    // a fresh press inside the activation region; on capture, re-anchor
    // the base to the press point.
    rt.base_cx = home_cx;
    rt.base_cy = home_cy;

    auto try_capture = [&](i32 slot, f32 fx, f32 fy) -> bool {
        if (!joystick_hit_test_point(cfg, fx, fy)) return false;
        rt.captured_slot = slot;
        rt.base_cx = fx;
        rt.base_cy = fy;
        rt.knob_dx = rt.knob_dy = 0.0f;
        return true;
    };

    if (input.touch_count > 0) {
        for (u32 t = 0; t < input.touch_count; ++t) {
            if (try_capture(static_cast<i32>(t),
                            map_x(input.touch_x[t]),
                            map_y(input.touch_y[t]))) {
                break;
            }
        }
    } else if (input.mouse_left_pressed) {
        try_capture(0, map_x(input.mouse_x), map_y(input.mouse_y));
    }
}

// True if (px, py) in dp lies inside the action_bar's cancel zone.
// Margin tolerates finger drift on the boundary so the Aiming ↔
// Cancelling transition doesn't flicker pixel-by-pixel.
static bool action_bar_cancel_zone_contains(const Hud::Impl& s,
                                            f32 px, f32 py, f32 margin) {
    const auto& r = s.action_bar_cfg.cancel_zone_rect;
    if (r.w <= 0.0f || r.h <= 0.0f) return false;
    return px >= r.x - margin && px < r.x + r.w + margin
        && py >= r.y - margin && py < r.y + r.h + margin;
}

void Hud::action_bar_drag_update(const platform::InputState& input) {
    if (!m_impl) return;
    auto& s = *m_impl;
    if (!s.is_mobile) return;
    if (s.drag_cast.phase == Impl::DragCastPhase::Idle) return;
    if (!s.world_ctx || !s.world_ctx->camera || !s.world_ctx->world) return;

    using Phase = Impl::DragCastPhase;

    // Refresh caster world position each frame — if the unit moved
    // mid-drag we want the range ring + drag arrow origin to follow
    // along, not stay anchored where the press happened. Bail (cancel
    // the gesture) if the caster handle no longer validates (died,
    // recycled, etc.).
    if (!s.world_ctx->world->validate(s.drag_cast.caster)) {
        if (s.drag_cast.slot_index >= 0 &&
            static_cast<u32>(s.drag_cast.slot_index) < s.action_bar_cfg.slots.size()) {
            s.action_bar_cfg.slots[s.drag_cast.slot_index].pressed = false;
        }
        s.drag_cast = Impl::DragCastState{};
        return;
    }
    if (auto* tf = s.world_ctx->world->transforms.get(s.drag_cast.caster.id)) {
        s.drag_cast.caster_x = tf->position.x;
        s.drag_cast.caster_y = tf->position.y;
        s.drag_cast.caster_z = tf->position.z;
    }

    // Pointer in dp (same space as handle_pointer's coords).
    f32 inv = (s.ui_scale > 0.0f) ? (1.0f / s.ui_scale) : 1.0f;
    f32 px = input.mouse_x * inv;
    f32 py = input.mouse_y * inv;
    s.drag_cast.current_x = px;
    s.drag_cast.current_y = py;

    // Recompute drag-point world position from finger displacement,
    // projected onto the camera's ground-plane axes. Sensitivity is
    // fixed for v1 (~6 world units per dp); enough thumb travel to
    // reach the edge of a 600-range cast in roughly one comfortable
    // arc. Tunable later via hud.json or settings.
    constexpr f32 SENS = 6.0f;
    f32 yaw = s.world_ctx->camera->yaw();
    f32 cyaw = std::cos(yaw), syaw = std::sin(yaw);
    glm::vec3 right{cyaw, syaw, 0.0f};
    glm::vec3 forward{-syaw, cyaw, 0.0f};
    f32 ddx = px - s.drag_cast.press_x;
    f32 ddy = py - s.drag_cast.press_y;
    glm::vec3 caster{s.drag_cast.caster_x, s.drag_cast.caster_y, s.drag_cast.caster_z};
    glm::vec3 drag = caster + right * (ddx * SENS) - forward * (ddy * SENS);
    if (s.world_ctx->terrain) {
        drag.z = map::sample_height(*s.world_ctx->terrain, drag.x, drag.y);
    }
    s.drag_cast.drag_world_x = drag.x;
    s.drag_cast.drag_world_y = drag.y;
    s.drag_cast.drag_world_z = drag.z;

    // Snap (target_unit only). Pick the nearest valid candidate within
    // a snap radius of the drag point; static target_filter eval, no
    // network round-trip. Snap radius scales with cast range so close-
    // range and long-range abilities both feel similar.
    s.drag_cast.snapped_target = simulation::Unit{};
    if (s.drag_cast.form == simulation::AbilityForm::TargetUnit &&
        s.world_ctx->simulation) {
        const auto& world = *s.world_ctx->world;
        const auto* def = s.world_ctx->abilities
                            ? s.world_ctx->abilities->get(s.drag_cast.ability_id)
                            : nullptr;
        if (def) {
            simulation::Unit caster_unit{};
            if (s.world_ctx->selection &&
                !s.world_ctx->selection->selected().empty()) {
                caster_unit = s.world_ctx->selection->selected().front();
            }
            f32 snap_r = std::max(64.0f, s.drag_cast.range * 0.15f);
            f32 best_d2 = snap_r * snap_r;
            simulation::Unit best{};
            for (u32 i = 0; i < world.transforms.count(); ++i) {
                u32 id = world.transforms.ids()[i];
                const auto* hinfo = world.handle_infos.get(id);
                if (!hinfo || hinfo->category != simulation::Category::Unit) continue;
                simulation::Unit cand{};
                cand.id = id;
                cand.generation = hinfo->generation;
                if (cand.id == caster_unit.id && def->target_filter.self_ == false) {
                    // self-cast only allowed if filter says so
                    continue;
                }
                if (!s.world_ctx->simulation->target_filter_passes(
                        def->target_filter, caster_unit, cand)) {
                    continue;
                }
                const auto* tf = world.transforms.get(id);
                if (!tf) continue;
                f32 dx2 = tf->position.x - drag.x;
                f32 dy2 = tf->position.y - drag.y;
                f32 d2  = dx2 * dx2 + dy2 * dy2;
                if (d2 < best_d2) { best_d2 = d2; best = cand; }
            }
            s.drag_cast.snapped_target = best;
        }
    }

    // Phase transitions:
    //   - Press → Aiming when finger leaves the originating slot rect.
    //     Tap-without-drag (release while still on the slot) is treated
    //     as cancel by the release handler below.
    //   - Aiming ↔ Cancelling driven by the AoV-style cancel zone, NOT
    //     the slot itself. Dragging back over the slot used to trigger
    //     cancel; that was awkward (the finger naturally returns near
    //     the slot during fine-aiming) so we moved it to a dedicated
    //     screen rect that the player explicitly drags into.
    constexpr f32 CANCEL_MARGIN     = 16.0f;
    constexpr f32 SLOT_LEAVE_MARGIN = 8.0f;
    const auto& slot_rect = (s.drag_cast.slot_index >= 0 &&
                             static_cast<u32>(s.drag_cast.slot_index) < s.action_bar_cfg.slots.size())
                              ? s.action_bar_cfg.slots[s.drag_cast.slot_index].rect
                              : Rect{};
    bool over_slot   = (slot_rect.w > 0.0f && slot_rect.h > 0.0f) &&
                       (px >= slot_rect.x - SLOT_LEAVE_MARGIN &&
                        px <  slot_rect.x + slot_rect.w + SLOT_LEAVE_MARGIN &&
                        py >= slot_rect.y - SLOT_LEAVE_MARGIN &&
                        py <  slot_rect.y + slot_rect.h + SLOT_LEAVE_MARGIN);
    bool over_cancel = action_bar_cancel_zone_contains(s, px, py, CANCEL_MARGIN);
    bool button_down = input.mouse_left;

    if (button_down) {
        if (s.drag_cast.phase == Phase::Pressed) {
            // Leaving the slot rect commits to Aiming. Once Aiming, we
            // never go back to Pressed; instead Cancelling is the only
            // way to "abort" the gesture without firing.
            if (!over_slot) s.drag_cast.phase = Phase::Aiming;
        } else if (s.drag_cast.phase == Phase::Aiming) {
            if (over_cancel) s.drag_cast.phase = Phase::Cancelling;
        } else if (s.drag_cast.phase == Phase::Cancelling) {
            if (!over_cancel) s.drag_cast.phase = Phase::Aiming;
        }
        return;
    }

    // Release: decide commit vs. cancel based on phase.
    bool commit = (s.drag_cast.phase == Phase::Aiming);
    if (commit && s.drag_cast.form == simulation::AbilityForm::TargetUnit &&
        !s.drag_cast.snapped_target.is_valid()) {
        // Unit-targeted but the player released without snapping to
        // anything — treat as cancel (Cast order has no target).
        commit = false;
    }
    if (commit && s.action_bar_cast_at_target_fn) {
        u32 target_uid = s.drag_cast.snapped_target.is_valid()
                           ? s.drag_cast.snapped_target.id
                           : UINT32_MAX;
        s.action_bar_cast_at_target_fn(s.drag_cast.ability_id, target_uid,
                                       s.drag_cast.drag_world_x,
                                       s.drag_cast.drag_world_y,
                                       s.drag_cast.drag_world_z);
    }

    // Reset visual + state regardless of commit/cancel.
    if (s.drag_cast.slot_index >= 0 &&
        static_cast<u32>(s.drag_cast.slot_index) < s.action_bar_cfg.slots.size()) {
        s.action_bar_cfg.slots[s.drag_cast.slot_index].pressed = false;
    }
    s.drag_cast = Impl::DragCastState{};
}

Hud::AbilityAimState Hud::aim_state() const {
    AbilityAimState out{};
    if (!m_impl) return out;
    const auto& dc = m_impl->drag_cast;

    // Mobile drag-cast path — feeds aim state directly from the gesture.
    if (dc.phase != Impl::DragCastPhase::Idle) {
        out.active        = true;
        out.is_drag_cast  = true;
        out.caster_x   = dc.caster_x;
        out.caster_y   = dc.caster_y;
        out.caster_z   = dc.caster_z;
        out.drag_x     = dc.drag_world_x;
        out.drag_y     = dc.drag_world_y;
        out.drag_z     = dc.drag_world_z;
        out.range      = dc.range;
        out.is_unit_target = (dc.form == simulation::AbilityForm::TargetUnit);

        // Shape mirrors the ability's indicator shape. For target_unit
        // forms, an area_radius > 0 still draws a circle around the
        // snapped unit even though def->shape is Point.
        switch (dc.shape) {
            case simulation::IndicatorShape::Area: out.area_shape = AimAreaShape::Circle; break;
            case simulation::IndicatorShape::Line: out.area_shape = AimAreaShape::Line;   break;
            case simulation::IndicatorShape::Cone: out.area_shape = AimAreaShape::Cone;   break;
            default:                                out.area_shape = AimAreaShape::None;   break;
        }
        if (out.is_unit_target && dc.area_radius > 0.0f) {
            out.area_shape = AimAreaShape::Circle;
        }
        out.area_radius = dc.area_radius;
        out.area_width  = dc.area_width;
        out.area_angle  = dc.area_angle;
        out.has_area    = (out.area_shape != AimAreaShape::None);

        if (dc.snapped_target.is_valid() && m_impl->world_ctx &&
            m_impl->world_ctx->world) {
            const auto* tf = m_impl->world_ctx->world->transforms.get(
                                 dc.snapped_target.id);
            if (tf) {
                out.snapped_id = dc.snapped_target.id;
                out.snapped_x  = tf->position.x;
                out.snapped_y  = tf->position.y;
                out.snapped_z  = tf->position.z;
                const auto* sel = m_impl->world_ctx->world->selectables.get(
                                      dc.snapped_target.id);
                out.snapped_radius = sel ? sel->selection_radius : 48.0f;
            }
        }

        // Distance from caster to the *anchor the cast will resolve at*.
        f32 anchor_x = out.drag_x;
        f32 anchor_y = out.drag_y;
        if (out.is_unit_target && out.snapped_id != 0xFFFFFFFFu) {
            anchor_x = out.snapped_x;
            anchor_y = out.snapped_y;
        }
        f32 dx = anchor_x - out.caster_x;
        f32 dy = anchor_y - out.caster_y;
        out.distance = std::sqrt(dx * dx + dy * dy);

        if (dc.phase == Impl::DragCastPhase::Cancelling) {
            out.phase = AimPhase::Cancelling;
        } else if (out.range > 0 && out.distance > out.range) {
            out.phase = AimPhase::OutOfRange;
        } else {
            out.phase = AimPhase::Normal;
        }
        return out;
    }

    // Desktop targeting-mode path — preset has armed an ability and is
    // waiting on a world click. Indicator follows the mouse-ground-pick
    // and snaps to a unit (for target_unit forms) using the same
    // target_filter the mobile drag-cast snap consults.
    if (m_impl->action_bar_targeting_ability.empty()) return out;
    if (!m_impl->world_ctx) return out;
    const auto& ctx = *m_impl->world_ctx;
    if (!ctx.world || !ctx.abilities || !ctx.selection || !ctx.picker) return out;

    const auto* def = ctx.abilities->get(m_impl->action_bar_targeting_ability);
    if (!def) return out;
    bool is_unit  = (def->form == simulation::AbilityForm::TargetUnit);
    bool is_point = (def->form == simulation::AbilityForm::TargetPoint);
    if (!is_unit && !is_point) return out;

    if (ctx.selection->selected().empty()) return out;
    simulation::Unit caster_unit = ctx.selection->selected().front();
    const auto* caster_tf = ctx.world->transforms.get(caster_unit.id);
    if (!caster_tf) return out;

    out.active   = true;
    out.caster_x = caster_tf->position.x;
    out.caster_y = caster_tf->position.y;
    out.caster_z = caster_tf->position.z;
    out.is_unit_target = is_unit;

    // Find the ability instance to get its current level (for the
    // level-data range / area_radius). Falls back to level 1 data if
    // the unit doesn't actually own the ability — the preset's
    // targeting-mode logic accepts any armed id, so be defensive.
    u32 level = 1;
    if (const auto* aset = ctx.world->ability_sets.get(caster_unit.id)) {
        for (const auto& a : aset->abilities) {
            if (a.ability_id == m_impl->action_bar_targeting_ability) {
                level = a.level; break;
            }
        }
    }
    const auto& lvl = def->level_data(level);
    out.range = lvl.range;
    switch (def->shape) {
        case simulation::IndicatorShape::Area: out.area_shape = AimAreaShape::Circle; break;
        case simulation::IndicatorShape::Line: out.area_shape = AimAreaShape::Line;   break;
        case simulation::IndicatorShape::Cone: out.area_shape = AimAreaShape::Cone;   break;
        default:                                out.area_shape = AimAreaShape::None;   break;
    }
    if (is_unit && lvl.area.radius > 0.0f) {
        out.area_shape = AimAreaShape::Circle;
    }
    out.area_radius = lvl.area.radius;
    out.area_width  = lvl.area.width;
    out.area_angle  = lvl.area.angle;
    out.has_area    = (out.area_shape != AimAreaShape::None);

    // Pointer is stored in dp; Picker expects physical px.
    f32 s = (m_impl->ui_scale > 0.0f) ? m_impl->ui_scale : 1.0f;
    f32 mx = m_impl->pointer_x * s;
    f32 my = m_impl->pointer_y * s;

    glm::vec3 ground{};
    if (ctx.picker->screen_to_world(mx, my, ground)) {
        out.drag_x = ground.x;
        out.drag_y = ground.y;
        out.drag_z = ground.z;
    } else {
        // Off-terrain pointer (above horizon, sky, etc.) — keep the
        // indicator at the caster so it doesn't drift into nonsense.
        out.drag_x = out.caster_x;
        out.drag_y = out.caster_y;
        out.drag_z = out.caster_z;
    }

    // Unit snap for target_unit — magnetic, mirroring the mobile drag
    // logic so both platforms feel identical. The drag point itself
    // stays at the cursor's ground projection (NO repositioning); we
    // just light up the snapped unit's ring + suppress the reticle
    // when a valid candidate is within the snap radius. Snap radius
    // scales with cast range so close- and long-range abilities feel
    // proportionate.
    if (is_unit && ctx.simulation) {
        f32 snap_r = std::max(64.0f, out.range * 0.15f);
        f32 best_d2 = snap_r * snap_r;
        simulation::Unit best{};
        for (u32 i = 0; i < ctx.world->transforms.count(); ++i) {
            u32 id = ctx.world->transforms.ids()[i];
            const auto* hi = ctx.world->handle_infos.get(id);
            if (!hi || hi->category != simulation::Category::Unit) continue;
            simulation::Unit cand{ id, hi->generation };
            if (!ctx.simulation->target_filter_passes(def->target_filter,
                                                      caster_unit, cand)) {
                continue;
            }
            const auto* tf = ctx.world->transforms.get(id);
            if (!tf) continue;
            f32 dx2 = tf->position.x - out.drag_x;
            f32 dy2 = tf->position.y - out.drag_y;
            f32 d2  = dx2 * dx2 + dy2 * dy2;
            if (d2 < best_d2) { best_d2 = d2; best = cand; }
        }
        if (best.is_valid()) {
            const auto* tf = ctx.world->transforms.get(best.id);
            if (tf) {
                out.snapped_id = best.id;
                out.snapped_x  = tf->position.x;
                out.snapped_y  = tf->position.y;
                out.snapped_z  = tf->position.z;
                const auto* sel = ctx.world->selectables.get(best.id);
                out.snapped_radius = sel ? sel->selection_radius : 48.0f;
            }
        }
    }

    // Distance + phase resolution — same logic as the mobile branch.
    f32 anchor_x = out.drag_x;
    f32 anchor_y = out.drag_y;
    if (out.is_unit_target && out.snapped_id != 0xFFFFFFFFu) {
        anchor_x = out.snapped_x;
        anchor_y = out.snapped_y;
    }
    f32 ddx = anchor_x - out.caster_x;
    f32 ddy = anchor_y - out.caster_y;
    out.distance = std::sqrt(ddx * ddx + ddy * ddy);
    if (out.range > 0 && out.distance > out.range) {
        out.phase = AimPhase::OutOfRange;
    } else {
        out.phase = AimPhase::Normal;
    }
    // No Cancelling on desktop — pressing Esc / right-click clears the
    // armed ability via the preset path; the HUD just stops seeing
    // `action_bar_targeting_ability`.
    return out;
}

// Scale the alpha channel of a packed RGBA color by `frac` in [0, 1].
// Used to dim the joystick base + knob while idle — keeps hue/lightness
// authored by the map and only bleeds the opacity.
static Color scale_color_alpha(Color c, f32 frac) {
    if (frac >= 1.0f) return c;
    if (frac <= 0.0f) frac = 0.0f;
    u32 a = (c.rgba >> 24) & 0xFFu;
    u32 a2 = static_cast<u32>(static_cast<f32>(a) * frac + 0.5f);
    if (a2 > 255u) a2 = 255u;
    return Color{ (c.rgba & 0x00FFFFFFu) | (a2 << 24) };
}

static void draw_joystick(Hud&, Hud::Impl& s) {
    const auto& cfg = s.joystick_cfg;
    const auto& rt  = s.joystick_rt;
    if (!cfg.enabled || !rt.visible) return;

    // Base center follows rt.base_cx/cy — equals home while idle, jumps
    // to the press point while captured.
    f32 cx = rt.base_cx;
    f32 cy = rt.base_cy;
    f32 base_r = std::min(cfg.rect.w, cfg.rect.h) * 0.5f;
    f32 knob_r = base_r * cfg.style.knob_size_frac * 0.5f;

    bool active = rt.captured_slot >= 0;
    f32 alpha_frac = active ? 1.0f : cfg.style.idle_alpha_frac;

    draw_filled_circle(s, cx, cy, base_r,
                       scale_color_alpha(cfg.style.base_color, alpha_frac));
    if (cfg.style.base_border_width > 0.0f) {
        draw_ring(s, cx, cy, base_r, base_r - cfg.style.base_border_width,
                  scale_color_alpha(cfg.style.base_border, alpha_frac));
    }

    f32 kx = cx + rt.knob_dx;
    f32 ky = cy + rt.knob_dy;
    draw_filled_circle(s, kx, ky, knob_r,
                       scale_color_alpha(cfg.style.knob_color, alpha_frac));
    if (cfg.style.knob_border_width > 0.0f) {
        draw_ring(s, kx, ky, knob_r, knob_r - cfg.style.knob_border_width,
                  scale_color_alpha(cfg.style.knob_border, alpha_frac));
    }
}

void Hud::handle_hotkeys(const platform::InputState& input) {
    if (!m_impl) return;
    auto& s = *m_impl;

    // Priority walk. A key letter fires at most one source per frame,
    // even if it appears in multiple places. Order: command_bar ↓
    // action_bar (declaration order) ↓ hidden abilities. Non-rising
    // edges still update each slot's prev-down so a held key doesn't
    // "re-fire" when it's claimed by a later source on a later frame.
    std::unordered_set<std::string> claimed;

    // 1. Command bar.
    {
        auto& cfg = s.command_bar_cfg;
        if (cfg.enabled && s.command_bar_rt.visible && s.command_bar_fn) {
            for (auto& slot : cfg.slots) {
                if (!slot.visible || slot.hotkey.empty() || slot.command.empty()) continue;
                bool down   = input::InputBindings::resolve_key(slot.hotkey, input);
                bool rising = down && !slot.hotkey_prev_down;
                slot.hotkey_prev_down = down;
                if (!rising) continue;
                if (claimed.count(slot.hotkey)) continue;
                claimed.insert(slot.hotkey);
                s.command_bar_fn(slot.command);
            }
        }
    }

    // 2. Action bar. Slots iterate in declaration order; lower index
    // wins on conflict, per the authoring contract. While we're here,
    // collect the set of ability ids that *did* resolve to a slot —
    // stage 3 uses it to decide which unit abilities are "not on any
    // slot" and therefore free to dispatch via their def->hotkey.
    std::unordered_set<std::string> slotted_abilities;
    {
        auto& cfg = s.action_bar_cfg;
        if (cfg.enabled && s.action_bar_rt.visible && s.world_ctx && s.action_bar_cast_fn) {
            for (u32 i = 0; i < cfg.slots.size(); ++i) {
                auto& slot = cfg.slots[i];

                const simulation::AbilityDef* def = nullptr;
                const simulation::AbilityInstance* inst =
                    resolve_slot_ability(i, cfg, *s.world_ctx, def);
                if (inst) slotted_abilities.insert(inst->ability_id);

                if (!slot.visible) continue;
                // Which key triggers this slot depends on the keymap
                // mode: Positional → slot.hotkey (Q/W/E/R from layout);
                // Ability → def->hotkey (the letter authored on the
                // ability itself). Both modes resolve the *same* slot
                // to the *same* ability — only the trigger key differs.
                const std::string* trigger_key = nullptr;
                if (s.action_bar_rt.hotkey_mode == ActionBarHotkeyMode::Ability) {
                    if (!def || def->hotkey.empty()) continue;
                    trigger_key = &def->hotkey;
                } else {
                    if (slot.hotkey.empty()) continue;
                    trigger_key = &slot.hotkey;
                }

                bool down   = input::InputBindings::resolve_key(*trigger_key, input);
                bool rising = down && !slot.hotkey_prev_down;
                slot.hotkey_prev_down = down;
                if (!rising) continue;
                if (claimed.count(*trigger_key)) continue;
                if (!inst || !def) continue;

                u32 unit_id = s.world_ctx->selection
                                ? s.world_ctx->selection->selected().front().id
                                : UINT32_MAX;
                if (unit_id == UINT32_MAX) continue;
                if (!slot_castable_now(*s.world_ctx, unit_id, *inst, *def)) continue;

                claimed.insert(*trigger_key);
                s.action_bar_cast_fn(inst->ability_id);
            }
        }
    }

    // 3. Hidden abilities on the selected unit. "Hidden" here means
    // either explicitly `hidden: true` in the type def OR simply not
    // resolved into any action_bar slot this frame (e.g. slot count
    // too small in positional mode, no slot matches its letter in
    // ability mode, or Lua didn't bind it in manual mode). Both cases
    // fall out naturally from the slotted_abilities set above — any
    // non-slotted ability is fair game to dispatch via its own hotkey.
    if (s.world_ctx && s.world_ctx->world && s.world_ctx->abilities
        && s.world_ctx->selection && s.action_bar_cast_fn) {
        const auto& sel = s.world_ctx->selection->selected();
        if (!sel.empty()) {
            const auto* aset = s.world_ctx->world->ability_sets.get(sel.front().id);
            if (aset) {
                u32 unit_id = sel.front().id;
                for (const auto& inst : aset->abilities) {
                    if (slotted_abilities.count(inst.ability_id)) continue;

                    const auto* def = s.world_ctx->abilities->get(inst.ability_id);
                    if (!def || def->hotkey.empty()) continue;
                    if (!is_castable_form(def->form)) continue;  // passive/aura aren't triggerable

                    bool down   = input::InputBindings::resolve_key(def->hotkey, input);
                    bool& prev  = s.hidden_hotkey_prev[def->hotkey];
                    bool rising = down && !prev;
                    prev = down;
                    if (!rising) continue;
                    if (claimed.count(def->hotkey)) continue;
                    if (!slot_castable_now(*s.world_ctx, unit_id, inst, *def)) continue;

                    claimed.insert(def->hotkey);
                    s.action_bar_cast_fn(inst.ability_id);
                }
            }
        }
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
// Auto mode: slot assignment is ALWAYS positional — the slot's index
// selects the Nth non-hidden ability in the unit's registration order.
// `hotkey_mode` does NOT affect *which* ability fills *which* slot —
// it only governs which keyboard key triggers the slot and which letter
// gets drawn in the badge. (Earlier code path matched by hotkey letter
// here, which meant changing an ability's `hotkey` in JSON re-shuffled
// or hid abilities entirely. That was a bug; slot binding is decoupled
// from the keymap-mode setting.)
//
// Returns nullptr when there's no selection, no ability fills that
// slot, or the registry lookup fails.
static const simulation::AbilityInstance*
resolve_slot_ability(u32 slot_index,
                     const ActionBarConfig& cfg,
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

    // Auto mode (regardless of hotkey_mode): Nth non-hidden ability in
    // registration order. The keymap setting only affects which key
    // triggers each slot and which letter the badge shows — see the
    // hotkey dispatch loop and the slot draw site for those branches.
    // Passives / auras count toward slot positions (they show their
    // icon WC3-command-card style, just don't fire on click); only
    // explicitly hidden abilities are skipped.
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
        || f == F::TargetUnit || f == F::TargetPoint;
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
        if (s.world_ctx) inst = resolve_slot_ability(i, cfg, *s.world_ctx, def);

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
        // Which letter shows follows the keymap setting: Positional →
        // slot.hotkey (Q/W/E/R); Ability → def->hotkey (the ability's
        // own letter). The slot ASSIGNMENT is unaffected by this — see
        // resolve_slot_ability.
        bool show_hotkey = def && is_castable_form(def->form);
        std::string_view badge_key;
        if (show_hotkey) {
            badge_key = (rt.hotkey_mode == ActionBarHotkeyMode::Ability)
                            ? std::string_view{def->hotkey}
                            : std::string_view{slot.hotkey};
        }
        if (!badge_key.empty()) {
            f32 px_size = slot.rect.h * 0.28f;
            if (px_size < 10.0f) px_size = 10.0f;
            f32 text_w = hud.text_width_px(badge_key, px_size);
            f32 x_left = slot.rect.x + slot.rect.w - text_w - 4.0f;
            f32 ascent = hud.text_ascent_px(px_size);
            f32 y_base = slot.rect.y + ascent + 2.0f;

            // Backing pill. ~2px padding all around the glyph; stays
            // inside the slot so it never hangs off the edge.
            if ((cfg.style.hotkey_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f;
                f32 pad_y = 1.0f;
                Rect bg_pill{
                    x_left - pad_x,
                    y_base - ascent - pad_y,
                    text_w + pad_x * 2.0f,
                    ascent + pad_y * 2.0f,
                };
                hud.draw_rect(bg_pill, cfg.style.hotkey_badge_bg);
            }

            hud.draw_text(x_left, y_base, badge_key, cfg.style.hotkey_color, px_size);
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

        // Project to minimap pixels. World +Y is "forward" / north; screen Y
        // grows downward. Flip Y so north lands at the top of the minimap
        // (WC3 convention). Dot is a small centered square.
        f32 sx = cfg.rect.x + (tf.position.x - td.origin_x()) * inv_w;
        f32 sy = cfg.rect.y + cfg.rect.h - (tf.position.y - td.origin_y()) * inv_h;
        f32 half = cfg.style.dot_size * 0.5f;
        Rect dot{ sx - half, sy - half, cfg.style.dot_size, cfg.style.dot_size };
        hud.draw_rect(dot, minimap_dot_color(*s.world_ctx, id, cfg.style));
    }
}

// Render the action bar for the current frame. Called from draw_tree()
// so composites participate in the same frame-build as atom nodes.
// Dispatches to the matching `style_id` render variant — each variant
// has its own fixed layer order and parameter block (see action_bar.h).
// Cancel-zone overlay — drawn only while a drag-cast gesture is
// active. Filled circle background (idle vs. active palette per
// current phase) plus an "✕" glyph centered.
static void draw_action_bar_cancel_zone(Hud& hud, Hud::Impl& s) {
    if (s.drag_cast.phase == Hud::Impl::DragCastPhase::Idle) return;
    const auto& cfg = s.action_bar_cfg;
    const Rect& r = cfg.cancel_zone_rect;
    if (r.w <= 0.0f || r.h <= 0.0f) return;

    bool active = (s.drag_cast.phase == Hud::Impl::DragCastPhase::Cancelling);
    Color bg     = active ? cfg.style.cancel_zone_active_bg     : cfg.style.cancel_zone_idle_bg;
    Color border = active ? cfg.style.cancel_zone_active_border : cfg.style.cancel_zone_idle_border;
    f32 cx = r.x + r.w * 0.5f;
    f32 cy = r.y + r.h * 0.5f;
    f32 radius = std::min(r.w, r.h) * 0.5f;
    f32 border_width = active ? 4.0f : 3.0f;

    draw_filled_circle(s, cx, cy, radius, bg);
    if ((border.rgba >> 24) != 0) {
        draw_ring(s, cx, cy, radius, radius - border_width, border);
    }

    // "✕" glyph centered. Sized so it occupies ~50% of the zone — visible
    // on a 100dp default zone, scales with author-provided sizes too.
    f32 px_size = radius * 1.0f;
    if (px_size < 16.0f) px_size = 16.0f;
    std::string_view glyph = "X";
    f32 text_w = hud.text_width_px(glyph, px_size);
    f32 ascent = hud.text_ascent_px(px_size);
    f32 line_h = hud.text_line_height_px(px_size);
    f32 x_left = cx - text_w * 0.5f;
    f32 y_base = cy + ascent - line_h * 0.5f;
    hud.draw_text(x_left, y_base, glyph, cfg.style.cancel_zone_glyph_color, px_size);
}

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

    // Cancel zone draws on top of everything else in the bar's render
    // pass — finger lands on it = visible target during the drag.
    draw_action_bar_cancel_zone(hud, s);
}

void Hud::draw_tree() {
    if (!m_impl || !m_impl->frame_open || !m_impl->root) return;
    m_impl->root->draw(*this);
    draw_action_bar(*this, *m_impl);
    draw_command_bar(*this, *m_impl);
    draw_minimap(*this, *m_impl);
    draw_joystick(*this, *m_impl);
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
    // Pointer arrives in physical framebuffer pixels. Convert to dp
    // so hit-tests run in the same space composite rects live in.
    f32 inv = (s.ui_scale > 0.0f) ? (1.0f / s.ui_scale) : 1.0f;
    x *= inv;
    y *= inv;
    s.pointer_x = x;
    s.pointer_y = y;

    // Composites sit on top of the node tree (drawn last in draw_tree);
    // hit-test them first so clicks beat anything underneath. Priority
    // order: action_bar > command_bar > minimap > joystick > tree.
    // Joystick is LAST of the composites so explicit UI (buttons, bar
    // slots, minimap dots) inside an otherwise-blank activation region
    // still wins. `on_joystick` also fires when slot 0 (the primary
    // finger) has already captured the stick — that suppresses tree /
    // drag-select from the same finger while dragging the knob.
    i32  bar_slot    = action_bar_hit_test(s, x, y);
    i32  cmd_slot    = (bar_slot < 0) ? command_bar_hit_test(s, x, y) : -1;
    bool on_minimap  = (bar_slot < 0) && (cmd_slot < 0) && minimap_hit_test(s, x, y);
    bool on_joystick = (bar_slot < 0) && (cmd_slot < 0) && !on_minimap
                       && (s.joystick_rt.captured_slot == 0
                           || joystick_hit_test_point(s.joystick_cfg, x, y));

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
    if (cmd_slot != s.command_bar_hover_slot) {
        if (s.command_bar_hover_slot >= 0 &&
            static_cast<u32>(s.command_bar_hover_slot) < s.command_bar_cfg.slots.size()) {
            s.command_bar_cfg.slots[s.command_bar_hover_slot].hovered = false;
        }
        if (cmd_slot >= 0) {
            s.command_bar_cfg.slots[cmd_slot].hovered = true;
        }
        s.command_bar_hover_slot = cmd_slot;
    }

    Node* under = nullptr;
    if (bar_slot < 0 && cmd_slot < 0 && !on_minimap && !on_joystick) {
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
            // On mobile, a press on a *targetable* and *castable-now*
            // slot starts a drag-cast gesture instead of the normal
            // press-and-release click flow. Drag-cast owns the slot
            // until release; the regular pressed_slot path is bypassed
            // so action_bar_cast_fn (which would enter desktop-style
            // targeting mode) doesn't fire here. Desktop falls through
            // to the existing path.
            bool drag_cast_started = false;
            if (s.is_mobile && s.world_ctx && s.action_bar_cast_at_target_fn) {
                const simulation::AbilityDef* def = nullptr;
                const simulation::AbilityInstance* inst =
                    resolve_slot_ability(static_cast<u32>(bar_slot),
                                         s.action_bar_cfg, *s.world_ctx, def);
                bool targetable = def &&
                    (def->form == simulation::AbilityForm::TargetUnit ||
                     def->form == simulation::AbilityForm::TargetPoint);
                u32 caster_id = s.world_ctx->selection &&
                                !s.world_ctx->selection->selected().empty()
                                  ? s.world_ctx->selection->selected().front().id
                                  : UINT32_MAX;
                if (targetable && inst && caster_id != UINT32_MAX &&
                    slot_castable_now(*s.world_ctx, caster_id, *inst, *def)) {
                    auto* tf = s.world_ctx->world->transforms.get(caster_id);
                    auto* hi = s.world_ctx->world->handle_infos.get(caster_id);
                    if (tf && hi) {
                        s.drag_cast.phase       = Hud::Impl::DragCastPhase::Pressed;
                        s.drag_cast.slot_index  = bar_slot;
                        s.drag_cast.press_x     = x;
                        s.drag_cast.press_y     = y;
                        s.drag_cast.current_x   = x;
                        s.drag_cast.current_y   = y;
                        s.drag_cast.caster.id         = caster_id;
                        s.drag_cast.caster.generation = hi->generation;
                        s.drag_cast.caster_x    = tf->position.x;
                        s.drag_cast.caster_y    = tf->position.y;
                        s.drag_cast.caster_z    = tf->position.z;
                        s.drag_cast.drag_world_x = tf->position.x;
                        s.drag_cast.drag_world_y = tf->position.y;
                        s.drag_cast.drag_world_z = tf->position.z;
                        s.drag_cast.ability_id  = inst->ability_id;
                        const auto& lvl = def->level_data(inst->level);
                        s.drag_cast.range       = lvl.range;
                        s.drag_cast.form        = def->form;
                        s.drag_cast.shape       = def->shape;
                        s.drag_cast.area_radius = lvl.area.radius;
                        s.drag_cast.area_width  = lvl.area.width;
                        s.drag_cast.area_angle  = lvl.area.angle;
                        s.drag_cast.snapped_target = simulation::Unit{};
                        s.action_bar_cfg.slots[bar_slot].pressed = true;
                        drag_cast_started = true;
                    }
                }
            }
            if (!drag_cast_started) {
                s.action_bar_pressed_slot = bar_slot;
                s.action_bar_cfg.slots[bar_slot].pressed = true;
            }
        } else if (cmd_slot >= 0) {
            s.command_bar_pressed_slot = cmd_slot;
            s.command_bar_cfg.slots[cmd_slot].pressed = true;
        } else if (on_minimap) {
            // Minimap press — start a drag that pans the camera. Each
            // pointer move while held re-fires minimap_jump_fn (handled
            // below the press/release edges); release clears the latch.
            // Silently ignore if the session isn't fully up yet.
            if (s.world_ctx && s.world_ctx->terrain && s.minimap_jump_fn) {
                s.minimap_dragging = true;
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
                        resolve_slot_ability(idx, s.action_bar_cfg,
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
        } else if (s.command_bar_pressed_slot >= 0) {
            // Click on a command-bar slot → fire the command callback
            // with the slot's command id. App routes to the input
            // preset so the tap dispatches the same order the keyboard
            // binding would (Stop / HoldPosition immediate, Attack /
            // Move entering targeting mode).
            u32 idx = static_cast<u32>(s.command_bar_pressed_slot);
            bool over = (cmd_slot == s.command_bar_pressed_slot);
            if (idx < s.command_bar_cfg.slots.size()) {
                auto& slot = s.command_bar_cfg.slots[idx];
                slot.pressed = false;
                if (over && s.command_bar_fn && !slot.command.empty()) {
                    s.command_bar_fn(slot.command);
                }
            }
            s.command_bar_pressed_slot = -1;
        } else if (s.pressed) {
            bool over = (under == s.pressed);
            std::string clicked_id = s.pressed->id;
            bool clicked = s.pressed->on_release(over);
            s.pressed = nullptr;
            if (clicked && !clicked_id.empty()) fire_button_event(clicked_id);
        }
    }

    // Minimap drag: while the latch is set, every pointer move re-fires
    // the jump callback so the camera follows the finger across the
    // minimap. Latch survives the pointer leaving the minimap rect (the
    // projected world point is just clamped by the terrain bounds), and
    // clears on release. Held-but-not-moved frames re-issue the same
    // world point — cheap, idempotent.
    if (s.minimap_dragging) {
        if (!button_down) {
            s.minimap_dragging = false;
        } else if (s.world_ctx && s.world_ctx->terrain && s.minimap_jump_fn) {
            f32 wx = 0.0f, wy = 0.0f;
            minimap_point_to_world(s.minimap_cfg.rect, *s.world_ctx->terrain,
                                   x, y, wx, wy);
            s.minimap_jump_fn(wx, wy);
        }
    }

    s.pointer_down_prev = button_down;
}

bool Hud::input_captured() const {
    if (!m_impl) return false;
    // Pointer over (or holding) any HUD surface that takes pointer input.
    // Joystick only counts when the primary finger (slot 0) is driving
    // it — if a secondary finger grabbed the stick, the preset's
    // primary-pointer code is still free to run.
    return m_impl->hover != nullptr || m_impl->pressed != nullptr
        || m_impl->action_bar_hover_slot    >= 0
        || m_impl->action_bar_pressed_slot  >= 0
        || m_impl->command_bar_hover_slot   >= 0
        || m_impl->command_bar_pressed_slot >= 0
        || minimap_hit_test(*m_impl, m_impl->pointer_x, m_impl->pointer_y)
        || m_impl->minimap_dragging
        || m_impl->joystick_rt.captured_slot == 0;
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

void Hud::draw_marquee(f32 x0, f32 y0, f32 x1, f32 y1) {
    if (!m_impl || !m_impl->frame_open) return;
    // Normalize so a drag from any corner lays down a well-formed rect.
    f32 xa = std::min(x0, x1), xb = std::max(x0, x1);
    f32 ya = std::min(y0, y1), yb = std::max(y0, y1);
    Rect r{ xa, ya, xb - xa, yb - ya };
    if (r.w <= 0.0f || r.h <= 0.0f) return;

    const auto& style = m_impl->marquee_style;
    if ((style.fill.rgba >> 24) != 0) draw_rect(r, style.fill);

    // 1-pixel border strips on all four sides so the marquee remains
    // readable over bright ground tiles.
    if ((style.border.rgba >> 24) != 0) {
        draw_rect({ r.x, r.y, r.w, 1.0f },               style.border);
        draw_rect({ r.x, r.y + r.h - 1.0f, r.w, 1.0f },  style.border);
        draw_rect({ r.x, r.y, 1.0f, r.h },               style.border);
        draw_rect({ r.x + r.w - 1.0f, r.y, 1.0f, r.h },  style.border);
    }
}

void Hud::set_marquee_style(const MarqueeStyle& style) {
    if (m_impl) m_impl->marquee_style = style;
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

    // Viewport covers the whole physical framebuffer; the ortho below
    // projects reference-pixel geometry across it, so HUD content
    // scales uniformly by `hud_scale` on the GPU.
    VkViewport vp{};
    vp.x        = 0.0f;
    vp.y        = 0.0f;
    vp.width    = static_cast<f32>(s.physical_w);
    vp.height   = static_cast<f32>(s.physical_h);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = { s.physical_w, s.physical_h };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Ortho is sized to the logical HUD dimensions (≥ reference along
    // the limiting axis, with bonus space on the longer axis). Vulkan
    // NDC is +Y down; glm::ortho is OpenGL +Y up, so we pass bottom=0,
    // top=h to flip Y into Vulkan's space.
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
