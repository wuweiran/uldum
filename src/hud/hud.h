#pragma once

#include "core/types.h"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

// Forward-decls: keep the header free of vulkan.h / VMA.
typedef struct VkCommandBuffer_T* VkCommandBuffer;

namespace uldum::rhi      { class VulkanRhi; }
namespace uldum::platform { struct InputState; }

namespace uldum::hud {

class Node;                    // node.h
class Panel;                   // node.h
struct WorldOverlayConfig;     // world.h
struct WorldContext;           // world.h
struct TextTagId;              // text_tag.h
struct TextTagCreateInfo;      // text_tag.h
struct ActionBarConfig;        // action_bar.h
enum class ActionBarHotkeyMode : u8;  // action_bar.h
struct MinimapConfig;          // minimap.h
struct CommandBarConfig;       // command_bar.h
struct JoystickConfig;         // joystick.h
struct CastIndicatorConfig;    // cast_indicator.h
struct CastIndicatorStyle;     // cast_indicator.h

// Packed RGBA color, 0xAABBGGRR on little-endian hosts. Use rgba() helper
// to build one; the HUD pipeline expects u8×4 → normalized vec4 with the
// R component first.
struct Color {
    u32 rgba = 0xFFFFFFFF;
};
inline constexpr Color rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return { static_cast<u32>(r)
           | (static_cast<u32>(g) <<  8)
           | (static_cast<u32>(b) << 16)
           | (static_cast<u32>(a) << 24) };
}

struct Rect {
    f32 x = 0, y = 0, w = 0, h = 0;
};

// The HUD subsystem. Owns the Vulkan pipeline, pooled ring buffers for
// vertex / index data, and a 1×1 white texture used for non-textured
// quads. Rendered as an overlay inside the main render pass after the
// 3D scene.
//
// Lifecycle per frame:
//   hud.begin_frame(w, h)        — reset CPU-side draw lists, stash viewport
//   hud.draw_rect(...) etc.       — record quads
//   hud.render(cmd)              — upload, bind pipeline, draw (inside render pass)
//
// Stage A scope: solid-color rectangles only. Widget tree, text, input,
// and platform-specific affordances land in subsequent stages.
class Hud {
public:
    Hud();
    ~Hud();
    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    bool init(rhi::VulkanRhi& rhi);
    void shutdown();

    // Local player for this process (engine-internal — NOT Lua-visible).
    // Used as a render / sync filter so owned-by-other-player nodes don't
    // appear on this user's screen. Set from App::start_session. Dedicated
    // servers never render anyway, so the value there is moot.
    void set_local_player(u32 player_id);
    u32  local_player() const;

    // Real-world-sized HUD authoring. The canonical unit is Android's
    // `dp` (1 dp = 1/160 inch). Every `"w": 50` in hud.json renders at
    // 50/160 inch on any device, regardless of resolution or density.
    // The platform layer reports `physical pixels per dp` at init — on
    // Android it's (density / 160), on Windows it's (DPI / 160),
    // default 1.0 elsewhere. The engine assumes the physical screen is
    // at least 800×600 dp (≈ 5×3.75 inches) — smaller devices are not
    // targeted.
    void set_ui_scale(f32 px_per_dp);
    f32  ui_scale() const;

    // True on touch-first platforms. App pushes this once from
    // `Platform::is_mobile()` at init so the hud.json loader can gate
    // mobile-only composites (virtual joystick) without duplicating
    // the map.
    void set_is_mobile(bool mobile);
    bool is_mobile() const;

    // Safe-area insets in physical pixels — the region that's blocked
    // by system UI on the target device (Android status/nav bars,
    // notch, etc.). HUD composites + tree root anchor against the
    // inner rect so nothing lands under a system bar. App pushes
    // these from `Platform::safe_insets()` at init and on resize.
    struct SafeInsets { f32 left = 0, top = 0, right = 0, bottom = 0; };
    void set_safe_insets(const SafeInsets& insets);
    SafeInsets safe_insets() const;

