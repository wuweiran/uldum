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
#include "hud/inventory.h"
#include "hud/layout.h"

#include <nlohmann/json.hpp>

#include "rhi/vulkan/vulkan_rhi.h"
#include "asset/asset.h"
#include "asset/texture.h"
#include "simulation/world.h"
#include "simulation/components.h"
#include "simulation/ability_def.h"
#include "simulation/simulation.h"
#include "simulation/type_registry.h"
#include "simulation/vision.h"
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

#include <algorithm>
#include <array>
#include <chrono>
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

// Forward decl — defined alongside draw_command_bar but referenced
// from inside its render loop. Tells per-slot rendering whether to
// draw command-icon content (vs. leaving the slot frame blank).
static bool command_bar_slots_active(const Hud::Impl& s);

// Disc / ring-arc primitives — defined further down with the other
// circle-drawing helpers, but the round command-bar style needs them
// before that point.
static void draw_disc(Hud::Impl& s, f32 cx, f32 cy, f32 radius, Color color);
static void draw_ring_arc(Hud::Impl& s, f32 cx, f32 cy,
                          f32 r_outer, f32 r_inner,
                          f32 start_angle, f32 sweep, Color color);

// Forward decls for inventory helpers — defined alongside
// `draw_inventory` but used by `Hud::handle_right_click` further
// up in the file.
static i32 inventory_hit_test(const Hud::Impl& s, f32 x, f32 y);
static const simulation::Inventory*
inventory_resolve_selected(const Hud::Impl& s, u32* out_carrier_id = nullptr);
static bool inventory_resolve_slot(const Hud::Impl& s,
                                   const simulation::Inventory* inv,
                                   u32 slot_index,
                                   simulation::Item& out_item,
                                   const simulation::ItemInfo*& out_info,
                                   const simulation::ItemTypeDef*& out_def);

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

    // Lua-instantiated tree registry. One entry per active CreateNode
    // call; mirrors how composites cache (Placement, Rect) in their
    // config struct. On viewport resize we walk this list, look up
    // each tree's root by id, re-resolve its rect against the new
    // viewport, and translate the subtree by the delta — same shape
    // as composite reflow, just data-driven instead of struct-driven
    // because tree contents are arbitrary. Removed on DestroyNode.
    struct InstantiatedTree {
        std::string                  id;
        ::uldum::hud::Placement      placement;   // layout::Placement (AnchorFrac-based)
    };
    std::vector<InstantiatedTree> instantiated_trees;

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
        simulation::AbilityForm form = simulation::AbilityForm::PassiveModifier;
        // Target-form metadata snapshot — bitmask of `widget_kind::*`
        // values the cursor accepts (Unit/Destructable/Item) and whether
        // the cursor falls through to the ground when no widget snaps.
        // Drives reticle / snap logic during drag.
        u32          widget_kinds = 0;
        bool         accept_point = false;
        simulation::IndicatorShape shape = simulation::IndicatorShape::Point;
        f32          area_radius = 0;
        f32          area_width  = 0;   // Line
        f32          area_angle  = 0;   // Cone, degrees
        // Snapped target unit (target_unit form only). Invalid when
        // not snapped.
        simulation::Unit snapped_target{};
        // Command-bar drag (Phase 5a). When non-empty, the drag was
        // started from a command_bar slot — release fires the command
        // commit callback (Move / Attack / AttackMove) instead of the
        // ability cast callback. ability_id stays empty in this case.
        std::string command_id;
        // Inventory drag (Phase 5b mobile). When inventory_slot >= 0,
        // the drag came from an inventory slot. Mutually exclusive
        // with command_id and the regular ability slot path. On
        // release: quick tap (Pressed + still over slot) fires the
        // no-target use callback; drag (Aiming) fires the
        // use-at-target callback. A long press in Pressed phase
        // (no slide-off) lifts the item into held mode and clears
        // this state. inventory_targetable is the press-time snapshot
        // of whether the item's first ability supports drag-cast
        // (form is Target AND castable now).
        i32          inventory_slot = -1;
        u32          inventory_item_id = UINT32_MAX;
        std::string  inventory_item_icon;
        bool         inventory_targetable = false;
        std::chrono::steady_clock::time_point press_time{};
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
    Hud::CommandBarDragCommitFn command_bar_drag_commit_fn;
    std::string command_bar_armed_command;

    // Joystick composite. Captured touch slot + last knob offset live
    // in JoystickRuntime; `joystick_update` re-evaluates them each
    // frame against the current InputState.
    JoystickConfig  joystick_cfg{};
    JoystickRuntime joystick_rt{};

    // Inventory composite. Slot contents are looked up live from the
    // selected unit's `Inventory.slots` each frame; the composite owns
    // layout + click latching only.
    InventoryConfig  inventory_cfg{};
    InventoryRuntime inventory_rt{};
    i32 inventory_hover_slot   = -1;
    i32 inventory_pressed_slot = -1;
    Hud::InventoryUseFn          inventory_use_fn;
    Hud::InventoryUseAtTargetFn  inventory_use_at_target_fn;
    Hud::InventoryDropFn         inventory_drop_fn;
    Hud::InventorySwapFn         inventory_swap_fn;

    // WC3-style item hold (desktop). Right-click on a slot lifts the
    // item: `held_item_slot` is the source slot, `held_item_id` is
    // the item being held (snapshotted so we can render its icon at
    // the cursor even if the underlying inventory shifts), and
    // `held_item_icon` caches the icon path. The next left-click
    // commits — on another slot it's a swap; on terrain it's a drop
    // at the clicked point. Right-click again (or ESC) cancels.
    i32 held_item_slot = -1;
    u32 held_item_id   = UINT32_MAX;
    std::string held_item_icon;

    // Cast indicator style — applied to range ring, arrow, reticle,
    // AoE indicator, target-unit ring, and per-phase tints. Drives
    // AbilityIndicators calls in the app each frame.
    CastIndicatorConfig cast_indicator_cfg{};

    // Hover / long-press tooltip for action_bar, inventory, command_bar.
    // One tooltip at a time — whichever slot the pointer is dwelling on
    // wins. Reset whenever the dwelling target changes; pops up after a
    // platform-dependent delay (400 ms hover on desktop; 500 ms stationary
    // press on mobile, AoV-style).
    struct TooltipState {
        enum class Source : u8 { None, ActionBar, Inventory, CommandBar };
        Source source = Source::None;
        i32    slot_index = -1;
        std::chrono::steady_clock::time_point activate_at{};
        bool   visible = false;
    };
    TooltipState tooltip{};

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

    // Client-side i18n resolver. Set by App. Used at render time to
    // resolve LocalizedString payloads on text tags / labels in the
    // local player's locale. Null = no localization, renders the
    // literal `text` field instead.
    i18n::LocaleManager* locale_manager = nullptr;

    // hud.json's `"preset"` value — drives preset-specific HUD behavior
    // (e.g. focus_target auto-acquire only runs for `"action"`).
    std::string preset;

    // Focus target — Action-preset "who am I aiming abilities at" state.
    // Auto-acquired by `Hud::update_focus` from the local player's hero
    // each frame, or locked by `Hud::set_focus_target`.
    simulation::Unit focus_target_unit{};
    bool             focus_manual = false;

    // Text tag pool. Slot-based with per-slot generation counter for
    // handle validation. Destroyed tags leave alive=false and bump
    // generation; create_text_tag reuses the first dead slot it finds.
    struct TextTagEntry {
        bool                  alive       = false;
        u32                   generation  = 0;
        i18n::LocalizedString text;                       // resolved per-render with active locale
        f32                   px_size     = 14.0f;
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
        u32                players_mask = UINT32_MAX;  // broadcast by default
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

// Destroy every cached HUD image and clear the map. Used on session
// reset so a new map's same-named texture (e.g. two maps both shipping
// `textures/icons/attack.ktx2`) doesn't read the previous map's GPU
// texture out of the cache. Caller must hold the device-idle guarantee
// (we don't vkDeviceWaitIdle here — sites that call this already do).
static void destroy_hud_images(Hud::Impl& s) {
    if (!s.rhi) { s.images.clear(); return; }
    VkDevice device = s.rhi->device();
    for (auto& [path, img] : s.images) {
        if (!img) continue;
        if (img->set)   vkFreeDescriptorSets(device, s.desc_pool, 1, &img->set);
        if (img->view)  vkDestroyImageView(device, img->view, nullptr);
        if (img->image) vmaDestroyImage(s.rhi->allocator(), img->image, img->alloc);
    }
    s.images.clear();
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
    // Build the platform's system-font chain: primary (Latin / Cyrillic
    // / Greek / Arabic) + CJK fallback + emoji fallback. The engine ships
    // no fonts of its own — text quality matches what the OS provides.
    // Game projects can layer their own font on top via load_fallback
    // later (deferred).
    m_impl->font->init_from_system(rhi, m_impl->desc_layout, m_impl->sampler);

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

    // Inventory — same structure as action_bar / command_bar.
    auto& iv = m_impl->inventory_cfg;
    if (iv.enabled) {
        iv.rect = resolve(viewport, iv.placement);
        for (auto& slot : iv.slots) {
            slot.rect = resolve(iv.rect, slot.placement);
        }
    }

    // Lua-instantiated trees: same shape as composite reflow above,
    // just driven by the registry instead of a typed config struct.
    // For each entry, look up the root by id, re-resolve its rect
    // against the new viewport, and translate the whole subtree by
    // the delta. Children inside the subtree have absolute rects
    // that were resolved against the parent at instantiate time, so
    // a uniform translation keeps every label / bar / image in its
    // correct relative slot. SetLabelText / SetBarFill / etc.
    // mutations on those children survive — we don't rebuild.
    for (const auto& tree : m_impl->instantiated_trees) {
        Node* root = find_node_by_id(tree.id);
        if (!root) continue;
        Rect new_rect = resolve(viewport, tree.placement);
        f32 dx = new_rect.x - root->rect.x;
        f32 dy = new_rect.y - root->rect.y;
        if (dx == 0.0f && dy == 0.0f) continue;
        std::vector<Node*> stack{ root };
        while (!stack.empty()) {
            Node* n = stack.back();
            stack.pop_back();
            n->rect.x += dx;
            n->rect.y += dy;
            for (const auto& c : n->children()) {
                if (c) stack.push_back(c.get());
            }
        }
    }
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

// Per-vertex UV variant for textured fans (e.g. circle-clipped icons).
static void append_triangle_uv(Hud::Impl& s,
                               f32 x0, f32 y0, f32 u0, f32 v0,
                               f32 x1, f32 y1, f32 u1, f32 v1,
                               f32 x2, f32 y2, f32 u2, f32 v2,
                               u32 premul) {
    if (s.verts.size() + 3 > MAX_VERTS) return;
    u16 base = static_cast<u16>(s.verts.size());
    s.verts.push_back({ { x0, y0 }, premul, { u0, v0 } });
    s.verts.push_back({ { x1, y1 }, premul, { u1, v1 } });
    s.verts.push_back({ { x2, y2 }, premul, { u2, v2 } });
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

void Hud::reset_scene_state() {
    if (!m_impl) return;
    auto& s = *m_impl;

    // Lua-created node children (per-scene). hud.json composites live
    // outside the root tree, so they survive.
    if (s.root) s.root->clear_children();
    s.hover   = nullptr;
    s.pressed = nullptr;

    // Floating text tags spawned by the previous scene's main / triggers.
    s.text_tags.clear();

    // Transient input state — handles in flight refer to dead unit ids.
    s.drag_cast = Impl::DragCastState{};
    s.action_bar_hover_slot      = -1;
    s.action_bar_pressed_slot    = -1;
    s.action_bar_targeting_ability.clear();
    s.command_bar_hover_slot     = -1;
    s.command_bar_pressed_slot   = -1;
    s.command_bar_armed_command.clear();
    s.minimap_dragging           = false;
    s.inventory_hover_slot       = -1;
    s.inventory_pressed_slot     = -1;
    s.focus_target_unit          = simulation::Unit{};
    s.focus_manual               = false;
    s.tooltip                    = Impl::TooltipState{};

    // Edge-tracking for ability hotkeys — stale entries would mis-fire
    // on the new scene's first frame.
    s.hidden_hotkey_prev.clear();

    // Lua-created tree instances are tied to the just-cleared node tree.
    s.instantiated_trees.clear();
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
    s.inventory_hover_slot       = -1;
    s.inventory_pressed_slot     = -1;
    s.focus_target_unit          = simulation::Unit{};
    s.focus_manual               = false;
    s.tooltip                    = Impl::TooltipState{};

    // Drop the cross-map icon / texture cache so a same-named texture
    // in the next map (e.g. two maps both shipping
    // `textures/icons/attack.ktx2`) doesn't pick up the previous map's
    // GPU image out of the cache. We also need to free the underlying
    // Vulkan resources, so wait for the device to be idle first.
    if (s.rhi) {
        vkDeviceWaitIdle(s.rhi->device());
        destroy_hud_images(s);
    }

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
    s.inventory_cfg      = {};
    s.inventory_rt       = {};
    s.cast_indicator_cfg = {};
    s.world_cfg          = {};
    s.marquee_style      = {};

    // Node templates from previous map's `nodes` block.
    s.node_templates.clear();
    // Instantiated-tree registry — entries are tied to the just-cleared
    // node tree, so they'd dangle into the next session otherwise.
    s.instantiated_trees.clear();

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
    s.inventory_use_fn           = {};
    s.inventory_use_at_target_fn = {};
    s.inventory_drop_fn          = {};
    s.inventory_swap_fn          = {};
    s.held_item_slot    = -1;
    s.held_item_id      = UINT32_MAX;
    s.held_item_icon.clear();
}

void Hud::set_local_player(u32 player_id) {
    if (m_impl) m_impl->local_player = player_id;
}
void Hud::set_preset_name(std::string_view name) {
    if (m_impl) m_impl->preset.assign(name);
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
    // Capture the target mask before the node is freed so we can route
    // the sync to the right peers; otherwise post-remove we'd have no
    // way to tell.
    u32 mask = UINT32_MAX;
    if (auto* n = find_node_by_id(id)) mask = n->players_mask;
    // Clear transient hover / pressed references before we drop the node,
    // else we'd chase a freed pointer next input frame.
    if (m_impl->hover && m_impl->hover->id == id)   m_impl->hover = nullptr;
    if (m_impl->pressed && m_impl->pressed->id == id) m_impl->pressed = nullptr;
    bool ok = remove_node_recursive(m_impl->root.get(), id);
    if (ok) {
        // Drop the matching registry entry so the resize path stops
        // looking for a node that's been freed.
        auto& reg = m_impl->instantiated_trees;
        reg.erase(std::remove_if(reg.begin(), reg.end(),
                                 [&](const auto& t) { return t.id == id; }),
                  reg.end());
        emit_sync(*m_impl, uldum::network::build_hud_destroy_node(id), mask);
    }
    return ok;
}

void Hud::register_instantiated_tree(std::string id, std::string_view anchor,
                                     f32 x, f32 y, f32 w, f32 h) {
    if (!m_impl || id.empty()) return;
    ::uldum::hud::Placement p{ parse_anchor(anchor), x, y, w, h };
    m_impl->instantiated_trees.push_back({ std::move(id), p });
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
    tp.players_mask = placement.players_mask;
    bool ok = uldum::hud::instantiate_template(*this, id, ex.width, ex.height, tp);
    if (ok) {
        emit_sync(*m_impl,
                  uldum::network::build_hud_create_node(id, placement.anchor,
                                                         placement.x, placement.y,
                                                         placement.w, placement.h),
                  placement.players_mask);
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
        if (!(t.players_mask & (1u << s.local_player))) continue;

        // Resolve the localized payload against the client's active
        // locale. No resolver wired → render the key literal as a
        // visible fallback.
        std::string rendered = s.locale_manager
            ? s.locale_manager->resolve(i18n::Pool::Map, t.text)
            : t.text.key;
        if (rendered.empty()) continue;

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
        f32 text_w    = hud.text_width_px(rendered, t.px_size);
        f32 line_h    = hud.text_line_height_px(t.px_size);
        f32 ascent    = hud.text_ascent_px(t.px_size);
        f32 x_left    = cx - text_w * 0.5f;
        f32 y_baseline = cy + ascent - line_h * 0.5f;
        hud.draw_text(x_left, y_baseline, rendered, final_color, t.px_size);
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

void Hud::set_locale_manager(i18n::LocaleManager* mgr) {
    if (!m_impl) return;
    m_impl->locale_manager = mgr;
}

i18n::LocaleManager* Hud::locale_manager() const {
    return m_impl ? m_impl->locale_manager : nullptr;
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

// ── Focus target ──────────────────────────────────────────────────────────
// Auto-acquired each frame from the local player's hero (or locked by an
// explicit set_focus_target). v1 constants are hard-coded; can be moved
// to hud.json later if maps want per-style tuning.

namespace {
constexpr f32 FOCUS_CONE_HALF_ANGLE   = 1.0472f;   // 60° → 120° cone in front
constexpr f32 FOCUS_AUTO_RANGE        = 800.0f;    // pick within this distance
constexpr f32 FOCUS_LOST_RANGE        = 1200.0f;   // drop if existing focus farther

// Tile-coord visibility check shared with the rest of the HUD's world
// queries. Returns true when fog is disabled / terrain isn't ready, so
// pre-fog or test maps don't accidentally hide everything.
bool focus_visible(const WorldContext& ctx, glm::vec3 pos) {
    if (!ctx.vision || !ctx.terrain || !ctx.terrain->is_valid()) return true;
    f32 ts = ctx.terrain->tile_size;
    if (ts <= 0.0f) return true;
    if (!ctx.vision->enabled()) return true;
    i32 tx = static_cast<i32>((pos.x - ctx.terrain->origin_x()) / ts);
    i32 ty = static_cast<i32>((pos.y - ctx.terrain->origin_y()) / ts);
    if (tx < 0 || ty < 0 ||
        static_cast<u32>(tx) >= ctx.terrain->tiles_x ||
        static_cast<u32>(ty) >= ctx.terrain->tiles_y) return false;
    return ctx.vision->is_visible(ctx.local_player,
                                  static_cast<u32>(tx),
                                  static_cast<u32>(ty));
}
} // namespace

void Hud::update_focus(f32 /*dt*/) {
    if (!m_impl) return;
    auto& s = *m_impl;
    // Focus_target is an Action-preset concept; RTS-style maps select
    // and command via clicks and don't want a reticle following enemies.
    if (s.preset != "action") {
        s.focus_target_unit = simulation::Unit{};
        s.focus_manual = false;
        return;
    }
    if (!s.world_ctx || !s.world_ctx->world || !s.world_ctx->selection) return;

    const auto& world = *s.world_ctx->world;
    const auto& sel   = *s.world_ctx->selection;

    // Hero = the local player's lead selected unit. Action preset locks
    // selection to the hero; for RTS-style multi-select this still picks
    // the first slot, which matches the existing convention.
    if (sel.empty()) {
        s.focus_target_unit = simulation::Unit{};
        s.focus_manual = false;
        return;
    }
    auto hero = sel.selected().front();
    if (!world.validate(hero)) {
        s.focus_target_unit = simulation::Unit{};
        s.focus_manual = false;
        return;
    }
    auto* hero_tf = world.transforms.get(hero.id);
    auto* hero_owner = world.owners.get(hero.id);
    if (!hero_tf || !hero_owner) return;
    glm::vec3 hp = hero_tf->position;

    // Validate the current focus first. Both auto and manual share the
    // alive + visible checks; they differ only on the range condition.
    auto focus_alive_and_visible = [&](simulation::Unit u, glm::vec3& out_pos) -> bool {
        if (!u.is_valid() || !world.validate(u)) return false;
        auto* hp = world.healths.get(u.id);
        if (hp && hp->current <= 0) return false;
        auto* tf = world.transforms.get(u.id);
        if (!tf) return false;
        if (!focus_visible(*s.world_ctx, tf->position)) return false;
        out_pos = tf->position;
        return true;
    };

    if (s.focus_manual) {
        glm::vec3 fp;
        if (!focus_alive_and_visible(s.focus_target_unit, fp)) {
            // Manual lock broken — clear and fall through to auto re-acquire.
            s.focus_target_unit = simulation::Unit{};
            s.focus_manual = false;
        } else {
            return;  // manual still good, no auto eval
        }
    }

    // Cone vectors used by both the retain and re-acquire checks below.
    f32 cosf_half = std::cos(FOCUS_CONE_HALF_ANGLE);
    f32 hero_dx   = std::cos(hero_tf->facing);
    f32 hero_dy   = std::sin(hero_tf->facing);

    // Auto: keep existing focus if alive, visible, within lost range, AND
    // still inside the hero's facing cone. The cone gate is what makes
    // turning the hero re-acquire — without it, focus would stay locked
    // on a target that's now behind you.
    glm::vec3 cur_pos;
    if (focus_alive_and_visible(s.focus_target_unit, cur_pos)) {
        glm::vec3 d = cur_pos - hp;
        f32 d2 = d.x * d.x + d.y * d.y;
        if (d2 <= FOCUS_LOST_RANGE * FOCUS_LOST_RANGE) {
            f32 dlen = std::sqrt(d2);
            if (dlen < 0.001f) return;  // standing on focus — keep
            f32 dot = (d.x * hero_dx + d.y * hero_dy) / dlen;
            if (dot >= cosf_half) {
                return;  // sticky — current auto focus still valid
            }
        }
    }

    // Re-acquire: nearest visible enemy in the hero's facing cone within
    // FOCUS_AUTO_RANGE. Cheap O(N) over alive enemies — N is small for
    // the test maps; if it grows, switch to the spatial grid.
    simulation::Unit best;
    f32 best_d2 = FOCUS_AUTO_RANGE * FOCUS_AUTO_RANGE;

    for (u32 i = 0; i < world.transforms.count(); ++i) {
        u32 id = world.transforms.ids()[i];
        if (id == hero.id) continue;
        auto* h = world.healths.get(id);
        if (!h || h->current <= 0) continue;
        auto* o = world.owners.get(id);
        if (!o) continue;
        // Enemy filter — same alliance check the spatial grid uses.
        if (s.world_ctx->simulation
            ? s.world_ctx->simulation->is_allied(hero_owner->player, o->player)
            : (o->player == hero_owner->player)) {
            continue;
        }
        const auto& etf = world.transforms.data()[i];
        glm::vec3 dv = etf.position - hp;
        f32 d2 = dv.x * dv.x + dv.y * dv.y;
        if (d2 > best_d2) continue;
        f32 dlen = std::sqrt(d2);
        if (dlen < 0.001f) continue;
        // Cone test: dot(forward, normalize(to_target)) >= cos(half_angle)
        f32 dot = (dv.x * hero_dx + dv.y * hero_dy) / dlen;
        if (dot < cosf_half) continue;
        if (!focus_visible(*s.world_ctx, etf.position)) continue;
        simulation::Unit cand{ id, o->player.id };  // owner.id != generation; fix below
        // Resolve generation for a stable handle.
        if (auto* hi = world.handle_infos.get(id)) {
            cand.generation = hi->generation;
        }
        best = cand;
        best_d2 = d2;
    }

    s.focus_target_unit = best;  // invalid handle = "no focus"
}

simulation::Unit Hud::focus_target() const {
    return m_impl ? m_impl->focus_target_unit : simulation::Unit{};
}

bool Hud::focus_is_manual() const {
    return m_impl && m_impl->focus_manual;
}

void Hud::set_focus_target(simulation::Unit unit) {
    if (!m_impl) return;
    m_impl->focus_target_unit = unit;
    m_impl->focus_manual = unit.is_valid();
}

void Hud::clear_focus_target() {
    if (!m_impl) return;
    m_impl->focus_target_unit = simulation::Unit{};
    m_impl->focus_manual = false;
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
    t.players_mask = info.players_mask;

    // MP sync: fire-and-forget creation. Clients run the animation
    // locally from identical params (same lifespan / velocity / fadepoint).
    // No mid-life setters are synced in v1 — setters apply locally only.
    emit_sync(*m_impl,
              uldum::network::build_hud_create_text_tag(
                  info.text.key, info.text.args,
                  info.px_size,
                  info.pos.x, info.pos.y, info.pos.z,
                  info.unit.id, info.z_offset,
                  info.color.rgba,
                  info.velocity_x, info.velocity_y,
                  info.lifespan, info.fadepoint),
              info.players_mask);

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

void Hud::set_text_tag_text(TextTagId id, i18n::LocalizedString text) {
    if (!m_impl) return;
    if (auto* t = lookup_tag(*m_impl, id)) t->text = std::move(text);
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

bool Hud::is_minimap_dragging() const {
    return m_impl && m_impl->minimap_dragging;
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

void Hud::set_command_bar_drag_commit_fn(CommandBarDragCommitFn fn) {
    if (m_impl) m_impl->command_bar_drag_commit_fn = std::move(fn);
}

// ── Inventory composite ──────────────────────────────────────────────────

void Hud::set_inventory_config(const InventoryConfig& cfg) {
    if (!m_impl) return;
    m_impl->inventory_cfg = cfg;
    m_impl->inventory_rt  = InventoryRuntime{};
    m_impl->inventory_hover_slot   = -1;
    m_impl->inventory_pressed_slot = -1;
}

void Hud::inventory_set_visible(bool visible) {
    if (m_impl) m_impl->inventory_rt.visible = visible;
}

void Hud::set_inventory_use_fn(InventoryUseFn fn) {
    if (m_impl) m_impl->inventory_use_fn = std::move(fn);
}

void Hud::set_inventory_use_at_target_fn(InventoryUseAtTargetFn fn) {
    if (m_impl) m_impl->inventory_use_at_target_fn = std::move(fn);
}

void Hud::set_inventory_drop_fn(InventoryDropFn fn) {
    if (m_impl) m_impl->inventory_drop_fn = std::move(fn);
}

void Hud::set_inventory_swap_fn(InventorySwapFn fn) {
    if (m_impl) m_impl->inventory_swap_fn = std::move(fn);
}

bool Hud::handle_right_click(f32 x, f32 y) {
    if (!m_impl) return false;
    auto& s = *m_impl;
    f32 inv_s = (s.ui_scale > 0.0f) ? (1.0f / s.ui_scale) : 1.0f;
    f32 dpx = x * inv_s, dpy = y * inv_s;

    // Already holding → right-click anywhere cancels the hold (WC3 UX).
    if (s.held_item_slot >= 0) {
        s.held_item_slot = -1;
        s.held_item_id   = UINT32_MAX;
        s.held_item_icon.clear();
        return true;
    }

    // Otherwise: right-click on an inventory slot lifts that item.
    i32 slot = inventory_hit_test(s, dpx, dpy);
    if (slot < 0) return false;
    u32 carrier = UINT32_MAX;
    const simulation::Inventory* invc = inventory_resolve_selected(s, &carrier);
    simulation::Item item;
    const simulation::ItemInfo*    info = nullptr;
    const simulation::ItemTypeDef* tdef = nullptr;
    if (!inventory_resolve_slot(s, invc, static_cast<u32>(slot), item, info, tdef)) return false;
    s.held_item_slot = slot;
    s.held_item_id   = item.id;
    s.held_item_icon = (tdef ? tdef->icon_path : std::string{});
    return true;
}

bool Hud::is_holding_item() const {
    return m_impl && m_impl->held_item_slot >= 0;
}

void Hud::cancel_held_item() {
    if (!m_impl) return;
    m_impl->held_item_slot = -1;
    m_impl->held_item_id   = UINT32_MAX;
    m_impl->held_item_icon.clear();
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
// Should the command bar's slots render this frame? Engine commands
// (Move / Stop / Hold / Attack) only make sense for the local player's
// own units. Future: filter individual slots against a per-unit-type
// allow-list (a building shouldn't show Move) — the gate here is
// "any-or-none" until that lands. As with action_bar, the bar frame
// itself always shows; only the slot icons are conditional.
static bool command_bar_slots_active(const Hud::Impl& s) {
    if (!s.world_ctx || !s.world_ctx->selection ||
        s.world_ctx->selection->selected().empty()) return false;
    u32 lead = s.world_ctx->selection->selected().front().id;
    const auto* own = s.world_ctx->world ? s.world_ctx->world->owners.get(lead) : nullptr;
    return own && own->player.id == s.world_ctx->local_player.id;
}

// Round-button command bar. Each slot is a disc; otherwise mirrors the
// classic style's hover/press/armed/icon/hotkey treatment. Pairs nicely
// with an Action-preset layout where you want a big primary action
// (attack) plus a few smaller satellites.
static void draw_command_bar_round(Hud& hud, Hud::Impl& s) {
    const auto& cfg = s.command_bar_cfg;

    bool slots_active = command_bar_slots_active(s);
    const std::string& armed = s.command_bar_armed_command;
    auto now = std::chrono::steady_clock::now();

    for (const auto& slot : cfg.slots) {
        if (!slot.visible) continue;
        bool is_armed = slots_active && (!armed.empty() && slot.command == armed);
        // The pulse keeps the press visual on for ~80 ms after a click
        // so a one-frame mouse_down → mouse_up still reads as "I clicked".
        bool press_visual = slot.pressed || now < slot.press_pulse_until;

        f32 cx = slot.rect.x + slot.rect.w * 0.5f;
        f32 cy = slot.rect.y + slot.rect.h * 0.5f;
        f32 button_r = std::min(slot.rect.w, slot.rect.h) * 0.5f - slot.style.border_width;
        if (button_r <= 0.0f) button_r = std::min(slot.rect.w, slot.rect.h) * 0.5f;

        Color bg = slot.style.bg;
        if (slots_active && press_visual)      bg = slot.style.press_bg;
        else if (is_armed)                     bg = slot.style.press_bg;
        else if (slots_active && slot.hovered) bg = slot.style.hover_bg;
        draw_disc(s, cx, cy, button_r, bg);

        if (slots_active && !slot.icon.empty()) {
            hud.draw_image_disc(cx, cy, button_r, slot.icon);
        }

        // Border ring. Armed state takes the bar-wide accent color +
        // thicker stroke, matching the classic style's behavior.
        Color border_color = slot.style.border_color;
        f32   border_w     = slot.style.border_width;
        if (is_armed) {
            border_color = cfg.style.armed_border_color;
            border_w     = cfg.style.armed_border_width;
        }
        if (border_w > 0.0f && (border_color.rgba >> 24) != 0) {
            f32 r_outer = button_r + border_w;
            draw_ring_arc(s, cx, cy, r_outer, button_r, 0.0f, 6.2831853f, border_color);
        }

        if (slots_active && !slot.hotkey.empty()) {
            f32 px_size = button_r * 0.45f;
            if (px_size < 10.0f) px_size = 10.0f;
            f32 text_w = hud.text_width_px(slot.hotkey, px_size);
            f32 ascent = hud.text_ascent_px(px_size);
            f32 x_left = cx + button_r * 0.55f - text_w * 0.5f;
            f32 y_base = cy - button_r * 0.55f + ascent * 0.5f;
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

static void draw_command_bar(Hud& hud, Hud::Impl& s) {
    const auto& cfg = s.command_bar_cfg;
    if (!cfg.enabled || !s.command_bar_rt.visible) return;

    if ((cfg.style.bg.rgba >> 24) != 0) hud.draw_rect(cfg.rect, cfg.style.bg);

    if (cfg.style_id == CommandBarStyleId::Round) {
        draw_command_bar_round(hud, s);
        return;
    }

    // Bar bg + slot frames always render. Slot CONTENT (icon, hotkey
    // badge, armed border) is gated — when no own unit is selected,
    // every slot reads as a blank frame so commands aren't suggested
    // to a player who can't issue them.
    bool slots_active = command_bar_slots_active(s);

    const std::string& armed = s.command_bar_armed_command;

    for (const auto& slot : cfg.slots) {
        if (!slot.visible) continue;
        bool is_armed = slots_active && (!armed.empty() && slot.command == armed);

        Color bg = slot.style.bg;
        if (slots_active && slot.pressed)      bg = slot.style.press_bg;
        else if (is_armed)                     bg = slot.style.press_bg;   // "held down" look
        else if (slots_active && slot.hovered) bg = slot.style.hover_bg;
        hud.draw_rect(slot.rect, bg);

        f32 bw = (slot.style.border_width > 0.0f) ? slot.style.border_width : 0.0f;
        Rect icon_rect{ slot.rect.x + bw, slot.rect.y + bw,
                        slot.rect.w - bw * 2.0f, slot.rect.h - bw * 2.0f };
        if (slots_active && !slot.icon.empty()) {
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

        if (slots_active && !slot.hotkey.empty()) {
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
// Solid filled disc — triangle fan from the center.
static void draw_disc(Hud::Impl& s, f32 cx, f32 cy, f32 radius, Color color) {
    if (radius <= 0.0f) return;
    if ((color.rgba >> 24) == 0) return;
    ensure_batch(s, PIPE_SOLID, s.white_set);
    u32 premul = premul_rgba(color);

    constexpr u32 kSegments = 32;
    constexpr f32 TWO_PI = 6.2831853f;
    f32 step = TWO_PI / static_cast<f32>(kSegments);
    for (u32 i = 0; i < kSegments; ++i) {
        f32 a0 = step * static_cast<f32>(i);
        f32 a1 = step * static_cast<f32>(i + 1);
        f32 x0 = cx + std::cos(a0) * radius;
        f32 y0 = cy + std::sin(a0) * radius;
        f32 x1 = cx + std::cos(a1) * radius;
        f32 y1 = cy + std::sin(a1) * radius;
        append_triangle(s, cx, cy, x0, y0, x1, y1, 0.0f, 0.0f, premul);
    }
}

// Partial ring (annular sector). Sweeps `sweep_angle` radians from
// `start_angle`, with thickness (r_outer − r_inner). Used for the MOBA
// cooldown ring.
static void draw_ring_arc(Hud::Impl& s, f32 cx, f32 cy, f32 r_outer, f32 r_inner,
                          f32 start_angle, f32 sweep_angle, Color color) {
    if (r_outer <= r_inner || r_outer <= 0.0f) return;
    if (sweep_angle <= 0.0f) return;
    if ((color.rgba >> 24) == 0) return;
    ensure_batch(s, PIPE_SOLID, s.white_set);
    u32 premul = premul_rgba(color);

    constexpr u32 kSegmentsFull = 48;
    constexpr f32 TWO_PI = 6.2831853f;
    f32 frac = sweep_angle / TWO_PI;
    if (frac > 1.0f) frac = 1.0f;
    u32 n = static_cast<u32>(std::ceil(static_cast<f32>(kSegmentsFull) * frac));
    if (n < 1) n = 1;
    f32 step = sweep_angle / static_cast<f32>(n);
    for (u32 i = 0; i < n; ++i) {
        f32 a0 = start_angle + step * static_cast<f32>(i);
        f32 a1 = start_angle + step * static_cast<f32>(i + 1);
        f32 c0 = std::cos(a0), s0 = std::sin(a0);
        f32 c1 = std::cos(a1), s1 = std::sin(a1);
        f32 ox0 = cx + c0 * r_outer, oy0 = cy + s0 * r_outer;
        f32 ox1 = cx + c1 * r_outer, oy1 = cy + s1 * r_outer;
        f32 ix0 = cx + c0 * r_inner, iy0 = cy + s0 * r_inner;
        f32 ix1 = cx + c1 * r_inner, iy1 = cy + s1 * r_inner;
        append_triangle(s, ox0, oy0, ox1, oy1, ix1, iy1, 0.0f, 0.0f, premul);
        append_triangle(s, ox0, oy0, ix1, iy1, ix0, iy0, 0.0f, 0.0f, premul);
    }
}

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

Hud::TargetingIntent Hud::cursor_intent() const {
    if (!m_impl || !m_impl->world_ctx) return TargetingIntent::Neutral;
    const auto& ctx = *m_impl->world_ctx;
    if (!ctx.picker || !ctx.world) return TargetingIntent::Neutral;
    // Picker takes physical-pixel coords; HUD pointer is dp.
    f32 sx = m_impl->pointer_x * m_impl->ui_scale;
    f32 sy = m_impl->pointer_y * m_impl->ui_scale;
    // Item beats unit when both share the cursor — items are smaller so
    // the player almost always wants the item-pickup intent in that case.
    if (auto item = ctx.picker->pick_item(sx, sy); item.is_valid()) {
        return TargetingIntent::Item;
    }
    if (auto unit = ctx.picker->pick_target(sx, sy); unit.is_valid()) {
        const auto* owner = ctx.world->owners.get(unit.id);
        if (!owner) return TargetingIntent::Neutral;
        if (owner->player.id == ctx.local_player.id) return TargetingIntent::Ally;
        if (ctx.simulation && ctx.simulation->is_allied(ctx.local_player, owner->player)) {
            return TargetingIntent::Ally;
        }
        return TargetingIntent::Enemy;
    }
    return TargetingIntent::Neutral;
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
    return m_impl && m_impl->joystick_rt.captured_id != -1;
}

i32 Hud::joystick_captured_slot() const {
    return m_impl ? m_impl->joystick_rt.captured_slot : -1;
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
        rt.captured_id = -1;
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

    // Resolve the captured pointer's CURRENT slot from its stable ID.
    // Touches compact when a non-primary lifts (Android), so the slot
    // index of "the same finger" can change frame to frame — we look
    // it up rather than trusting a cached slot. Returns -1 if the ID
    // is no longer present (finger lifted).
    auto find_slot_for_id = [&](i32 id) -> i32 {
        if (id < 0) return -1;
        for (u32 i = 0; i < input.touch_count; ++i) {
            if (input.touch_id[i] == id) return static_cast<i32>(i);
        }
        return -1;
    };

    // Captured driver of the stick. captured_id == -2 is the desktop
    // mouse path (no real pointer ID); >=0 is a touch ID.
    constexpr i32 MOUSE_ID = -2;
    if (rt.captured_id == MOUSE_ID) {
        if (!input.mouse_left) {
            rt.captured_id = -1;
            rt.captured_slot = -1;
            rt.knob_dx = rt.knob_dy = 0.0f;
            rt.base_cx = home_cx;
            rt.base_cy = home_cy;
            return;
        }
        f32 fx = map_x(input.mouse_x);
        f32 fy = map_y(input.mouse_y);
        rt.captured_slot = 0;  // mouse aliases primary pointer
        f32 ex = fx - rt.base_cx;
        f32 ey = fy - rt.base_cy;
        f32 mag2 = ex * ex + ey * ey;
        if (mag2 > travel * travel) {
            f32 mag = std::sqrt(mag2);
            ex = ex / mag * travel;
            ey = ey / mag * travel;
        }
        rt.knob_dx = ex; rt.knob_dy = ey;
        f32 nx = ex / travel, ny = ey / travel;
        f32 mag = std::sqrt(nx * nx + ny * ny);
        if (mag < cfg.style.deadzone_frac) {
            rt.out_x = rt.out_y = 0.0f;
        } else {
            f32 scale_out = (mag - cfg.style.deadzone_frac)
                          / (1.0f - cfg.style.deadzone_frac) / mag;
            rt.out_x = nx * scale_out;
            rt.out_y = ny * scale_out;
        }
        return;
    }

    if (rt.captured_id >= 0) {
        i32 slot = find_slot_for_id(rt.captured_id);
        if (slot < 0) {
            // Finger released. Return the base to its home position so
            // the next idle render shows the configured anchor, not
            // the last press point.
            rt.captured_id = -1;
            rt.captured_slot = -1;
            rt.knob_dx = rt.knob_dy = 0.0f;
            rt.base_cx = home_cx;
            rt.base_cy = home_cy;
            return;
        }
        rt.captured_slot = slot;
        f32 fx = map_x(input.touch_x[slot]);
        f32 fy = map_y(input.touch_y[slot]);
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

    // Floating-joystick anchor. The hud.json rect is the *activation*
    // area; the base center is clamped to that rect shrunk inward by
    // the knob's travel distance. A press at the activation edge
    // clamps the base to the corresponding inner edge, putting the
    // knob at full deflection so a move-order emits immediately —
    // modern MOBA behavior. A press inside the inner (shrunk) region
    // anchors the base AT the touch with knob centered (no immediate
    // move). Shrinking by `travel` (rather than the full visible
    // base_r) keeps the float region usable: shrinking by base_r
    // would collapse it for typical configs where rect.w ≈ 2·base_r.
    // The visible disc may extend slightly past the rect when at the
    // edge of the float region — same as MLBB / AoV / Wild Rift.
    auto compute_anchor = [&](f32 fx, f32 fy, f32& cx, f32& cy) {
        f32 cx_min = cfg.rect.x + travel;
        f32 cx_max = cfg.rect.x + cfg.rect.w - travel;
        f32 cy_min = cfg.rect.y + travel;
        f32 cy_max = cfg.rect.y + cfg.rect.h - travel;
        if (cx_max < cx_min) { cx_min = cx_max = cfg.rect.x + cfg.rect.w * 0.5f; }
        if (cy_max < cy_min) { cy_min = cy_max = cfg.rect.y + cfg.rect.h * 0.5f; }
        cx = std::clamp(fx, cx_min, cx_max);
        cy = std::clamp(fy, cy_min, cy_max);
    };

    // Compute knob + output for a freshly anchored base. Pulled out so
    // both touch and mouse capture lambdas can fire a move-order on
    // the same frame the press lands — without this, the captured
    // branch above runs only next tick, so a touch in the outer
    // activation ring takes one frame to start moving the unit.
    auto apply_press_output = [&](f32 fx, f32 fy) {
        f32 ex = fx - rt.base_cx;
        f32 ey = fy - rt.base_cy;
        f32 mag2 = ex * ex + ey * ey;
        if (mag2 > travel * travel) {
            f32 m = std::sqrt(mag2);
            ex = ex / m * travel;
            ey = ey / m * travel;
        }
        rt.knob_dx = ex;
        rt.knob_dy = ey;
        f32 nx = ex / travel, ny = ey / travel;
        f32 mag = std::sqrt(nx * nx + ny * ny);
        if (mag < cfg.style.deadzone_frac) {
            rt.out_x = rt.out_y = 0.0f;
        } else {
            f32 scale_out = (mag - cfg.style.deadzone_frac)
                          / (1.0f - cfg.style.deadzone_frac) / mag;
            rt.out_x = nx * scale_out;
            rt.out_y = ny * scale_out;
        }
    };

    auto try_capture_touch = [&](u32 t, f32 fx, f32 fy) -> bool {
        if (!joystick_hit_test_point(cfg, fx, fy)) return false;
        rt.captured_id   = input.touch_id[t];
        rt.captured_slot = static_cast<i32>(t);
        compute_anchor(fx, fy, rt.base_cx, rt.base_cy);
        apply_press_output(fx, fy);
        return true;
    };
    auto try_capture_mouse = [&](f32 fx, f32 fy) -> bool {
        if (!joystick_hit_test_point(cfg, fx, fy)) return false;
        rt.captured_id   = MOUSE_ID;
        rt.captured_slot = -1;
        compute_anchor(fx, fy, rt.base_cx, rt.base_cy);
        apply_press_output(fx, fy);
        return true;
    };

    if (input.touch_count > 0) {
        for (u32 t = 0; t < input.touch_count; ++t) {
            if (try_capture_touch(t,
                                  map_x(input.touch_x[t]),
                                  map_y(input.touch_y[t]))) {
                break;
            }
        }
    } else if (input.mouse_left_pressed) {
        try_capture_mouse(map_x(input.mouse_x), map_y(input.mouse_y));
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
        if (s.drag_cast.inventory_slot >= 0) {
            if (static_cast<u32>(s.drag_cast.inventory_slot) < s.inventory_cfg.slots.size())
                s.inventory_cfg.slots[s.drag_cast.inventory_slot].pressed = false;
        } else if (s.drag_cast.slot_index >= 0) {
            bool is_command = !s.drag_cast.command_id.empty();
            if (is_command) {
                if (static_cast<u32>(s.drag_cast.slot_index) < s.command_bar_cfg.slots.size())
                    s.command_bar_cfg.slots[s.drag_cast.slot_index].pressed = false;
            } else {
                if (static_cast<u32>(s.drag_cast.slot_index) < s.action_bar_cfg.slots.size())
                    s.action_bar_cfg.slots[s.drag_cast.slot_index].pressed = false;
            }
        }
        s.drag_cast = Impl::DragCastState{};
        return;
    }
    if (auto* tf = s.world_ctx->world->transforms.get(s.drag_cast.caster.id)) {
        s.drag_cast.caster_x = tf->position.x;
        s.drag_cast.caster_y = tf->position.y;
        s.drag_cast.caster_z = tf->position.z;
    }

    // Find the touch slot owning the drag-cast finger. With one-finger
    // play it's slot 0 and the mouse_x/y mirror works fine. With
    // joystick + drag-cast (two fingers), input.mouse_x/y reflect the
    // joystick slot (always slot 0 in the platform layer), and
    // input.mouse_left stays true while the joystick is held even
    // after the drag-cast finger lifts — so polling those would put
    // the drag in the wrong place AND mask the release. We skip the
    // joystick's slot and take the first remaining touch as the drag
    // finger; release is detected by that slot disappearing from the
    // live touch list.
    f32 inv = (s.ui_scale > 0.0f) ? (1.0f / s.ui_scale) : 1.0f;
    i32 stick_slot = s.joystick_rt.captured_slot;
    i32 drag_slot  = -1;
    for (u32 i = 0; i < input.touch_count; ++i) {
        if (static_cast<i32>(i) == stick_slot) continue;
        drag_slot = static_cast<i32>(i);
        break;
    }
    bool drag_down;
    f32 px, py;
    if (drag_slot >= 0) {
        px = input.touch_x[drag_slot] * inv;
        py = input.touch_y[drag_slot] * inv;
        drag_down = true;
    } else {
        // No live touch other than the joystick (or no joystick + no
        // touches). Use last-known dp position so the release-frame
        // computations downstream see consistent coords; drag_down
        // false drives the release branch.
        px = s.drag_cast.current_x;
        py = s.drag_cast.current_y;
        drag_down = false;
    }
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

    // Snap (TargetUnit form only). Pick the nearest valid candidate
    // within a snap radius of the drag point; static filter eval, no
    // network round-trip. Two filter regimes share this loop:
    //   • abilities — gated by AbilityDef::target_filter (ally/enemy/
    //     self / classifications). Without a def we can't evaluate;
    //     the loop bails.
    //   • commands  — Move and Attack accept any unit (Move-on-unit
    //     becomes Follow; Attack-on-unit attacks regardless of
    //     alliance — friendly fire allowed). Self never snaps for
    //     commands; "follow yourself" / "attack yourself" are nonsense.
    // Snap radius scales with cast range so close- and long-range
    // abilities feel similar; commands have range 0 so they get the
    // 64-unit floor.
    //
    // Recompute only while the finger is held; on the release frame
    // (mouse_left=false) we keep the last good snap. The OS can
    // drift the UP-event position a few px away from the prior
    // MOVE due to event coalescing and the natural lift-off motion
    // of a finger — re-running snap on release would let that
    // jitter erase the snap the player just saw and turn
    // "release on indicator" into a silent cancel. Holding the
    // last value matches the player's mental model: if the
    // indicator was up when they let go, the cast fires.
    if (drag_down) {
        s.drag_cast.snapped_target = simulation::Unit{};
    }
    if (drag_down &&
        s.drag_cast.form == simulation::AbilityForm::Target &&
        s.drag_cast.widget_kinds != 0 &&
        s.world_ctx->simulation) {
        bool is_command = !s.drag_cast.command_id.empty();
        const simulation::AbilityDef* def = nullptr;
        if (!is_command) {
            def = s.world_ctx->abilities
                    ? s.world_ctx->abilities->get(s.drag_cast.ability_id)
                    : nullptr;
        }
        bool eligible = is_command || def != nullptr;
        if (eligible) {
            const auto& world = *s.world_ctx->world;
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
                if (cand.id == caster_unit.id) {
                    // Commands never self-snap. Abilities use the
                    // filter's `self_` flag.
                    if (is_command || !def->target_filter.self_) continue;
                }
                // Commands always reject dead units — Move-on-corpse can't
                // follow, Attack-on-corpse can't attack. Abilities run the
                // full filter (alive/dead flags handled inside).
                if (is_command && world.dead_states.has(id)) continue;
                if (!is_command &&
                    !s.world_ctx->simulation->target_filter_passes(
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
    //   - Press → Aiming once the finger moves more than `TAP_SLOP` dp
    //     from the press point. Below that threshold the touch reads
    //     as a tap candidate (jitter / accidental motion within the
    //     tap-slop budget); release-on-slot fires the no-target use,
    //     release-off-slot cancels.
    //   - Aiming ↔ Cancelling driven by the AoV-style cancel zone, NOT
    //     the slot itself. Dragging back over the slot used to trigger
    //     cancel; that was awkward (the finger naturally returns near
    //     the slot during fine-aiming) so we moved it to a dedicated
    //     screen rect that the player explicitly drags into.
    constexpr f32 CANCEL_MARGIN     = 16.0f;
    constexpr f32 SLOT_LEAVE_MARGIN = 8.0f;
    constexpr f32 TAP_SLOP          = 8.0f;
    // Slot rect comes from whichever composite started the drag —
    // action_bar for ability slots, command_bar for command slots,
    // inventory for item slots.
    Rect slot_rect{};
    if (s.drag_cast.inventory_slot >= 0) {
        if (static_cast<u32>(s.drag_cast.inventory_slot) < s.inventory_cfg.slots.size())
            slot_rect = s.inventory_cfg.slots[s.drag_cast.inventory_slot].rect;
    } else if (s.drag_cast.slot_index >= 0) {
        if (!s.drag_cast.command_id.empty()) {
            if (static_cast<u32>(s.drag_cast.slot_index) < s.command_bar_cfg.slots.size())
                slot_rect = s.command_bar_cfg.slots[s.drag_cast.slot_index].rect;
        } else {
            if (static_cast<u32>(s.drag_cast.slot_index) < s.action_bar_cfg.slots.size())
                slot_rect = s.action_bar_cfg.slots[s.drag_cast.slot_index].rect;
        }
    }
    bool over_slot   = (slot_rect.w > 0.0f && slot_rect.h > 0.0f) &&
                       (px >= slot_rect.x - SLOT_LEAVE_MARGIN &&
                        px <  slot_rect.x + slot_rect.w + SLOT_LEAVE_MARGIN &&
                        py >= slot_rect.y - SLOT_LEAVE_MARGIN &&
                        py <  slot_rect.y + slot_rect.h + SLOT_LEAVE_MARGIN);
    bool over_cancel = action_bar_cancel_zone_contains(s, px, py, CANCEL_MARGIN);
    bool button_down = drag_down;

    bool is_inventory = (s.drag_cast.inventory_slot >= 0);
    bool is_command   = !s.drag_cast.command_id.empty();

    if (button_down) {
        if (s.drag_cast.phase == Phase::Pressed) {
            // Inventory long-press: stationary press for 500 ms while
            // still over the slot lifts the item into held mode (the
            // mobile equivalent of right-click on desktop). Lifting
            // wins over drag-cast — once lifted, drag_cast is cleared
            // and the next tap drops/swaps via the existing held-item
            // release path. Slid-off presses don't lift; they either
            // become drag-cast (if targetable) or cancel on release.
            if (is_inventory && over_slot) {
                auto held_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - s.drag_cast.press_time).count();
                if (held_ms >= 500) {
                    s.held_item_slot = s.drag_cast.inventory_slot;
                    s.held_item_id   = s.drag_cast.inventory_item_id;
                    s.held_item_icon = s.drag_cast.inventory_item_icon;
                    if (static_cast<u32>(s.drag_cast.inventory_slot) < s.inventory_cfg.slots.size())
                        s.inventory_cfg.slots[s.drag_cast.inventory_slot].pressed = false;
                    s.drag_cast = Impl::DragCastState{};
                    return;
                }
            }
            // Moving past TAP_SLOP commits to Aiming — but only when a
            // drag-cast actually makes sense. For non-targetable
            // inventory items (passive, self-only, or on cooldown) we
            // stay in Pressed; release after slide-off is handled as
            // a cancel by the inventory release branch below.
            f32 mdx = px - s.drag_cast.press_x;
            f32 mdy = py - s.drag_cast.press_y;
            if (mdx * mdx + mdy * mdy > TAP_SLOP * TAP_SLOP) {
                bool can_aim = !is_inventory || s.drag_cast.inventory_targetable;
                if (can_aim) s.drag_cast.phase = Phase::Aiming;
            }
        } else if (s.drag_cast.phase == Phase::Aiming) {
            if (over_cancel) s.drag_cast.phase = Phase::Cancelling;
        } else if (s.drag_cast.phase == Phase::Cancelling) {
            if (!over_cancel) s.drag_cast.phase = Phase::Aiming;
        }
        return;
    }

    // Release. Three commit paths — inventory, command, ability —
    // each with their own cancel rules.
    if (is_inventory) {
        // Inventory release branches:
        //   Pressed + over_slot  → quick tap, fire no-target use.
        //   Pressed + slide-off  → cancel.
        //   Aiming  + widget-accepting → cancel unless a widget was snapped.
        //   Aiming  + point-only → fire use-at-target with the drag
        //                          point (no snap needed).
        //   Cancelling           → cancel.
        bool fire_no_target = false;
        bool fire_at_target = false;
        if (s.drag_cast.phase == Phase::Pressed && over_slot) {
            fire_no_target = true;
        } else if (s.drag_cast.phase == Phase::Aiming) {
            if (s.drag_cast.widget_kinds != 0) {
                if (s.drag_cast.snapped_target.is_valid()) fire_at_target = true;
                else if (s.drag_cast.accept_point) fire_at_target = true;
            } else if (s.drag_cast.accept_point) {
                fire_at_target = true;
            }
        }
        if (fire_no_target && s.inventory_use_fn && !s.drag_cast.ability_id.empty()) {
            s.inventory_use_fn(s.drag_cast.inventory_item_id, s.drag_cast.ability_id);
        } else if (fire_at_target && s.inventory_use_at_target_fn) {
            u32 target_uid = s.drag_cast.snapped_target.is_valid()
                               ? s.drag_cast.snapped_target.id
                               : UINT32_MAX;
            glm::vec3 wp{s.drag_cast.drag_world_x,
                         s.drag_cast.drag_world_y,
                         s.drag_cast.drag_world_z};
            s.inventory_use_at_target_fn(s.drag_cast.inventory_item_id,
                                         s.drag_cast.ability_id,
                                         target_uid, wp);
        }
        if (s.drag_cast.inventory_slot >= 0 &&
            static_cast<u32>(s.drag_cast.inventory_slot) < s.inventory_cfg.slots.size())
            s.inventory_cfg.slots[s.drag_cast.inventory_slot].pressed = false;
        s.drag_cast = Impl::DragCastState{};
        return;
    }

    bool commit = (s.drag_cast.phase == Phase::Aiming);

    // Tap-fire on a command (attack / attack_move) — Pressed at
    // release means the finger never moved past TAP_SLOP, so it's a
    // genuine tap. Resolve through focus_target (the auto / manual
    // focus). A tap on Move with no direction is still genuinely
    // ambiguous and stays a no-op.
    if (!commit && is_command && s.drag_cast.phase == Phase::Pressed) {
        if (s.drag_cast.command_id == "attack" ||
            s.drag_cast.command_id == "attack_move") {
            if (s.focus_target_unit.is_valid() && s.world_ctx &&
                s.world_ctx->world &&
                s.world_ctx->world->validate(s.focus_target_unit)) {
                s.drag_cast.snapped_target = s.focus_target_unit;
                if (auto* tf = s.world_ctx->world->transforms.get(
                                   s.focus_target_unit.id)) {
                    s.drag_cast.drag_world_x = tf->position.x;
                    s.drag_cast.drag_world_y = tf->position.y;
                    s.drag_cast.drag_world_z = tf->position.z;
                }
                commit = true;
            }
        }
    }

    // Pressed release for abilities — finger never moved past
    // TAP_SLOP, so it's a tap. Use focus_target when the ability's
    // target_filter accepts it; otherwise fall back to whatever snap
    // the player saw under their finger (typically nothing for a
    // pure tap). Only the explicit cancel zone should cancel.
    if (!commit && !is_command && s.drag_cast.phase == Phase::Pressed) {
        // Resolve focus_target through the ability's target_filter so
        // a heal-on-enemy focus falls back to the local snap instead
        // of casting on something the spell can't touch.
        bool focus_usable = false;
        if (s.focus_target_unit.is_valid() && s.world_ctx && s.world_ctx->world &&
            s.world_ctx->world->validate(s.focus_target_unit)) {
            const auto* def = s.world_ctx->abilities
                                ? s.world_ctx->abilities->get(s.drag_cast.ability_id)
                                : nullptr;
            if (def && s.world_ctx->simulation) {
                focus_usable = s.world_ctx->simulation->target_filter_passes(
                    def->target_filter, s.drag_cast.caster, s.focus_target_unit);
            }
        }

        if (s.drag_cast.widget_kinds != 0) {
            if (focus_usable) {
                s.drag_cast.snapped_target = s.focus_target_unit;
                commit = true;
            } else if (s.drag_cast.snapped_target.is_valid()) {
                commit = true;
            } else if (s.drag_cast.accept_point) {
                commit = true;   // hybrid form: fall through to ground
            }
        } else if (s.drag_cast.accept_point) {
            // For AoE casts, drop the indicator on the focus target's
            // position so a tap-and-fire feels like "this enemy gets
            // hit" rather than always landing under the caster.
            if (focus_usable) {
                if (auto* tf = s.world_ctx->world->transforms.get(
                                   s.focus_target_unit.id)) {
                    s.drag_cast.drag_world_x = tf->position.x;
                    s.drag_cast.drag_world_y = tf->position.y;
                    s.drag_cast.drag_world_z = tf->position.z;
                }
            }
            commit = true;
        }
    }
    // Ability-side: widget-only casts cancel if released without snap
    // (no valid target = nothing to cast on). Hybrid (widget + point)
    // and point-only always commit — falling through to the ground.
    // Command-side: both Move and Attack always commit — with a snap
    // they go to Follow / Attack-unit; without one they fall back to
    // the ground point (Move-to-point / AttackMove).
    if (commit && !is_command &&
        s.drag_cast.form == simulation::AbilityForm::Target &&
        s.drag_cast.widget_kinds != 0 && !s.drag_cast.accept_point &&
        !s.drag_cast.snapped_target.is_valid()) {
        commit = false;
    }
    if (commit) {
        u32 target_uid = s.drag_cast.snapped_target.is_valid()
                           ? s.drag_cast.snapped_target.id
                           : UINT32_MAX;
        if (is_command) {
            if (s.command_bar_drag_commit_fn) {
                s.command_bar_drag_commit_fn(s.drag_cast.command_id, target_uid,
                                             s.drag_cast.drag_world_x,
                                             s.drag_cast.drag_world_y,
                                             s.drag_cast.drag_world_z);
            }
        } else if (s.action_bar_cast_at_target_fn) {
            s.action_bar_cast_at_target_fn(s.drag_cast.ability_id, target_uid,
                                           s.drag_cast.drag_world_x,
                                           s.drag_cast.drag_world_y,
                                           s.drag_cast.drag_world_z);
        }
    }

    // Reset visual + state regardless of commit/cancel.
    if (s.drag_cast.slot_index >= 0) {
        if (is_command) {
            if (static_cast<u32>(s.drag_cast.slot_index) < s.command_bar_cfg.slots.size())
                s.command_bar_cfg.slots[s.drag_cast.slot_index].pressed = false;
        } else {
            if (static_cast<u32>(s.drag_cast.slot_index) < s.action_bar_cfg.slots.size())
                s.action_bar_cfg.slots[s.drag_cast.slot_index].pressed = false;
        }
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
        out.source        = (dc.inventory_slot >= 0)
                              ? TargetingSource::Item
                              : (dc.command_id.empty()
                                  ? TargetingSource::Ability
                                  : TargetingSource::Command);
        out.is_drag_cast  = true;
        out.caster_x   = dc.caster_x;
        out.caster_y   = dc.caster_y;
        out.caster_z   = dc.caster_z;
        out.drag_x     = dc.drag_world_x;
        out.drag_y     = dc.drag_world_y;
        out.drag_z     = dc.drag_world_z;
        out.range      = dc.range;
        // Widget-snapped this frame? Hybrid forms can switch between
        // widget and point each frame as the cursor moves.
        out.is_unit_target = dc.snapped_target.is_valid();

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

    // Desktop command-targeting path — preset is waiting for a ground
    // click after the player tapped Move / AttackMove (or pressed A).
    // Range / area are zero (these are simple ground clicks), so the
    // visual layer falls through to the cursor swap + post-commit
    // ping. This path makes commands first-class members of the
    // unified targeting state alongside abilities.
    if (!m_impl->action_bar_targeting_ability.empty()) {
        // Ability path takes precedence — fall through below.
    } else if (!m_impl->command_bar_armed_command.empty()) {
        out.active = true;
        out.source = TargetingSource::Command;
        // Commands target a ground point (Move / AttackMove). Set
        // is_unit_target false so the visual layer renders the
        // point-targeting cue, not a snap ring.
        out.is_unit_target = false;
        if (m_impl->world_ctx && m_impl->world_ctx->selection) {
            const auto& sel = m_impl->world_ctx->selection->selected();
            if (!sel.empty() && m_impl->world_ctx->world) {
                if (auto* tf = m_impl->world_ctx->world->transforms.get(sel.front().id)) {
                    out.caster_x = tf->position.x;
                    out.caster_y = tf->position.y;
                    out.caster_z = tf->position.z;
                }
            }
        }
        return out;
    }

    // Desktop ability targeting-mode path — preset has armed an ability
    // and is waiting on a world click. Indicator follows the mouse-
    // ground-pick and snaps to a unit (for target_unit forms) using
    // the same target_filter the mobile drag-cast snap consults.
    if (m_impl->action_bar_targeting_ability.empty()) return out;
    if (!m_impl->world_ctx) return out;
    const auto& ctx = *m_impl->world_ctx;
    if (!ctx.world || !ctx.abilities || !ctx.selection || !ctx.picker) return out;

    const auto* def = ctx.abilities->get(m_impl->action_bar_targeting_ability);
    if (!def) return out;
    // Target-form ability with at least one of widget snap / point fall-
    // through enabled. `is_unit` here means the cursor accepts a widget
    // pick at all (drives reticle logic below); a hybrid ability has
    // both is_unit and accept_point true.
    if (def->form != simulation::AbilityForm::Target) return out;
    bool is_unit  = (def->widget_kinds != 0);
    bool accept_point = def->accept_point;
    if (!is_unit && !accept_point) return out;

    if (ctx.selection->selected().empty()) return out;
    simulation::Unit caster_unit = ctx.selection->selected().front();
    const auto* caster_tf = ctx.world->transforms.get(caster_unit.id);
    if (!caster_tf) return out;

    out.active   = true;
    out.source   = TargetingSource::Ability;
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
                    if (inst.from_item) continue;  // item abilities fire via inventory slot, not hotkey

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
        if (!def || def->hidden || inst.from_item) continue;
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
    return f == F::Instant || f == F::Target;
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

        // Slot background. Hover / press feedback only fires when there
        // IS a button to fire — an empty slot reads as a static frame
        // so hovering it doesn't suggest interactivity that isn't
        // there. Armed (targeting-mode) state implies an ability was
        // resolved, so it stays gated by `inst && def`.
        bool has_button = (inst && def);
        Color bg = slot.style.bg;
        if (has_button && slot.pressed)      bg = slot.style.press_bg;
        else if (armed)                      bg = slot.style.press_bg;
        else if (has_button && slot.hovered) bg = slot.style.hover_bg;
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

// Mobile-MOBA-style action bar render path. Each slot is a round button:
// solid disc background + centered icon + a bright cooldown ring sweeping
// just outside the button perimeter. Hit-testing is still rect-based via
// slot.rect (clicks near the disc's bounding-box corners still fire),
// which feels lenient on touch — fine for a thumb target.
static void draw_action_bar_moba(Hud& hud, Hud::Impl& s) {
    const auto& cfg = s.action_bar_cfg;
    const auto& rt  = s.action_bar_rt;

    bool any_armed = !s.action_bar_targeting_ability.empty();

    for (u32 i = 0; i < cfg.slots.size(); ++i) {
        const auto& slot = cfg.slots[i];
        if (!slot.visible) continue;

        const simulation::AbilityDef* def = nullptr;
        const simulation::AbilityInstance* inst = nullptr;
        if (s.world_ctx) inst = resolve_slot_ability(i, cfg, *s.world_ctx, def);

        bool armed = any_armed && def && inst
                  && inst->ability_id == s.action_bar_targeting_ability;
        bool has_button = (inst && def);

        f32 cx = slot.rect.x + slot.rect.w * 0.5f;
        f32 cy = slot.rect.y + slot.rect.h * 0.5f;
        // Reserve room outside the button for the cooldown ring + gap.
        f32 ring_w = cfg.style.cooldown_ring_width;
        f32 ring_g = cfg.style.cooldown_ring_gap;
        f32 button_r = std::min(slot.rect.w, slot.rect.h) * 0.5f - ring_w - ring_g;
        if (button_r <= 0.0f) button_r = std::min(slot.rect.w, slot.rect.h) * 0.4f;

        // Background disc — hover / press / armed state.
        Color bg = slot.style.bg;
        if (has_button && slot.pressed)      bg = slot.style.press_bg;
        else if (armed)                      bg = slot.style.press_bg;
        else if (has_button && slot.hovered) bg = slot.style.hover_bg;
        draw_disc(s, cx, cy, button_r, bg);

        // Icon as a circle-clipped disc — fills the button entirely,
        // with the texture's square corners cropped at the rim.
        if (def && !def->icon.empty()) {
            hud.draw_image_disc(cx, cy, button_r, def->icon);
        }

        if (armed) {
            // Armed slot: skip cooldown / affordability overlays.
        } else if (any_armed) {
            // Wash other slots while one is armed.
            draw_disc(s, cx, cy, button_r, cfg.style.disabled_tint);
        } else {
            bool on_cooldown = def && inst && inst->cooldown_remaining > 0.05f
                            && def->level_data(inst->level).cooldown > 0.0f;
            if (on_cooldown) {
                f32 total = def->level_data(inst->level).cooldown;
                f32 frac  = inst->cooldown_remaining / total;

                // Dark tint over the icon while on cooldown.
                draw_disc(s, cx, cy, button_r, cfg.style.cooldown_overlay);

                // Bright ring around the perimeter — remaining fraction
                // sweeping clockwise from 12 o'clock. Ring shrinks as
                // the cooldown elapses.
                constexpr f32 TWO_PI = 6.2831853f;
                constexpr f32 TWELVE_OCLOCK = -1.5707963f;  // -PI/2
                f32 r_outer = button_r + ring_g + ring_w;
                f32 r_inner = button_r + ring_g;
                f32 sweep   = frac * TWO_PI;
                draw_ring_arc(s, cx, cy, r_outer, r_inner,
                              TWELVE_OCLOCK, sweep, cfg.style.cooldown_ring_color);

                // Remaining seconds, centered.
                char buf[16];
                format_cooldown_secs(inst->cooldown_remaining, buf, sizeof(buf));
                if (buf[0] != '\0') {
                    f32 px = cfg.style.cooldown_text_size;
                    f32 tw = hud.text_width_px(buf, px);
                    f32 line_h = hud.text_line_height_px(px);
                    f32 ascent = hud.text_ascent_px(px);
                    f32 tx = cx - tw * 0.5f;
                    f32 ty = cy - line_h * 0.5f + ascent;
                    hud.draw_text(tx, ty, buf, cfg.style.cooldown_text_color, px);
                }
            } else if (def && inst && s.world_ctx && is_castable_form(def->form)) {
                u32 unit_id = s.world_ctx->selection
                                ? s.world_ctx->selection->selected().front().id
                                : UINT32_MAX;
                if (unit_id != UINT32_MAX && s.world_ctx->world) {
                    if (!can_afford(*s.world_ctx->world, unit_id,
                                    def->level_data(inst->level))) {
                        draw_disc(s, cx, cy, button_r, cfg.style.disabled_tint);
                    }
                }
            }
        }

        // Border ring — concentric with the button. Armed override
        // matches ClassicRts (accent color, thicker stroke).
        Color border_color = slot.style.border_color;
        f32   border_w     = slot.style.border_width;
        if (armed) {
            border_color = cfg.style.armed_border_color;
            border_w     = cfg.style.armed_border_width;
        }
        if (border_w > 0.0f && (border_color.rgba >> 24) != 0) {
            draw_ring(s, cx, cy, button_r, button_r - border_w, border_color);
        }

        // Hotkey badge — bottom-right corner of the slot rect (sits
        // slightly outside the disc). Only drawn for castable forms,
        // same as ClassicRts.
        bool show_hotkey = def && is_castable_form(def->form);
        std::string_view badge_key;
        if (show_hotkey) {
            badge_key = (rt.hotkey_mode == ActionBarHotkeyMode::Ability)
                            ? std::string_view{def->hotkey}
                            : std::string_view{slot.hotkey};
        }
        if (!badge_key.empty()) {
            f32 px_size = button_r * 0.55f;
            if (px_size < 10.0f) px_size = 10.0f;
            f32 text_w = hud.text_width_px(badge_key, px_size);
            f32 ascent = hud.text_ascent_px(px_size);
            f32 x_left = slot.rect.x + slot.rect.w - text_w - 4.0f;
            f32 y_base = slot.rect.y + slot.rect.h - 4.0f;

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
    const auto* vision = s.world_ctx->vision;

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
        if (vision) {
            i32 tx = static_cast<i32>((tf.position.x - td.origin_x()) / td.tile_size);
            i32 ty = static_cast<i32>((tf.position.y - td.origin_y()) / td.tile_size);
            if (tx >= 0 && ty >= 0 &&
                static_cast<u32>(tx) < td.tiles_x &&
                static_cast<u32>(ty) < td.tiles_y) {
                if (!vision->is_visible(s.world_ctx->local_player,
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
    // Show only during an actual drag — Aiming or Cancelling. Pressed
    // means the finger hasn't moved past TAP_SLOP yet (tap candidate),
    // so the cancel zone would just clutter taps. Once any drag past
    // TAP_SLOP commits to Aiming, the cancel zone appears.
    using Phase = Hud::Impl::DragCastPhase;
    if (s.drag_cast.phase != Phase::Aiming &&
        s.drag_cast.phase != Phase::Cancelling) return;
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

// ── Inventory composite ─────────────────────────────────────────────────

// Mirrors action_bar_hit_test / command_bar_hit_test — same shape.
static i32 inventory_hit_test(const Hud::Impl& s, f32 x, f32 y) {
    const auto& cfg = s.inventory_cfg;
    if (!cfg.enabled || !s.inventory_rt.visible) return -1;
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

// Look up the local selection's lead unit's Inventory. Returns nullptr
// when nothing is selected, the unit has no Inventory component, or
// world context is incomplete.
static const simulation::Inventory*
inventory_resolve_selected(const Hud::Impl& s, u32* out_carrier_id) {
    if (out_carrier_id) *out_carrier_id = UINT32_MAX;
    if (!s.world_ctx || !s.world_ctx->world || !s.world_ctx->selection) return nullptr;
    const auto& sel = s.world_ctx->selection->selected();
    if (sel.empty()) return nullptr;
    u32 id = sel.front().id;
    const auto* inv = s.world_ctx->world->inventories.get(id);
    if (inv && out_carrier_id) *out_carrier_id = id;
    return inv;
}

// Look up the slot's item handle and its type def. Returns true when
// the slot holds a valid item with a registered type.
static bool inventory_resolve_slot(const Hud::Impl& s,
                                   const simulation::Inventory* inv,
                                   u32 slot_index,
                                   simulation::Item& out_item,
                                   const simulation::ItemInfo*& out_info,
                                   const simulation::ItemTypeDef*& out_def) {
    out_item = {};
    out_info = nullptr;
    out_def  = nullptr;
    if (!inv || slot_index >= inv->slots.size()) return false;
    simulation::Item item = inv->slots[slot_index];
    if (!item.is_valid() || !s.world_ctx || !s.world_ctx->world) return false;
    const auto* info = s.world_ctx->world->item_infos.get(item.id);
    if (!info) return false;
    out_item = item;
    out_info = info;
    if (s.world_ctx->types) out_def = s.world_ctx->types->get_item_type(info->type_id);
    return true;
}

static void draw_inventory(Hud& hud, Hud::Impl& s) {
    const auto& cfg = s.inventory_cfg;
    if (!cfg.enabled || !s.inventory_rt.visible) return;

    // Bar frame always renders; slot frames always render too (so the
    // layout stays stable). Slot CONTENT (icon, charges, level badge)
    // is conditional — appears only when an item is in that slot —
    // and the existing `has_item` / `info` null-checks below already
    // handle empty-slot rendering naturally.
    if ((cfg.style.bg.rgba >> 24) != 0) hud.draw_rect(cfg.rect, cfg.style.bg);

    u32 carrier_id = UINT32_MAX;
    const simulation::Inventory* inv = inventory_resolve_selected(s, &carrier_id);

    for (u32 i = 0; i < cfg.slots.size(); ++i) {
        const auto& slot = cfg.slots[i];
        if (!slot.visible) continue;

        simulation::Item item;
        const simulation::ItemInfo*    info = nullptr;
        const simulation::ItemTypeDef* def  = nullptr;
        bool has_item = inventory_resolve_slot(s, inv, i, item, info, def);

        // Slot may be beyond the carrier's `inventory_size` (the unit
        // simply can't hold an item there). Render those with the
        // author-defined `unavailable_bg` and skip every interactive
        // state — no hover, no press, no hotkey, no badges.
        bool available = inv && i < inv->slots.size();

        // Slot background. Empty slots use a darker `empty_bg`; out-of-
        // range slots use `unavailable_bg`. Hover / press only fire on
        // slots that actually hold an item.
        Color bg;
        if (!available)        bg = slot.style.unavailable_bg;
        else if (has_item)     bg = slot.style.bg;
        else                   bg = slot.style.empty_bg;
        if (has_item && slot.pressed)      bg = slot.style.press_bg;
        else if (has_item && slot.hovered) bg = slot.style.hover_bg;
        hud.draw_rect(slot.rect, bg);

        f32 bw = (slot.style.border_width > 0.0f) ? slot.style.border_width : 0.0f;
        Rect icon_rect{ slot.rect.x + bw, slot.rect.y + bw,
                        slot.rect.w - bw * 2.0f, slot.rect.h - bw * 2.0f };
        // Source icon stays visible during hold — WC3 leaves the slot
        // icon in place and just shows a duplicate at the cursor, so
        // the player can see "this is the item I lifted" at a glance.
        if (def && !def->icon_path.empty()) {
            hud.draw_image(icon_rect, def->icon_path);
        }

        // Cooldown / disabled wash come from the item's first ability
        // (the "use" ability for active items). Look it up live from the
        // carrier's ability set — it lives there because give_item_to_unit
        // grants every item ability into the carrier on pickup.
        if (def && !def->abilities.empty() && carrier_id != UINT32_MAX
            && s.world_ctx && s.world_ctx->world && s.world_ctx->abilities) {
            const std::string& fa = def->abilities[0];
            const auto* aset = s.world_ctx->world->ability_sets.get(carrier_id);
            const simulation::AbilityInstance* inst = nullptr;
            if (aset) {
                for (const auto& a : aset->abilities) {
                    if (a.ability_id == fa) { inst = &a; break; }
                }
            }
            const auto* abil_def = s.world_ctx->abilities->get(fa);
            if (inst && abil_def && is_castable_form(abil_def->form)) {
                bool on_cooldown = inst->cooldown_remaining > 0.05f
                                && abil_def->level_data(inst->level).cooldown > 0.0f;
                if (on_cooldown) {
                    f32 total = abil_def->level_data(inst->level).cooldown;
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
                } else if (!can_afford(*s.world_ctx->world, carrier_id,
                                       abil_def->level_data(inst->level))) {
                    hud.draw_rect(icon_rect, cfg.style.disabled_tint);
                }
            }
        }

        // Border.
        if (bw > 0.0f && (slot.style.border_color.rgba >> 24) != 0) {
            Rect r = slot.rect;
            hud.draw_rect({ r.x, r.y, r.w, bw }, slot.style.border_color);
            hud.draw_rect({ r.x, r.y + r.h - bw, r.w, bw }, slot.style.border_color);
            hud.draw_rect({ r.x, r.y, bw, r.h }, slot.style.border_color);
            hud.draw_rect({ r.x + r.w - bw, r.y, bw, r.h }, slot.style.border_color);
        }

        // Hotkey badge — top-right. Only shown when the slot holds an
        // item; an empty (or unavailable) slot has nothing to fire, so
        // the keybind label would just be noise.
        if (has_item && !slot.hotkey.empty()) {
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

        // Charges badge — bottom-right, drawn only when value > 0.
        // The hotkey already owns top-right; bottom-right is the clear
        // corner. Pill-on-glyph keeps it readable on bright icons.
        if (info && info->charges > 0) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", info->charges);
            f32 px = cfg.style.charges_text_size;
            f32 tw = hud.text_width_px(buf, px);
            f32 ascent = hud.text_ascent_px(px);
            f32 x_left = slot.rect.x + slot.rect.w - tw - 4.0f;
            f32 y_base = slot.rect.y + slot.rect.h - 4.0f;
            if ((cfg.style.charges_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f, pad_y = 1.0f;
                Rect bg_pill{
                    x_left - pad_x, y_base - ascent - pad_y,
                    tw + pad_x * 2.0f, ascent + pad_y * 2.0f,
                };
                hud.draw_rect(bg_pill, cfg.style.charges_badge_bg);
            }
            hud.draw_text(x_left, y_base, buf, cfg.style.charges_color, px);
        }

        // Level badge — top-left. Same render rule as charges, just a
        // different corner so a pile-of-charges-and-levels item shows
        // both without overlap.
        if (info && info->level > 0) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d", info->level);
            f32 px = cfg.style.level_text_size;
            f32 tw = hud.text_width_px(buf, px);
            f32 ascent = hud.text_ascent_px(px);
            f32 x_left = slot.rect.x + 4.0f;
            f32 y_base = slot.rect.y + ascent + 2.0f;
            if ((cfg.style.level_badge_bg.rgba >> 24) != 0) {
                f32 pad_x = 3.0f, pad_y = 1.0f;
                Rect bg_pill{
                    x_left - pad_x, y_base - ascent - pad_y,
                    tw + pad_x * 2.0f, ascent + pad_y * 2.0f,
                };
                hud.draw_rect(bg_pill, cfg.style.level_badge_bg);
            }
            hud.draw_text(x_left, y_base, buf, cfg.style.level_color, px);
        }
    }
}

static void draw_action_bar(Hud& hud, Hud::Impl& s) {
    const auto& cfg = s.action_bar_cfg;
    if (!cfg.enabled) return;
    if (!s.action_bar_rt.visible) return;

    // Bar background — always renders when enabled+visible. Bar is just
    // a visual container; slot CONTENT (icon, hotkey, cooldown) is the
    // conditional part. When there's no selection, resolve_slot_ability
    // returns nullptr and the per-slot render naturally skips icon /
    // hotkey / cooldown — what's left is the empty slot frame.
    if ((cfg.style.bg.rgba >> 24) != 0) {
        hud.draw_rect(cfg.rect, cfg.style.bg);
    }

    switch (cfg.style_id) {
        case ActionBarStyleId::ClassicRts:
            draw_action_bar_classic_rts(hud, s);
            break;
        case ActionBarStyleId::Moba:
            draw_action_bar_moba(hud, s);
            break;
    }

    // Cancel zone draws on top of everything else in the bar's render
    // pass — finger lands on it = visible target during the drag.
    draw_action_bar_cancel_zone(hud, s);
}

// ── Tooltip ──────────────────────────────────────────────────────────────
// Returns {name_key, body_key} for the currently-dwelled slot. Keys route
// through the i18n raw-fallback chain — `ability.<id>.name` /
// `.tooltip`, `item.<id>.name` / `.tooltip`, `ui.command.<id>.name` /
// `.tooltip`. Maps localize them via `<map>/strings/<locale>/*.json`;
// ability + item names already fall back to `display_name` / `name` in
// the type def, so even un-localized maps show readable names. Bodies
// without a JSON or locale entry render as the key literal so authors
// can see what's missing.
static void tooltip_keys(const Hud::Impl& s,
                         i18n::LocalizedString& out_name,
                         i18n::LocalizedString& out_body) {
    out_name.clear();
    out_body.clear();
    switch (s.tooltip.source) {
        case Hud::Impl::TooltipState::Source::ActionBar: {
            if (!s.world_ctx) return;
            const simulation::AbilityDef* def = nullptr;
            resolve_slot_ability(static_cast<u32>(s.tooltip.slot_index),
                                 s.action_bar_cfg, *s.world_ctx, def);
            if (!def) return;
            out_name.key = "ability." + def->id + ".name";
            out_body.key = "ability." + def->id + ".tooltip";
            break;
        }
        case Hud::Impl::TooltipState::Source::Inventory: {
            const simulation::Inventory* inv = inventory_resolve_selected(s);
            simulation::Item item;
            const simulation::ItemInfo*    info = nullptr;
            const simulation::ItemTypeDef* def  = nullptr;
            if (!inventory_resolve_slot(s, inv,
                                        static_cast<u32>(s.tooltip.slot_index),
                                        item, info, def)) return;
            if (!def) return;
            out_name.key = "item." + def->id + ".name";
            out_body.key = "item." + def->id + ".tooltip";
            break;
        }
        case Hud::Impl::TooltipState::Source::CommandBar: {
            const auto& slots = s.command_bar_cfg.slots;
            u32 idx = static_cast<u32>(s.tooltip.slot_index);
            if (idx >= slots.size()) return;
            const std::string& cmd = slots[idx].command;
            if (cmd.empty()) return;
            out_name.key = "ui.command." + cmd + ".name";
            out_body.key = "ui.command." + cmd + ".tooltip";
            break;
        }
        default: break;
    }
}

// Greedy word-wrap. Splits on ASCII spaces and explicit '\n'. Words wider
// than `max_w` are broken between codepoints. UTF-8 multibyte sequences
// are walked as units so we never split mid-codepoint. Sufficient for the
// short, prose-like ability / item tooltips authors write; languages
// without spaces (CJK) end up on one line unless authored with line
// breaks, which is acceptable for v1.
static std::vector<std::string> tooltip_wrap(const Hud& hud,
                                             const std::string& text,
                                             f32 px_size, f32 max_w) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    size_t i = 0;
    while (i < text.size()) {
        size_t line_end = text.find('\n', i);
        std::string segment = (line_end == std::string::npos)
                                ? text.substr(i)
                                : text.substr(i, line_end - i);
        i = (line_end == std::string::npos) ? text.size() : (line_end + 1);

        // Wrap this segment.
        std::string current;
        size_t k = 0;
        while (k < segment.size()) {
            // Read one word (run of non-space) into `word`.
            size_t ws = k;
            while (ws < segment.size() && segment[ws] == ' ') ++ws;
            std::string spaces = segment.substr(k, ws - k);
            size_t we = ws;
            while (we < segment.size() && segment[we] != ' ') ++we;
            std::string word = segment.substr(ws, we - ws);
            k = we;

            std::string candidate = current + spaces + word;
            f32 cw = hud.text_width_px(candidate, px_size);
            if (cw <= max_w || current.empty()) {
                current = std::move(candidate);
            } else {
                out.push_back(std::move(current));
                current = word;     // spaces drop at line break
            }
            // If `current` itself is wider than max_w (a single very
            // long word, or CJK with no spaces), break by codepoints.
            if (hud.text_width_px(current, px_size) > max_w) {
                std::string acc;
                size_t cp = 0;
                while (cp < current.size()) {
                    unsigned char c = static_cast<unsigned char>(current[cp]);
                    size_t n = 1;
                    if      ((c & 0x80) == 0x00) n = 1;
                    else if ((c & 0xE0) == 0xC0) n = 2;
                    else if ((c & 0xF0) == 0xE0) n = 3;
                    else if ((c & 0xF8) == 0xF0) n = 4;
                    if (cp + n > current.size()) n = current.size() - cp;
                    std::string trial = acc + current.substr(cp, n);
                    if (hud.text_width_px(trial, px_size) > max_w && !acc.empty()) {
                        out.push_back(std::move(acc));
                        acc.clear();
                    }
                    acc += current.substr(cp, n);
                    cp  += n;
                }
                current = std::move(acc);
            }
        }
        out.push_back(std::move(current));
    }
    return out;
}

static void draw_tooltip(Hud& hud, Hud::Impl& s) {
    using TT = Hud::Impl::TooltipState;
    if (s.tooltip.source == TT::Source::None) return;
    auto now = std::chrono::steady_clock::now();
    if (now < s.tooltip.activate_at) return;
    s.tooltip.visible = true;

    // Anchor rect.
    Rect anchor{};
    bool have_anchor = false;
    u32 idx = static_cast<u32>(s.tooltip.slot_index);
    if (s.tooltip.source == TT::Source::ActionBar
        && idx < s.action_bar_cfg.slots.size()) {
        anchor = s.action_bar_cfg.slots[idx].rect;
        have_anchor = true;
    } else if (s.tooltip.source == TT::Source::Inventory
               && idx < s.inventory_cfg.slots.size()) {
        anchor = s.inventory_cfg.slots[idx].rect;
        have_anchor = true;
    } else if (s.tooltip.source == TT::Source::CommandBar
               && idx < s.command_bar_cfg.slots.size()) {
        anchor = s.command_bar_cfg.slots[idx].rect;
        have_anchor = true;
    }
    if (!have_anchor) return;

    auto resolve = [&](const i18n::LocalizedString& l) -> std::string {
        if (l.empty()) return {};
        if (!s.locale_manager) return l.key;
        return s.locale_manager->resolve(i18n::Pool::Map, l);
    };
    i18n::LocalizedString name_loc, body_loc;
    tooltip_keys(s, name_loc, body_loc);

    constexpr f32 PAD     = 8.0f;
    constexpr f32 GAP     = 4.0f;
    constexpr f32 MAX_W   = 280.0f;
    constexpr f32 NAME_PX = 15.0f;
    constexpr f32 BODY_PX = 12.0f;

    // Flatten the tooltip into a uniform line list. Empty resolutions
    // (e.g. unauthored body) contribute zero entries, so the layout
    // math below has no special cases — drawing happens iff there's
    // anything to draw.
    struct Line { std::string text; f32 px; Color color; };
    std::vector<Line> lines;
    if (auto t = resolve(name_loc); !t.empty()) {
        lines.push_back({ std::move(t), NAME_PX, rgba(255, 230, 150, 255) });
    }
    for (auto& wrapped : tooltip_wrap(hud, resolve(body_loc), BODY_PX, MAX_W - PAD * 2.0f)) {
        lines.push_back({ std::move(wrapped), BODY_PX, rgba(220, 220, 220, 255) });
    }
    if (lines.empty()) return;

    // Walk lines once to size the panel. Vertical GAP appears wherever
    // adjacent lines use a different size — natural visual break
    // between the name (NAME_PX) and the first body line (BODY_PX).
    f32 content_w = 0.0f;
    f32 panel_h   = PAD * 2.0f;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0 && lines[i - 1].px != lines[i].px) panel_h += GAP;
        panel_h   += hud.text_line_height_px(lines[i].px);
        content_w  = std::max(content_w, hud.text_width_px(lines[i].text, lines[i].px));
    }
    f32 panel_w = std::min(MAX_W, content_w + PAD * 2.0f);

    // Position. Above slot by default; flip below if it would clip
    // the top edge. Clamp horizontally so the panel stays on-screen.
    f32 px = anchor.x + anchor.w * 0.5f - panel_w * 0.5f;
    f32 py = anchor.y - panel_h - 8.0f;
    if (py < 4.0f) py = anchor.y + anchor.h + 8.0f;
    f32 sw = static_cast<f32>(s.screen_w);
    if (px < 4.0f) px = 4.0f;
    if (px + panel_w > sw - 4.0f) px = sw - 4.0f - panel_w;

    Color bg     = rgba(20, 22, 30, 235);
    Color border = rgba(255, 255, 255, 90);
    hud.draw_rect({ px, py, panel_w, panel_h }, bg);
    hud.draw_rect({ px, py, panel_w, 1.0f }, border);
    hud.draw_rect({ px, py + panel_h - 1.0f, panel_w, 1.0f }, border);
    hud.draw_rect({ px, py, 1.0f, panel_h }, border);
    hud.draw_rect({ px + panel_w - 1.0f, py, 1.0f, panel_h }, border);

    f32 cursor_y = py + PAD;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0 && lines[i - 1].px != lines[i].px) cursor_y += GAP;
        f32 baseline = cursor_y + hud.text_ascent_px(lines[i].px);
        hud.draw_text(px + PAD, baseline, lines[i].text, lines[i].color, lines[i].px);
        cursor_y += hud.text_line_height_px(lines[i].px);
    }
}

void Hud::draw_tree() {
    if (!m_impl || !m_impl->frame_open || !m_impl->root) return;
    m_impl->root->draw(*this);
    draw_action_bar(*this, *m_impl);
    draw_command_bar(*this, *m_impl);
    draw_inventory(*this, *m_impl);
    draw_minimap(*this, *m_impl);
    draw_joystick(*this, *m_impl);
    // HUD cursor — desktop only. Mobile has no pointer to swap, so
    // cursor textures are skipped entirely; the snap-target indicator
    // (3D ground decal) is what tells the player where the cast will
    // land. Drawn only when the map authored a texture for the
    // current state. Tint comes from the hover-intent palette so a
    // unit under the pointer signals ally / enemy / item directly on
    // the cursor.
    if (!m_impl->is_mobile) {
        const auto& s = m_impl->cast_indicator_cfg.style;
        bool targeting = aim_state().active;
        const std::string& path = targeting ? s.cursor_target_path
                                            : s.cursor_default_path;
        if (!path.empty()) {
            f32 size = (s.cursor_size > 0.0f) ? s.cursor_size : 20.0f;
            // Target cursor is centered (reticle); default cursors are
            // expected to be authored top-left-anchored (arrow tip at
            // the texture's (0,0)).
            f32 ax = targeting ? size * 0.5f : 0.0f;
            f32 ay = targeting ? size * 0.5f : 0.0f;
            f32 cx = m_impl->pointer_x;
            f32 cy = m_impl->pointer_y;
            Rect r{ cx - ax, cy - ay, size, size };
            Color tint = s.intents.neutral;
            switch (cursor_intent()) {
                case TargetingIntent::Enemy: tint = s.intents.enemy; break;
                case TargetingIntent::Ally:  tint = s.intents.ally;  break;
                case TargetingIntent::Item:  tint = s.intents.item;  break;
                default:                     tint = s.intents.neutral; break;
            }
            draw_image(r, path, tint);
        }
    }

    // Held-item icon (WC3 lift). Layered on top of the always-on
    // cursor so the player can see both the system-style pointer
    // and the lifted item.
    if (m_impl->held_item_slot >= 0 && !m_impl->held_item_icon.empty()) {
        constexpr f32 kHeldIconSize = 28.0f;
        f32 cx = m_impl->pointer_x;
        f32 cy = m_impl->pointer_y;
        Rect r{ cx - kHeldIconSize * 0.5f, cy - kHeldIconSize * 0.5f,
                kHeldIconSize, kHeldIconSize };
        draw_image(r, m_impl->held_item_icon, rgba(255, 255, 255, 220));
    }

    // Tooltip — drawn last so it overlays everything except the held-item
    // icon (which the user is actively dragging and must stay legible).
    // Skipped when an item is held; the lift gesture already replaces
    // hover semantics.
    if (m_impl->held_item_slot < 0) {
        draw_tooltip(*this, *m_impl);
    }
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

    // Multi-touch lift fixup: when the gesture-owning finger lifts and
    // another finger (typically the joystick) is still down, the app's
    // pointer routing falls back to mouse_x — which now reflects the
    // wrong finger. The press's last known position is in s.pointer_x
    // / s.pointer_y from the previous frame; on a release edge we keep
    // those instead of overwriting with the new (wrong) coords. Hit
    // tests below then resolve to the slot the press actually ended on.
    if (!button_down && s.pointer_down_prev) {
        x = s.pointer_x;
        y = s.pointer_y;
    } else {
        s.pointer_x = x;
        s.pointer_y = y;
    }

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
    i32  inv_slot    = (bar_slot < 0 && cmd_slot < 0) ? inventory_hit_test(s, x, y) : -1;
    bool on_minimap  = (bar_slot < 0) && (cmd_slot < 0) && (inv_slot < 0) && minimap_hit_test(s, x, y);
    bool on_joystick = (bar_slot < 0) && (cmd_slot < 0) && (inv_slot < 0) && !on_minimap
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
    if (inv_slot != s.inventory_hover_slot) {
        if (s.inventory_hover_slot >= 0 &&
            static_cast<u32>(s.inventory_hover_slot) < s.inventory_cfg.slots.size()) {
            s.inventory_cfg.slots[s.inventory_hover_slot].hovered = false;
        }
        if (inv_slot >= 0) {
            s.inventory_cfg.slots[inv_slot].hovered = true;
        }
        s.inventory_hover_slot = inv_slot;
    }

    // Tooltip arming. The pointer dwelling on a slot for `delay_ms`
    // pops the tooltip; any change (different slot / left every slot)
    // resets the timer. Desktop uses 250ms — snappy hover. Mobile uses
    // 500ms — AoV-like long-press (handle_pointer is called every frame
    // with the last known position, so dwelling means stationary press;
    // sliding past the slot edges fails the hit-test and naturally
    // clears the timer).
    Impl::TooltipState::Source kind = Impl::TooltipState::Source::None;
    i32 kind_idx = -1;
    if      (bar_slot >= 0) { kind = Impl::TooltipState::Source::ActionBar;  kind_idx = bar_slot; }
    else if (cmd_slot >= 0) { kind = Impl::TooltipState::Source::CommandBar; kind_idx = cmd_slot; }
    else if (inv_slot >= 0) { kind = Impl::TooltipState::Source::Inventory;  kind_idx = inv_slot; }
    if (kind != s.tooltip.source || kind_idx != s.tooltip.slot_index) {
        s.tooltip.source     = kind;
        s.tooltip.slot_index = kind_idx;
        s.tooltip.visible    = false;
        if (kind != Impl::TooltipState::Source::None) {
            int delay_ms = s.is_mobile ? 500 : 250;
            s.tooltip.activate_at = std::chrono::steady_clock::now()
                                  + std::chrono::milliseconds(delay_ms);
        }
    }

    Node* under = nullptr;
    if (bar_slot < 0 && cmd_slot < 0 && inv_slot < 0 && !on_minimap && !on_joystick) {
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
        // Holding an item (WC3 lift): the next left-click commits.
        // On another inventory slot → swap. On terrain → drop at the
        // clicked world point. Anywhere else → cancel.
        if (s.held_item_slot >= 0) {
            i32 target = inv_slot;
            if (target >= 0 && target != s.held_item_slot) {
                if (s.inventory_swap_fn) {
                    s.inventory_swap_fn(s.held_item_slot, target);
                }
            } else if (target < 0 && s.world_ctx && s.world_ctx->picker
                       && s.inventory_drop_fn) {
                glm::vec3 wp;
                // Picker takes physical pixels; convert dp → physical.
                f32 sx = x * s.ui_scale;
                f32 sy = y * s.ui_scale;
                if (s.world_ctx->picker->screen_to_world(sx, sy, wp)) {
                    s.inventory_drop_fn(s.held_item_id,
                                        s.held_item_slot, wp);
                }
            }
            s.held_item_slot = -1;
            s.held_item_id   = UINT32_MAX;
            s.held_item_icon.clear();
            s.pointer_down_prev = button_down;
            return;
        }
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
                bool targetable = def && def->form == simulation::AbilityForm::Target;
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
                        s.drag_cast.widget_kinds = def->widget_kinds;
                        s.drag_cast.accept_point = def->accept_point;
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
            // Mobile: command_bar slots use the same drag-cast machine
            // as ability slots when the command targets a world point
            // (Move / Attack / AttackMove). Press to grab, drag-aim,
            // release to commit. Stop / HoldPosition stay click-to-fire
            // — they don't take a target. Desktop falls through to the
            // press/release click path below.
            bool drag_cast_started = false;
            if (s.is_mobile && s.world_ctx && s.command_bar_drag_commit_fn) {
                const auto& slot = s.command_bar_cfg.slots[cmd_slot];
                bool targetable = (slot.command == "move"
                                || slot.command == "attack"
                                || slot.command == "attack_move");
                u32 caster_id = s.world_ctx->selection &&
                                !s.world_ctx->selection->selected().empty()
                                  ? s.world_ctx->selection->selected().front().id
                                  : UINT32_MAX;
                if (targetable && caster_id != UINT32_MAX) {
                    auto* tf = s.world_ctx->world->transforms.get(caster_id);
                    auto* hi = s.world_ctx->world->handle_infos.get(caster_id);
                    if (tf && hi) {
                        s.drag_cast.phase       = Hud::Impl::DragCastPhase::Pressed;
                        s.drag_cast.slot_index  = cmd_slot;
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
                        s.drag_cast.ability_id.clear();
                        s.drag_cast.command_id  = slot.command;
                        s.drag_cast.range       = 0;
                        // Both Move and Attack snap to units on mobile.
                        // Move-on-unit → Follow; Attack-on-unit → Attack
                        // (friendly fire allowed); release on ground
                        // falls back to the point-target order. Use the
                        // hybrid form (widget-accepting + point fall-
                        // through) so the snap loop runs and release on
                        // ground commits the point order.
                        s.drag_cast.form        = simulation::AbilityForm::Target;
                        s.drag_cast.widget_kinds = simulation::widget_kind::Unit;
                        s.drag_cast.accept_point = true;
                        s.drag_cast.shape       = simulation::IndicatorShape::Point;
                        s.drag_cast.area_radius = 0;
                        s.drag_cast.area_width  = 0;
                        s.drag_cast.area_angle  = 0;
                        s.drag_cast.snapped_target = simulation::Unit{};
                        s.command_bar_cfg.slots[cmd_slot].pressed = true;
                        s.command_bar_cfg.slots[cmd_slot].press_pulse_until =
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
                        drag_cast_started = true;
                    }
                }
            }
            if (!drag_cast_started) {
                s.command_bar_pressed_slot = cmd_slot;
                s.command_bar_cfg.slots[cmd_slot].pressed = true;
                s.command_bar_cfg.slots[cmd_slot].press_pulse_until =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
            }
        } else if (inv_slot >= 0) {
            // Mobile: capture into drag_cast so we can run the
            // long-press → lift gesture (matches desktop right-click)
            // and the drag-out → cast-at-target gesture (matches the
            // ability bar). The two are decided by what happens during
            // the press: stationary 500ms wins long-press; drag past
            // the slot rect wins drag-cast. A quick tap (release while
            // still over the slot, before either threshold) falls
            // through to the no-target use callback. Desktop keeps
            // the existing pressed-slot tap-to-use path because it
            // already has right-click for lift and no drag UX.
            bool drag_cast_started = false;
            if (s.is_mobile && s.world_ctx && s.world_ctx->world &&
                s.world_ctx->abilities) {
                u32 carrier_id = UINT32_MAX;
                const simulation::Inventory* inv_data =
                    inventory_resolve_selected(s, &carrier_id);
                simulation::Item item;
                const simulation::ItemInfo*    info = nullptr;
                const simulation::ItemTypeDef* tdef = nullptr;
                if (carrier_id != UINT32_MAX &&
                    inventory_resolve_slot(s, inv_data,
                                           static_cast<u32>(inv_slot),
                                           item, info, tdef)) {
                    // Snapshot the item's first ability so a drag-out
                    // commits with the same range/form/area the press
                    // saw — handle_pointer is the only place we look
                    // these up; action_bar_drag_update reads from the
                    // drag_cast snapshot from here on out.
                    std::string first_ability;
                    f32  range = 0.0f;
                    auto form  = simulation::AbilityForm::PassiveModifier;
                    auto shape = simulation::IndicatorShape::Point;
                    u32  widget_kinds = 0;
                    bool accept_point = false;
                    f32  area_radius = 0, area_width = 0, area_angle = 0;
                    bool castable_now = false;
                    if (tdef && !tdef->abilities.empty()) {
                        first_ability = tdef->abilities[0];
                        const auto* abil_def =
                            s.world_ctx->abilities->get(first_ability);
                        const auto* aset =
                            s.world_ctx->world->ability_sets.get(carrier_id);
                        const simulation::AbilityInstance* inst = nullptr;
                        if (aset) {
                            for (const auto& a : aset->abilities) {
                                if (a.ability_id == first_ability) { inst = &a; break; }
                            }
                        }
                        if (abil_def && inst) {
                            const auto& lvl = abil_def->level_data(inst->level);
                            range        = lvl.range;
                            form         = abil_def->form;
                            widget_kinds = abil_def->widget_kinds;
                            accept_point = abil_def->accept_point;
                            shape        = abil_def->shape;
                            area_radius  = lvl.area.radius;
                            area_width   = lvl.area.width;
                            area_angle   = lvl.area.angle;
                            castable_now = is_castable_form(abil_def->form) &&
                                slot_castable_now(*s.world_ctx, carrier_id,
                                                  *inst, *abil_def);
                        }
                    }
                    bool targetable = castable_now && form == simulation::AbilityForm::Target;
                    auto* tf = s.world_ctx->world->transforms.get(carrier_id);
                    auto* hi = s.world_ctx->world->handle_infos.get(carrier_id);
                    if (tf && hi) {
                        s.drag_cast.phase       = Hud::Impl::DragCastPhase::Pressed;
                        s.drag_cast.slot_index  = -1;
                        s.drag_cast.press_x     = x;
                        s.drag_cast.press_y     = y;
                        s.drag_cast.current_x   = x;
                        s.drag_cast.current_y   = y;
                        s.drag_cast.caster.id         = carrier_id;
                        s.drag_cast.caster.generation = hi->generation;
                        s.drag_cast.caster_x    = tf->position.x;
                        s.drag_cast.caster_y    = tf->position.y;
                        s.drag_cast.caster_z    = tf->position.z;
                        s.drag_cast.drag_world_x = tf->position.x;
                        s.drag_cast.drag_world_y = tf->position.y;
                        s.drag_cast.drag_world_z = tf->position.z;
                        s.drag_cast.ability_id  = first_ability;
                        s.drag_cast.range       = range;
                        s.drag_cast.form        = form;
                        s.drag_cast.widget_kinds = widget_kinds;
                        s.drag_cast.accept_point = accept_point;
                        s.drag_cast.shape       = shape;
                        s.drag_cast.area_radius = area_radius;
                        s.drag_cast.area_width  = area_width;
                        s.drag_cast.area_angle  = area_angle;
                        s.drag_cast.snapped_target  = simulation::Unit{};
                        s.drag_cast.command_id.clear();
                        s.drag_cast.inventory_slot      = inv_slot;
                        s.drag_cast.inventory_item_id   = item.id;
                        s.drag_cast.inventory_item_icon = (tdef ? tdef->icon_path : std::string{});
                        s.drag_cast.inventory_targetable = targetable;
                        s.drag_cast.press_time  = std::chrono::steady_clock::now();
                        s.inventory_cfg.slots[inv_slot].pressed = true;
                        drag_cast_started = true;
                    }
                }
            }
            if (!drag_cast_started) {
                s.inventory_pressed_slot = inv_slot;
                s.inventory_cfg.slots[inv_slot].pressed = true;
            }
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
            // pressed. The lift-fixup at the top of handle_pointer
            // restores the press's last-known coords on this release
            // frame, so bar_slot resolves correctly even when another
            // finger (joystick) was the only thing left on screen.
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
        } else if (s.inventory_pressed_slot >= 0) {
            // Click on an inventory slot → fire the slot's first ability
            // through the use callback, with the item handle attached so
            // triggers reading GetTriggerItem() resolve to this item.
            // Passive items (`abilities[0].form == passive`) are filtered
            // here and don't fire — same behavior as a passive ability
            // landing in the action_bar.
            u32 idx = static_cast<u32>(s.inventory_pressed_slot);
            bool over = (inv_slot == s.inventory_pressed_slot);
            if (idx < s.inventory_cfg.slots.size()) {
                auto& slot = s.inventory_cfg.slots[idx];
                slot.pressed = false;
                if (over && s.inventory_use_fn && s.world_ctx
                    && s.world_ctx->world && s.world_ctx->abilities) {
                    u32 carrier_id = UINT32_MAX;
                    const simulation::Inventory* inv_data =
                        inventory_resolve_selected(s, &carrier_id);
                    simulation::Item item;
                    const simulation::ItemInfo*    info = nullptr;
                    const simulation::ItemTypeDef* def  = nullptr;
                    if (inventory_resolve_slot(s, inv_data, idx, item, info, def)
                        && def && !def->abilities.empty()) {
                        const std::string& fa = def->abilities[0];
                        const auto* abil_def = s.world_ctx->abilities->get(fa);
                        if (abil_def && is_castable_form(abil_def->form)) {
                            const auto* aset = s.world_ctx->world->ability_sets.get(carrier_id);
                            const simulation::AbilityInstance* inst = nullptr;
                            if (aset) {
                                for (const auto& a : aset->abilities) {
                                    if (a.ability_id == fa) { inst = &a; break; }
                                }
                            }
                            if (inst && slot_castable_now(*s.world_ctx, carrier_id, *inst, *abil_def)) {
                                s.inventory_use_fn(item.id, fa);
                            }
                        }
                    }
                }
            }
            s.inventory_pressed_slot = -1;
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
        || m_impl->inventory_hover_slot     >= 0
        || m_impl->inventory_pressed_slot   >= 0
        || m_impl->held_item_slot           >= 0   // hold mode owns the next click
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

void Hud::set_label_text(std::string_view id, i18n::LocalizedString text) {
    if (!m_impl) return;
    auto* n = find_node_by_id(id);
    if (!n) return;
    if (auto* l = dynamic_cast<hud::Label*>(n)) {
        emit_sync(*m_impl,
                  uldum::network::build_hud_set_label_text(id, text.key, text.args),
                  n->players_mask);
        l->text = std::move(text);
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
                  n->players_mask);
    }
}

void Hud::set_node_visible(std::string_view id, bool visible) {
    if (!m_impl) return;
    auto* n = find_node_by_id(id);
    if (!n) return;
    n->visible = visible;
    emit_sync(*m_impl,
              uldum::network::build_hud_set_node_visible(id, visible),
              n->players_mask);
}

void Hud::set_image_source(std::string_view id, std::string_view source) {
    if (!m_impl) return;
    auto* n = find_node_by_id(id);
    if (!n) return;
    if (auto* im = dynamic_cast<hud::Image*>(n)) {
        im->source.assign(source);
        emit_sync(*m_impl,
                  uldum::network::build_hud_set_image_source(id, source),
                  n->players_mask);
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
                  n->players_mask);
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

void Hud::draw_image_disc(f32 cx, f32 cy, f32 radius,
                          std::string_view asset_path, Color tint) {
    if (!m_impl || !m_impl->frame_open) return;
    if (radius <= 0.0f) return;
    HudImage* img = get_or_load_image(*m_impl, asset_path);
    if (!img) return;
    ensure_batch(*m_impl, PIPE_SOLID, img->set);
    u32 premul = premul_rgba(tint);

    // Triangle fan from disc center to perimeter. UVs map disc center
    // to the texture's center (0.5, 0.5) and the disc's perimeter to
    // the texture's inscribed circle. Effect: the disc renders the
    // icon scaled so its inscribed circle covers the disc, with the
    // texture's corners (outside the inscribed circle) clipped — so a
    // square icon fills a round button cleanly, no square edges past
    // the rim.
    constexpr u32 kSegments = 32;
    constexpr f32 TWO_PI = 6.2831853f;
    f32 step = TWO_PI / static_cast<f32>(kSegments);
    for (u32 i = 0; i < kSegments; ++i) {
        f32 a0 = step * static_cast<f32>(i);
        f32 a1 = step * static_cast<f32>(i + 1);
        f32 c0 = std::cos(a0), s0 = std::sin(a0);
        f32 c1 = std::cos(a1), s1 = std::sin(a1);
        append_triangle_uv(
            *m_impl,
            cx,                   cy,                   0.5f,            0.5f,
            cx + c0 * radius,     cy + s0 * radius,     0.5f + c0 * 0.5f, 0.5f + s0 * 0.5f,
            cx + c1 * radius,     cy + s1 * radius,     0.5f + c1 * 0.5f, 0.5f + s1 * 0.5f,
            premul);
    }
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
