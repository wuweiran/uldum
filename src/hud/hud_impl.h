#pragma once

// Internal header — shared by `hud.cpp` (state-side methods) and
// `src/render/hud/hud_renderer.cpp` (render-side methods). Lets the
// two translation units both see `Hud::Impl` without exposing it in
// the public hud.h.
//
// Not a public API — never include from outside src/hud/ and
// src/render/hud/. After Phase 23 step 4b, Vulkan-tied fields live on
// `HudRenderer::Impl` (declared inside hud_renderer.cpp); this struct
// holds the pure-data side that App / Lua / network mutate.

#include "hud/hud.h"
#include "hud/node.h"
#include "hud/action_bar.h"
#include "hud/minimap.h"
#include "hud/command_bar.h"
#include "hud/joystick.h"
#include "hud/cast_indicator.h"
#include "hud/inventory.h"
#include "hud/pickup_bar.h"
#include "hud/display_message.h"
#include "render/hud/world.h"           // WorldOverlayConfig (POD); WorldContext fwd
#include "simulation/handle_types.h"
#include "simulation/ability_def.h"    // AbilityForm, IndicatorShape

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace uldum::hud {

struct WorldContext;   // forward — defined in render/hud/world.h

struct Hud::Impl {
    // `screen_w/h` = logical (dp) HUD dimensions — what author-facing
    // coordinates live in. Computed each frame as physical / ui_scale.
    // `physical_w/h` = raw framebuffer extent (pushed by the renderer).
    // `ui_scale` = physical pixels per dp, set from the platform layer.
    u32 screen_w = 0;
    u32 screen_h = 0;
    u32 physical_w = 0;
    u32 physical_h = 0;
    f32 ui_scale = 1.0f;
    bool is_mobile = false;
    Hud::SafeInsets safe_insets{};

    // Retained widget tree. Root Panel is always present; its rect is
    // updated to the current viewport each frame so percentage-style
    // positioning (future) can anchor to the full window.
    std::unique_ptr<Panel> root;

    // World UI context — set at session start, cleared at session end.
    // Carries refs into the active sim, camera, picker, selection,
    // terrain.
    const WorldContext* world_ctx = nullptr;

    // World UI config (entity bars + name labels). Pure data, set at
    // map load from hud.json's `world_overlays` block. HudRenderer
    // reads it via friend access during draw_world_overlays.
    WorldOverlayConfig world_cfg;

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
        // Caster position frozen at press. When world_anchored (RTS
        // preset), the drag point is derived from this fixed origin so
        // the destination stays on the world spot the player aimed at,
        // even as the caster walks. Action preset leaves world_anchored
        // false → the drag point follows the live caster (hero-centric).
        f32          press_caster_x = 0, press_caster_y = 0, press_caster_z = 0;
        bool         world_anchored = false;
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
        // with command_id and the regular ability slot path.
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
    // True between press-on-minimap and the matching release.
    bool minimap_dragging = false;

    // Command-bar composite.
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

    // Inventory composite.
    InventoryConfig  inventory_cfg{};
    InventoryRuntime inventory_rt{};

    // Display-message composite.
    DisplayMessageConfig  display_message_cfg{};
    DisplayMessageRuntime display_message_rt{};
    i32 inventory_hover_slot   = -1;
    i32 inventory_pressed_slot = -1;
    Hud::InventoryUseFn          inventory_use_fn;
    Hud::InventoryUseAtTargetFn  inventory_use_at_target_fn;
    Hud::InventoryDropFn         inventory_drop_fn;
    Hud::InventorySwapFn         inventory_swap_fn;

    PickupBarConfig  pickup_bar_cfg{};
    PickupBarRuntime pickup_bar_rt{};
    i32 pickup_bar_hover_slot   = -1;
    i32 pickup_bar_pressed_slot = -1;
    Hud::PickupFn pickup_fn;

    // WC3-style item hold (desktop). Right-click on a slot lifts the
    // item: `held_item_slot` is the source slot, `held_item_id` is
    // the item being held (snapshotted so we can render its icon at
    // the cursor even if the underlying inventory shifts), and
    // `held_item_icon` caches the icon path.
    i32 held_item_slot = -1;
    u32 held_item_id   = UINT32_MAX;
    std::string held_item_icon;

    // Cast indicator style — applied to range ring, arrow, reticle,
    // AoE indicator, target-unit ring, and per-phase tints.
    CastIndicatorConfig cast_indicator_cfg{};

    // Hover / long-press tooltip for action_bar, inventory, command_bar.
    struct TooltipState {
        enum class Source : u8 { None, ActionBar, Inventory, CommandBar, PickupBar };
        Source source = Source::None;
        i32    slot_index = -1;
        std::chrono::steady_clock::time_point activate_at{};
        bool   visible = false;
    };
    TooltipState tooltip{};

    // Rising-edge tracking for hidden-ability hotkeys.
    std::unordered_map<std::string, bool> hidden_hotkey_prev;

    // Box-select marquee — per-map style, authored in hud.json.
    Hud::MarqueeStyle marquee_style{};

    // Network sync + input-event callbacks (host-side wiring).
    Hud::SyncFn         sync_fn;
    Hud::ButtonEventFn  button_event_fn;

    // Local player slot (UINT32_MAX = dedicated server, never used to render).
    u32 local_player = UINT32_MAX;

    // Client-side i18n resolver. Used at render time to resolve
    // LocalizedString payloads in the local player's locale.
    i18n::LocaleManager* locale_manager = nullptr;

    // hud.json's `"preset"` value — drives preset-specific HUD behavior
    // (e.g. focus_target auto-acquire only runs for `"action"`).
    std::string preset;

    // Focus target — Action-preset "who am I aiming abilities at" state.
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
        simulation::Unit   unit;
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

// ── Shared pure-data helpers ─────────────────────────────────────────────
// Both hud.cpp (state) and hud_renderer.cpp (render) need to resolve a
// slot's ability instance, check affordability, etc. Declared inline so
// both translation units get the same definition without linker noise.

// Hit-test helpers — defined in hud.cpp (state side owns hit-testing).
// HudRenderer doesn't need them; left out of this header by design.

// True when this ability form can actually be triggered from the action
// bar (as opposed to passives / auras).
inline bool is_castable_form(simulation::AbilityForm f) {
    using F = simulation::AbilityForm;
    return f == F::Instant || f == F::Target;
}

// WC3-style command card. The bar always draws; each slot self-gates on
// `command_bar_slot_applies`. It's false when the selection isn't a unit the
// local player owns, or when the lead unit lacks the capability the command
// needs (move/patrol/hold need locomotion, attack needs a weapon) — such
// slots render as an empty frame with no icon and can't be clicked. Defined
// in hud.cpp (has the sim includes); shared by the input-side hit-test and
// the renderer so shown ⇔ clickable stay in lockstep.
bool command_bar_slot_applies(const Hud::Impl& s, const std::string& command);

} // namespace uldum::hud