    // ── Viewport ─────────────────────────────────────────────────────────
    // Re-resolve every composite's absolute rect from its stored
    // placement. Called from App when the window resizes so bars,
    // minimaps, and their slots re-anchor correctly against the new
    // viewport instead of staying pinned to the old dimensions.
    void on_viewport_resized(u32 screen_w, u32 screen_h);

    // ── Per-frame ────────────────────────────────────────────────────────
    // begin_frame() resets the CPU-side draw list and stashes the viewport.
    // render() uploads to the GPU ring buffer and issues draw calls inside
    // the caller's render pass. Tree widgets are drawn via hud.root().draw()
    // — callers don't iterate explicitly.
    void begin_frame(u32 screen_w, u32 screen_h);
    void render(VkCommandBuffer cmd);

    // Low-level primitive — draw a solid-color rectangle. Used by widgets
    // internally; callers usually add Panel / Button nodes via the tree
    // instead.
    void draw_rect(const Rect& r, Color color);

    // Transient selection-marquee rectangle (the rect the player drags
    // across the world to box-select units). Draws with the colors
    // authored in hud.json's `selection_marquee` block; fill defaults
    // to transparent (outline-only) so a busy terrain stays readable.
    // Coords are in the same logical-pixel space as draw_rect.
    void draw_marquee(f32 x0, f32 y0, f32 x1, f32 y1);

    // Per-map styling for the selection marquee. Authored in hud.json
    // (`selection_marquee: { fill, border }`). Called by the loader.
    struct MarqueeStyle {
        Color fill   = rgba(0,   0,   0,   0);     // transparent by default
        Color border = rgba(80,  220, 90,  220);
    };
    void set_marquee_style(const MarqueeStyle& style);

    // Low-level primitive — draw a texture, stretched to fill the rect.
    // `asset_path` is resolved by the asset manager (KTX2 / PNG); the
    // first call for a path uploads the image and caches its GPU
    // resources for the Hud's lifetime, subsequent calls just bind the
    // cached descriptor. `tint` is a multiplier (default opaque white).
    // No-ops if the asset is missing or fails to decode.
    void draw_image(const Rect& r, std::string_view asset_path,
                    Color tint = rgba(255, 255, 255, 255));

    // Low-level primitive — draw a single line of UTF-8 text using the
    // default HUD font. Position is the left end of the text baseline (in
    // screen pixels). px_size selects on-screen glyph size; the MSDF
    // atlas was authored at one size but scales cleanly. No-ops silently
    // if the font failed to load or the atlas is full.
    void draw_text(f32 x_left, f32 y_baseline, std::string_view utf8,
                   Color color, f32 px_size);

    // Line metrics of the default UI font, in on-screen pixels at the
    // given target size. Used by Label and other text-bearing nodes to
    // position the baseline and align vertically.
    f32 text_ascent_px(f32 px_size)      const;
    f32 text_line_height_px(f32 px_size) const;

    // Measure the pixel width a UTF-8 string would occupy at the given
    // target size. Used by Label for center / right alignment. Returns 0
    // if the font failed to load.
    f32 text_width_px(std::string_view utf8, f32 px_size) const;

    // ── Widget tree ──────────────────────────────────────────────────────
    // One persistent root Panel is created at init; all widgets attach as
    // descendants of it. Cleared of children on shutdown.
    Panel& root();

    // Remove all nodes under the root. Called between sessions so a freshly
    // loaded hud.json starts from a clean slate. Also clears transient
    // input state (hover / pressed pointers) because those might reference
    // nodes we're about to free.
    void clear_nodes();

    // Reset every piece of HUD state that's tied to the active session
    // (widget tree, text tags, composite configs + runtime, slot
    // interaction state, drag-cast state, hidden-hotkey edge tracking,
    // pointer state). Called by App::end_session() so the next session
    // starts from a clean slate. Engine-level state (Vulkan pipelines,
    // font, image cache) is preserved.
    void reset_session_state();

    // Walk the tree and return the first node with the matching JSON id.
    // O(N) in the number of nodes — N is small in practice (tens to a few
    // hundred); revisit with a hash map if profiling shows hot lookups.
    // Returns nullptr if not found.
    Node* find_node_by_id(std::string_view id);

    // Walk the tree and destroy the first node with a matching id (and
    // its entire subtree). Returns true if a node was removed. Used by
    // Lua's DestroyNode(id); safe to call on unknown ids (returns false).
    bool remove_node_by_id(std::string_view id);

    // ── By-id state setters (Lua-facing; also route to MP sync) ─────────
    // Each resolves the node by id and applies the change locally; if a
    // sync_fn is installed (host mode), it also emits a protocol message
    // tagged with the node's owner so the network layer can route it.
    // Silent no-op on unknown id or wrong node type.
    void set_label_text(std::string_view id, std::string_view text);
    void set_bar_fill(std::string_view id, f32 fill);
    void set_node_visible(std::string_view id, bool visible);
    void set_image_source(std::string_view id, std::string_view source);
    void set_button_enabled(std::string_view id, bool enabled);

    // ── Templates (hud.json `nodes` block) ───────────────────────────────
    // Templates are definitions — they describe what a node subtree looks
    // like but don't render until Lua instantiates them. At map load the
    // hud.json loader registers each top-level entry in the `nodes` array
    // as a template keyed by its id. Lua calls `CreateNode(id)` which
    // looks up the template and builds a fresh instance under the root.
    void add_node_template(std::string id, const nlohmann::json& spec);
    const nlohmann::json* get_node_template(std::string_view id) const;
    void clear_node_templates();

    // Instantiate a registered template by id into the HUD root at the
    // given placement. Returns true on success. Fails if the template
    // doesn't exist or a node with the same id is already present.
    // Called from Lua's CreateNode(id, { anchor, x, y, w, h }).
    struct Placement {
        std::string_view anchor = "tl";
        f32 x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        u32 owner_player = UINT32_MAX;   // broadcast by default
    };
    bool instantiate_template(std::string_view id, const Placement& placement);

    // Walk the tree, hit-test + draw the visible subtree, and record quads
    // for this frame. Called between begin_frame() and render(). No-op if
    // the root is empty.
    void draw_tree();

    // ── World UI ─────────────────────────────────────────────────────────
    // Configuration is set once at map load (from hud.json's
    // `world_overlays` block). Context is set at session start / cleared at
    // session end; it carries refs into the active sim, camera, picker,
    // selection, terrain. draw_world_overlays() walks all entities each
    // frame and emits quads into the same batcher as screen UI.
    void set_world_overlay_config(const WorldOverlayConfig& cfg);
    void set_world_context(const WorldContext* ctx);
    // `alpha` is the sub-tick interpolation factor (0..1) — passed through
    // to the projection so world-anchored overlays follow smoothly-moving
    // units, matching how `Renderer::draw` interpolates the units themselves.
    void draw_world_overlays(f32 alpha);

    // Advance text-tag animation state (velocity × dt, age, fade).
    // Called once per frame before draw_world_overlays; expired tags
    // (`age >= lifespan` with `lifespan > 0`) are released.
    void update_text_tags(f32 dt);

    // ── Text tags ────────────────────────────────────────────────────────
    // WC3-style floating / persistent text. Engine-side API — Lua
    // bindings in 16c-v forward here. Returns an invalid id if the pool
    // is exhausted or the info is degenerate.
    TextTagId create_text_tag(const TextTagCreateInfo& info);
    void      destroy_text_tag(TextTagId id);
    bool      text_tag_alive(TextTagId id) const;

    void set_text_tag_text    (TextTagId id, std::string_view text);
    void set_text_tag_pos     (TextTagId id, f32 x, f32 y, f32 z);
    void set_text_tag_pos_unit(TextTagId id, u32 unit_id, f32 z_offset);
    void set_text_tag_color   (TextTagId id, Color color);
    void set_text_tag_velocity(TextTagId id, f32 vx, f32 vy);
    void set_text_tag_visible (TextTagId id, bool visible);

    // ── Composites ───────────────────────────────────────────────────────
    // Engine-authored node groups whose layout + styling come from
    // `composites.*` in hud.json. The action_bar's contents are
    // selection-driven — whichever unit the local player has selected
    // fills the slots, matched by hotkey letter (slot.hotkey ==
    // ability.hotkey). No Lua binding is required for the common case.

    // Install or replace the action_bar config (called by hud_loader.cpp
    // when parsing the `composites.action_bar` block). Resets transient
    // runtime state so stale state from a previous map can't leak.
    void set_action_bar_config(const ActionBarConfig& cfg);

    // Visibility — bar-level (hides the whole composite) and per-slot
    // (hides a single slot while leaving others visible). No-op when the
    // config is disabled or the slot index is out of range.
    void action_bar_set_visible(bool visible);
    void action_bar_set_slot_visible(u32 slot, bool visible);

    // Manual slot → ability binding (takes effect when the bar's
    // `binding_mode == Manual`). No-op on out-of-range slot index.
    // Passive abilities can be bound but shouldn't be (convention) —
    // nothing fires when the slot is triggered. Clearing removes the
    // binding so the slot renders empty again.
    void action_bar_set_slot(u32 slot, std::string_view ability_id);
    void action_bar_clear_slot(u32 slot);

    // Hotkey mode — driven by the global `input.action_bar_hotkey_mode`
    // setting. App subscribes to the key on init and pushes the value
    // here; resolve + keyboard dispatch consult it every frame so
    // changes take effect immediately.
    void action_bar_set_hotkey_mode(ActionBarHotkeyMode mode);

    // Mark which ability is currently in targeting mode (empty = none).
    // App pushes this each frame from the input preset so the classic_rts
    // render path can render the matching slot "armed" (press_bg) while
    // the player picks a target.
    void action_bar_set_targeting_ability(std::string_view ability_id);

    // Callback fired when a slot click OR slot hotkey press resolves to
    // an ability. The HUD performs hit-testing, keyboard scanning, and
    // selection-driven ability lookup internally; the app wires this up
    // to the input preset (typically `InputPreset::queue_ability`).
    using ActionBarCastFn = std::function<void(const std::string& ability_id)>;
    void set_action_bar_cast_fn(ActionBarCastFn fn);

    // Mobile drag-cast commit callback. Fired by the HUD when a drag
    // gesture on a targetable slot completes with a valid target —
    // either a snapped unit (target_unit form) or a ground point
    // (target_point form). The app wires this to the input preset's
    // direct-cast entry point so the resulting `Cast` order goes
    // through CommandSystem the same way a desktop click-targeting
    // commit does. `target_unit_id` is `UINT32_MAX` for ground casts.
    using ActionBarCastAtTargetFn = std::function<void(
        const std::string& ability_id,
        u32                target_unit_id,
        f32 target_x, f32 target_y, f32 target_z
    )>;
    void set_action_bar_cast_at_target_fn(ActionBarCastAtTargetFn fn);

    // Per-frame mobile drag-cast update. Reads the platform input state
    // (touch positions, primary pointer) and advances the drag-cast
    // state machine: press → aim → release. No-op on desktop (gated on
    // `is_mobile()`). Call alongside `joystick_update` each frame.
    void action_bar_drag_update(const platform::InputState& input);

    // What the HUD is currently aiming with — drives the ability
    // indicator renderer. Plain data, no render-side types, so callers
    // can decide how to draw (sub-phase D's `AbilityIndicators` reads
    // this each frame and emits ground decals). `active == false` means
    // nothing to draw. Coordinates are world-space; `area_*` mirror
    // the active ability level's `area` block when set.
    // Tri-state aim phase consumed by the indicator renderer to pick
    // colors. White / blue / red feel — exact RGBA values are app
    // (engine default) or hud.json (per-map override) decisions.
    enum class AimPhase : u8 {
        Normal      = 0,   // in cast range, will fire on release
        OutOfRange  = 1,   // beyond cast range — release still casts;
                           // sim approaches the target then casts
        Cancelling  = 2,   // finger is over the cancel rect — release
                           // cancels with no cast
    };

    // AoE shape for the cast indicator. Mirrors simulation::IndicatorShape
    // but stays in HUD-land so render-side code doesn't need to pull in
    // simulation headers.
    enum class AimAreaShape : u8 {
        None   = 0,   // no AoE indicator
        Circle = 1,   // disc at the cast/snap point — uses area_radius
        Line   = 2,   // rectangle from caster toward drag — uses area_width + range as length
        Cone   = 3,   // wedge from caster toward drag — uses area_angle (degrees) + range as radius
    };

    struct AbilityAimState {
        bool active = false;
        AimPhase phase = AimPhase::Normal;
        // Caster + drag point in world space (z is terrain-sampled).
        f32  caster_x = 0, caster_y = 0, caster_z = 0;
        f32  drag_x   = 0, drag_y   = 0, drag_z   = 0;
        // Cast range (drives the range ring radius). For Line / Cone
        // it's also the indicator's extent along the cast direction.
        f32  range = 0;
        // 2D world distance from caster to the *anchor* the cast will
        // resolve at — the snapped unit's pos for unit-target, the
        // drag point for ground-target. Used by callers that want to
        // make their own range judgement.
        f32  distance = 0;
        // Optional area-of-effect; valid when has_area == true.
        bool        has_area    = false;
        AimAreaShape area_shape = AimAreaShape::None;
        f32         area_radius = 0;   // Circle (and target_unit-around-target)
        f32         area_width  = 0;   // Line
        f32         area_angle  = 0;   // Cone, degrees
        // Unit-targeted snap state. snapped_id == UINT32_MAX when not
        // snapped (or form != target_unit).
        u32  snapped_id     = 0xFFFFFFFFu;
        f32  snapped_x = 0, snapped_y = 0, snapped_z = 0;
        f32  snapped_radius = 0;
        // Whether the active ability is unit-targeted. Affects whether
        // the renderer should hide the reticle (in favor of the snap
        // ring) when snapped.
        bool is_unit_target = false;
        // True when the aim state comes from a mobile drag-cast
        // gesture (finger held on an action-bar slot, dragging to
        // pick a target). False for desktop targeting mode. The
        // cast-arrow / curve indicator only reads naturally in the
        // mobile case — on desktop there's no "from" point, so the
        // renderer hides the curve when this is false.
        bool is_drag_cast = false;
    };
    AbilityAimState aim_state() const;

    // Per-map style for the cast/drag indicators (range ring, arrow,
    // reticle, AoE, target-unit ring, per-phase tints). Defaults are
    // engine-defined; hud.json's `composites.cast_indicator` block
    // overrides any subset. Read by the app each frame when feeding
    // AbilityIndicators draw calls.
    void set_cast_indicator_config(const CastIndicatorConfig& cfg);
    const CastIndicatorStyle& cast_indicator_style() const;

    // Keyboard hotkey dispatch for every HUD-owned source — command_bar
    // slots, action_bar slots, and hidden abilities on the selected
    // unit. Walks them in that priority order and claims keys as it
    // goes, so if two sources share a letter only the highest-priority
    // one fires. Call once per frame (alongside handle_pointer) before
    // the input preset's update so queued actions are consumed in the
    // same frame.
    void handle_hotkeys(const platform::InputState& input);

    // Minimap composite — schematic top-down view of the world. v1 is
    // bg + border + unit dots (fog-filtered) + click-to-jump camera.
    void set_minimap_config(const MinimapConfig& cfg);
    void minimap_set_visible(bool visible);

    // Fired when the player clicks inside the minimap. Coords are world
    // X / Y in the ground plane (z = 0). App wires this to a camera
    // pose change so the view snaps to that location.
    using MinimapJumpFn = std::function<void(f32 world_x, f32 world_y)>;
    void set_minimap_jump_fn(MinimapJumpFn fn);

    // Command-bar composite — grid of engine-built-in commands (Attack,
    // Move, Stop, Hold, Patrol). Maps opt in via `composites.command_bar`
    // in hud.json. Tapping a slot fires the callback with the command
    // id ("attack", "move", etc.); the app routes to the input preset
    // which dispatches the same order the keyboard binding would.
    void set_command_bar_config(const CommandBarConfig& cfg);
    void command_bar_set_visible(bool visible);

    using CommandFn = std::function<void(const std::string& command_id)>;
    void set_command_bar_fn(CommandFn fn);

    // Mark which command the preset is waiting on (targeting mode).
    // Passed through from the app each frame; the classic_rts render
    // highlights the matching slot so the player sees what's armed.
    void command_bar_set_armed_command(std::string_view command_id);

    // Joystick composite — virtual analog stick for touch-screen camera
    // pan. Per-frame flow is: App calls `joystick_update(input)` which
    // captures a finger on press and tracks it until release; App then
    // reads `joystick_vector(dx, dy)` to get a normalized [-1,1]² input
    // and feeds it to the camera. Touch-and-mouse input both drive it;
    // desktop maps simply don't declare the composite.
    void set_joystick_config(const JoystickConfig& cfg);
    void joystick_set_visible(bool visible);
    void joystick_update(const platform::InputState& input);
    void joystick_vector(f32& dx, f32& dy) const;
    // True if the joystick currently owns a finger / mouse button. Used
    // by the preset so drag-selection / tap-to-order don't double-fire.
    bool joystick_active() const;

    // ── Network sync (host-side) ─────────────────────────────────────────
    // Host installs a callback that turns local HUD mutations into
    // protocol messages and sends them to the appropriate client peer(s).
    // The callback receives:
    //   - an opaque byte packet (ready-to-send)
    //   - the owning player (UINT32_MAX = broadcast, else target slot)
    // Offline sessions and clients leave this unset; mutations stay local.
    using SyncFn = std::function<void(const std::vector<u8>& packet, u32 owner_player)>;
    void set_sync_fn(SyncFn fn);

    // ── Button event callback (for input → event routing) ─────────────────
    // Called synchronously from Button::on_release when a click completes
    // over the button. App wires this to the server's script engine
    // (host) or to the network send path (client).
    using ButtonEventFn = std::function<void(const std::string& node_id)>;
    void set_button_event_fn(ButtonEventFn fn);
    // Internal: invoked by Button::on_release; exposed so node.cpp can call
    // without needing to know about Impl or the callback storage.
    void fire_button_event(const std::string& node_id);

    // ── Input ────────────────────────────────────────────────────────────
    // The platform layer polls mouse / touch each frame and forwards the
    // resulting (x, y, button_down) here. The HUD tracks hover and press
    // edges internally, fires widget callbacks, and exposes input_captured()
    // so gameplay input can skip clicks that a widget handled.
    void handle_pointer(f32 x, f32 y, bool button_down);
    bool input_captured() const;

    // Most recent pointer coords — set by handle_pointer each frame.
    // Exposed so world-UI code can run hit tests (Picker::pick_target etc.)
    // against the same position the HUD is using for hover / press state.
    f32 pointer_x() const;
    f32 pointer_y() const;

    // Opaque state — declared in hud.cpp to keep Vulkan / VMA out of the header.
    struct Impl;
private:
    Impl* m_impl = nullptr;
};

} // namespace uldum::hud
