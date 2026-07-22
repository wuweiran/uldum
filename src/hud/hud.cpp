#include "hud/hud.h"
#include "hud/hud_impl.h"
#include "hud/text_tag.h"
#include "hud/hud_loader.h"
#include "hud/layout.h"
#include "render/hud/world.h"   // WorldContext — Hud holds a pointer, not a copy

#include <nlohmann/json.hpp>

#include "simulation/world.h"
#include "simulation/components.h"
#include "simulation/ability_def.h"
#include "simulation/simulation.h"
#include "simulation/type_registry.h"
#include "simulation/vision.h"
#include "map/terrain_data.h"
#include "simulation/selection.h"
#include "input/input_bindings.h"
#include "platform/platform.h"
#include "network/protocol.h"
#include "core/log.h"

#include <glm/vec3.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
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

// Why a slot can't be cast right now — the return of slot_cast_reject,
// destructured straight into emit_order_error. HUD-local: the sim no
// longer has a reason type (target_filter_passes returns a bare
// specifier; ability_can_afford names the lacking state).
struct OrderReject {
    std::string   base;        // "cooldown" | "cost"
    std::string   specifier;   // resource name, or empty
    i18n::ArgsMap args;
};

// Same-file forward decl for the affordability + cooldown gate, used by
// both click and keyboard dispatch. `is_castable_form` is `inline` in
// hud_impl.h — no forward decl needed for it. slot_cast_reject returns
// the reason (for emit_order_error); slot_castable_now is the bool wrapper.
static std::optional<OrderReject> slot_cast_reject(
        const WorldContext& ctx, u32 unit_id,
        const simulation::AbilityInstance& inst,
        const simulation::AbilityDef& def);
static bool slot_castable_now(const WorldContext& ctx, u32 unit_id,
                              const simulation::AbilityInstance& inst,
                              const simulation::AbilityDef& def);

// Forward decls for inventory helpers — defined further down but used
// from `Hud::handle_right_click` higher up.
static i32 inventory_hit_test(const Hud::Impl& s, f32 x, f32 y);
static const simulation::Inventory*
inventory_resolve_selected(const Hud::Impl& s, u32* out_carrier_id = nullptr);
static bool inventory_resolve_slot(const Hud::Impl& s,
                                   const simulation::Inventory* inv,
                                   u32 slot_index,
                                   simulation::Item& out_item,
                                   const simulation::ItemInfo*& out_info,
                                   const simulation::ItemTypeDef*& out_def);

// ── Hud facade (data side) ───────────────────────────────────────────────

Hud::Hud() = default;
Hud::~Hud() { shutdown(); }

// HudRenderInterface anchor — defining the destructor here forces the
// interface's vtable to be emitted into `uldum_hud`, so Node draws (which
// virtual-dispatch through the interface) link cleanly without needing
// HudRenderer's translation unit.
HudRenderInterface::~HudRenderInterface() = default;

bool Hud::init() {
    m_impl = new Impl{};
    m_impl->root = std::make_unique<Panel>();
    m_impl->root->bg = rgba(0, 0, 0, 0);  // invisible root — children draw their own backgrounds
    m_impl->root->hit_testable = false;   // let clicks on blank HUD areas fall through to world / gameplay
    log::info(TAG, "Hud initialized");
    return true;
}

void Hud::shutdown() {
    if (!m_impl) return;
    auto& s = *m_impl;
    s.root.reset();
    s.hover   = nullptr;
    s.pressed = nullptr;
    delete m_impl;
    m_impl = nullptr;
}

void Hud::on_viewport_resized(u32 screen_w, u32 screen_h) {
    if (!m_impl) return;
    // Caller supplies physical framebuffer dims; convert to logical
    // (dp) before re-resolving composite rects so anchors match what
    // begin_frame will use next frame.
    // Also push the dims into Impl now so any CreateNode calls that
    // run before the first begin_frame (e.g. scene main.lua executing
    // inside start_session, before AppState flips to Playing) can
    // still resolve placements against a non-zero viewport.
    f32 s = m_impl->ui_scale;
    if (s <= 0.0f) s = 1.0f;
    m_impl->physical_w = screen_w;
    m_impl->physical_h = screen_h;
    m_impl->screen_w   = static_cast<u32>(static_cast<f32>(screen_w) / s);
    m_impl->screen_h   = static_cast<u32>(static_cast<f32>(screen_h) / s);
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

    auto& pb = m_impl->pickup_bar_cfg;
    if (pb.enabled) {
        pb.rect = resolve(viewport, pb.placement);
        for (auto& slot : pb.slots) {
            slot.rect = resolve(pb.rect, slot.placement);
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
    s.pickup_bar_hover_slot      = -1;
    s.pickup_bar_pressed_slot    = -1;
    s.pickup_bar_rt.entries.clear();
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
    s.pickup_bar_hover_slot      = -1;
    s.pickup_bar_pressed_slot    = -1;
    s.pickup_bar_rt.entries.clear();
    s.focus_target_unit          = simulation::Unit{};
    s.focus_manual               = false;
    s.tooltip                    = Impl::TooltipState{};

    // Note: the GPU image cache (Vulkan resources) moved to HudRenderer
    // with the data/render split. The renderer's own reset hook handles
    // it; here we only clear data-side state.

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
    s.pickup_bar_cfg     = {};
    s.pickup_bar_rt      = {};
    s.display_message_cfg = {};
    s.display_message_rt  = {};
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
    s.pickup_fn                  = {};
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
    Node* removed = find_node_by_id(id);
    if (removed) mask = removed->players_mask;
    // Clear transient hover / pressed references before we drop the node,
    // else we'd chase a freed pointer next input frame. Removing a node
    // frees its ENTIRE subtree, so a hovered/pressed *descendant* dangles
    // too — test subtree membership, not id-equality against the removed
    // node alone.
    if (removed) {
        if (m_impl->hover   && removed->contains(m_impl->hover))   m_impl->hover   = nullptr;
        if (m_impl->pressed && removed->contains(m_impl->pressed)) m_impl->pressed = nullptr;
    }
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
    if (!m_impl) return false;
    // Use the cached physical extent (pushed by HudRenderer::begin_frame
    // and on_viewport_resized). Before the first viewport push, returns
    // false rather than instantiating against a 0×0 viewport.
    u32 ext_w = m_impl->physical_w;
    u32 ext_h = m_impl->physical_h;
    if (ext_w == 0 || ext_h == 0) return false;
    TemplatePlacement tp{};
    tp.anchor       = placement.anchor;
    tp.x            = placement.x;
    tp.y            = placement.y;
    tp.w            = placement.w;
    tp.h            = placement.h;
    tp.players_mask = placement.players_mask;
    bool ok = uldum::hud::instantiate_template(*this, id, ext_w, ext_h, tp);
    if (ok) {
        emit_sync(*m_impl,
                  uldum::network::build_hud_create_node(id, placement.anchor,
                                                         placement.x, placement.y,
                                                         placement.w, placement.h),
                  placement.players_mask);
    }
    return ok;
}

void Hud::set_world_overlay_config(const WorldOverlayConfig& cfg) {
    if (!m_impl) return;
    m_impl->world_cfg = cfg;
}

void Hud::set_world_context(const WorldContext* ctx) {
    if (!m_impl) return;
    m_impl->world_ctx = ctx;
}

const WorldContext* Hud::world_context() const {
    return m_impl ? m_impl->world_ctx : nullptr;
}

void Hud::set_locale_manager(i18n::LocaleManager* mgr) {
    if (!m_impl) return;
    m_impl->locale_manager = mgr;
}

i18n::LocaleManager* Hud::locale_manager() const {
    return m_impl ? m_impl->locale_manager : nullptr;
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
    if (!world.contains(hero)) {
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
        if (simulation::is_null_handle(u) || !world.contains(u)) return false;
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
            ? s.world_ctx->simulation->is_allied(*hero_owner, *o)
            : (*o == *hero_owner)) {
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
        best = simulation::Unit{id};
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
    m_impl->focus_manual = simulation::is_non_null_handle(unit);
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
    t.unit       = info.unit;
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
                  info.unit, info.z_offset,
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
        t->unit      = {};
    }
}
void Hud::set_text_tag_pos_unit(TextTagId id, simulation::Unit unit, f32 z_offset) {
    if (!m_impl) return;
    if (auto* t = lookup_tag(*m_impl, id)) {
        t->unit     = unit;
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

Rect Hud::minimap_screen_rect() const {
    // Zero rect when disabled/hidden so the picker's minimap proxy stays off.
    if (!m_impl || !m_impl->minimap_cfg.enabled || !m_impl->minimap_rt.visible) return {};
    return m_impl->minimap_cfg.rect;
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

void Hud::set_pickup_bar_config(const PickupBarConfig& cfg) {
    if (!m_impl) return;
    m_impl->pickup_bar_cfg = cfg;
    m_impl->pickup_bar_rt = PickupBarRuntime{};
    m_impl->pickup_bar_hover_slot = -1;
    m_impl->pickup_bar_pressed_slot = -1;
}

void Hud::pickup_bar_set_visible(bool visible) {
    if (m_impl) m_impl->pickup_bar_rt.visible = visible;
}

void Hud::set_pickup_fn(PickupFn fn) {
    if (m_impl) m_impl->pickup_fn = std::move(fn);
}

void Hud::pickup_bar_update() {
    if (!m_impl) return;
    auto& s = *m_impl;
    std::vector<PickupBarEntry> next;

    const auto clear_interaction = [&]() {
        for (auto& slot : s.pickup_bar_cfg.slots) {
            slot.hovered = false;
            slot.pressed = false;
        }
        s.pickup_bar_hover_slot = -1;
        s.pickup_bar_pressed_slot = -1;
        if (s.tooltip.source == Impl::TooltipState::Source::PickupBar) {
            s.tooltip = Impl::TooltipState{};
        }
    };

    const auto& cfg = s.pickup_bar_cfg;
    if (!s.world_ctx || !s.world_ctx->world || !s.world_ctx->selection) {
        if (!s.pickup_bar_rt.entries.empty()) clear_interaction();
        s.pickup_bar_rt.entries.clear();
        return;
    }
    const auto& world = *s.world_ctx->world;
    if (!s.is_mobile || !cfg.enabled || !s.pickup_bar_rt.visible || cfg.slots.empty()) {
        if (!s.pickup_bar_rt.entries.empty()) clear_interaction();
        s.pickup_bar_rt.entries.clear();
        return;
    }

    if (s.pickup_bar_pressed_slot >= 0) return;

    const auto& selected = s.world_ctx->selection->selected();
    if (selected.empty()) {
        if (!s.pickup_bar_rt.entries.empty()) clear_interaction();
        s.pickup_bar_rt.entries.clear();
        return;
    }

    simulation::Unit unit = selected.front();
    const auto* owner = world.owners.get(unit.id);
    const auto* unit_tf = world.transforms.get(unit.id);
    const auto* inventory = world.inventories.get(unit.id);
    bool owned = owner && owner->id == s.world_ctx->local_player.id;
    if (!world.contains(unit) || !owned || !unit_tf || !inventory) {
        if (!s.pickup_bar_rt.entries.empty()) clear_interaction();
        s.pickup_bar_rt.entries.clear();
        return;
    }

    bool has_free_slot = false;
    for (simulation::Item item : inventory->slots) {
        if (simulation::is_null_handle(item)) {
            has_free_slot = true;
            break;
        }
    }
    if (!has_free_slot) {
        if (!s.pickup_bar_rt.entries.empty()) clear_interaction();
        s.pickup_bar_rt.entries.clear();
        return;
    }

    struct Candidate {
        simulation::Item item;
        f32 distance_sq = 0.0f;
    };
    std::vector<Candidate> candidates;
    const f32 radius_sq = cfg.discovery_radius * cfg.discovery_radius;
    for (u32 item_id : world.item_infos.ids()) {
        simulation::Item item{item_id};
        if (!world.contains(item)) continue;
        if (world.dead_states.has(item_id)) continue;
        const auto* carriable = world.carriables.get(item_id);
        if (!carriable || simulation::is_non_null_handle(carriable->carried_by)) continue;
        const auto* item_tf = world.transforms.get(item_id);
        if (!item_tf || !focus_visible(*s.world_ctx, item_tf->position)) continue;

        f32 dx = item_tf->position.x - unit_tf->position.x;
        f32 dy = item_tf->position.y - unit_tf->position.y;
        f32 distance_sq = dx * dx + dy * dy;
        if (distance_sq <= radius_sq) candidates.push_back({item, distance_sq});
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.distance_sq != b.distance_sq) return a.distance_sq < b.distance_sq;
        return a.item.id < b.item.id;
    });

    const u32 count = std::min(static_cast<u32>(candidates.size()),
                               static_cast<u32>(cfg.slots.size()));
    next.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        next.push_back(PickupBarEntry{unit, candidates[i].item});
    }

    if (next != s.pickup_bar_rt.entries) clear_interaction();
    s.pickup_bar_rt.entries = std::move(next);
}

// ── Display-message composite ────────────────────────────────────────────

void Hud::set_display_message_config(const DisplayMessageConfig& cfg) {
    if (!m_impl) return;
    m_impl->display_message_cfg = cfg;
    m_impl->display_message_rt  = DisplayMessageRuntime{};
}

void Hud::display_message_set_visible(bool visible) {
    if (m_impl) m_impl->display_message_rt.visible = visible;
}

void Hud::display_message(i18n::LocalizedString text, f32 duration, u32 players_mask) {
    if (!m_impl) return;
    auto& s = *m_impl;
    if (!s.display_message_cfg.enabled) {
        // Graceful degradation: map didn't author the composite. Log
        // the call so dev can see the message in the console.
        log::info(TAG, "DisplayMessage (no composite authored): key='{}' duration={}",
                  text.key, duration);
        return;
    }
    const auto& style = s.display_message_cfg.style;
    f32 lifespan = (duration > 0.0f) ? duration : style.default_lifespan;
    if (lifespan <= 0.0f) lifespan = 5.0f;

    DisplayMessageLine line;
    line.loc          = std::move(text);
    line.lifespan     = lifespan;
    line.fadepoint    = style.fadepoint;
    line.players_mask = players_mask;
    s.display_message_rt.lines.push_back(std::move(line));

    // Cap simultaneous lines — drop the oldest if we'd exceed max_lines.
    u32 cap = (style.max_lines > 0) ? style.max_lines : 4;
    while (s.display_message_rt.lines.size() > cap) {
        s.display_message_rt.lines.erase(s.display_message_rt.lines.begin());
    }

    // Host-side fan-out — the network layer routes to matching peers
    // (or broadcasts when mask == UINT32_MAX). Each client renders the
    // line in its own active locale.
    if (s.sync_fn) {
        auto& back = s.display_message_rt.lines.back();
        auto pkt = uldum::network::build_hud_display_message(
            back.loc.key, back.loc.args, lifespan);
        s.sync_fn(pkt, players_mask);
    }
}

void Hud::set_error_sound(std::string path) {
    if (m_impl) m_impl->error_sound = std::move(path);
}

void Hud::set_play_sound_fn(PlaySoundFn fn) {
    if (m_impl) m_impl->play_sound_fn = std::move(fn);
}

void Hud::emit_order_error(std::string_view base, std::string_view specifier,
                           const i18n::ArgsMap& args) {
    if (!m_impl) return;
    auto& s = *m_impl;
    if (!s.display_message_cfg.enabled) return;   // no overlay authored → silent

    // Throttle: same reason within 1.5s is dropped so a spam-click on a
    // dark ability doesn't flood the line stack.
    std::string throttle_key = std::string(base) + "." + std::string(specifier);
    if (throttle_key == s.last_order_error_key && s.order_error_since < 1.5f) return;
    s.last_order_error_key = throttle_key;
    s.order_error_since    = 0.0f;

    // Resolve chain: text.json `ui.error.<base>.<specifier>` → `ui.error.
    // <base>` → engine built-in. First hit wins. The reject is client-
    // local, so we resolve to a final string here and store it literally
    // (no per-client re-resolve needed).
    auto builtin = [&]() -> std::string {
        if (base == "cooldown") return "Not ready yet";
        if (base == "cost")     return "Not enough resources";
        if (base == "target")   return "Invalid target";
        return std::string(base);
    };

    std::string text;
    bool found = false;
    const std::string base_field = std::string(base);
    const std::string spec_field = specifier.empty()
                                     ? std::string{} : base_field + "." + std::string(specifier);
    if (s.locale_manager) {
        if (!spec_field.empty()) {
            if (auto v = s.locale_manager->try_resolve(
                    i18n::Pool::Map, "ui.error." + spec_field, args)) {
                text = std::move(*v); found = true;
            }
        }
        if (!found) {
            if (auto v = s.locale_manager->try_resolve(
                    i18n::Pool::Map, "ui.error." + base_field, args)) {
                text = std::move(*v); found = true;
            }
        }
    }
    if (!found) text = builtin();

    i18n::LocalizedString loc;
    loc.key = std::move(text);   // literal — resolve() round-trips an unknown key
    display_message(std::move(loc), 0.0f, 1u << local_player());

    if (s.play_sound_fn && !s.error_sound.empty()) {
        s.play_sound_fn(s.error_sound);
    }
}

void Hud::update_display_messages(f32 dt) {
    if (!m_impl) return;
    m_impl->order_error_since += dt;
    auto& rt = m_impl->display_message_rt;
    if (rt.lines.empty()) return;
    for (auto& l : rt.lines) l.age += dt;
    // Drop expired lines in-place.
    rt.lines.erase(
        std::remove_if(rt.lines.begin(), rt.lines.end(),
                       [](const DisplayMessageLine& l) {
                           return l.age >= l.lifespan;
                       }),
        rt.lines.end());
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
    f32 inv_s = 1.0f / s.ui_scale;  // ui_scale clamped > 0 at its sole writer (set_ui_scale)
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
bool command_bar_slot_applies(const Hud::Impl& s, const std::string& command) {
    if (!s.world_ctx || !s.world_ctx->world || !s.world_ctx->selection) return false;
    const auto& sel = s.world_ctx->selection->selected();
    if (sel.empty()) return false;
    const simulation::World& world = *s.world_ctx->world;
    simulation::Unit lead = sel.front();
    const auto* own = world.owners.get(lead.id);
    if (!own || own->id != s.world_ctx->local_player.id) return false;

    const auto* mv  = world.movements.get(lead.id);
    bool can_move   = mv && mv->speed > 0.0f;
    // Combat is opt-in (create_unit only adds the component when the type
    // declares a `combat` block), so presence IS the capability — a
    // barracks has no Combat component and thus no attack command.
    bool can_attack = world.combats.get(lead.id) != nullptr;

    if (command == "attack")        return can_attack;
    if (command == "attack_move")   return can_attack && can_move;
    if (command == "move" ||
        command == "patrol" ||
        command == "hold_position") return can_move;
    if (command == "stop")          return can_move || can_attack;
    return true;   // unknown / map-custom command — leave it to the map author
}

static i32 command_bar_hit_test(const Hud::Impl& s, f32 x, f32 y) {
    const auto& cfg = s.command_bar_cfg;
    if (!cfg.enabled || !s.command_bar_rt.visible) return -1;
    for (u32 i = 0; i < cfg.slots.size(); ++i) {
        const auto& slot = cfg.slots[i];
        if (!slot.visible || !command_bar_slot_applies(s, slot.command)) continue;
        const Rect& r = slot.rect;
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
            return static_cast<i32>(i);
        }
    }
    return -1;
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
    if (!ctx.world || !ctx.pick_item || !ctx.pick_target) return TargetingIntent::Neutral;
    // Picker takes physical-pixel coords; HUD pointer is dp.
    f32 sx = m_impl->pointer_x * m_impl->ui_scale;
    f32 sy = m_impl->pointer_y * m_impl->ui_scale;
    // Item beats unit when both share the cursor — items are smaller so
    // the player almost always wants the item-pickup intent in that case.
    if (auto item = ctx.pick_item(sx, sy); simulation::is_non_null_handle(item)) {
        return TargetingIntent::Item;
    }
    if (auto unit = ctx.pick_target(sx, sy); simulation::is_non_null_handle(unit)) {
        const auto* owner = ctx.world->owners.get(unit.id);
        if (!owner) return TargetingIntent::Neutral;
        if (owner->id == ctx.local_player.id) return TargetingIntent::Ally;
        if (ctx.simulation && ctx.simulation->is_allied(ctx.local_player, *owner)) {
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
    f32 scale = s.ui_scale;  // clamped > 0 at its sole writer (set_ui_scale)
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
    // along, not stay anchored where the press happened. Cancel if the
    // caster no longer exists.
    if (!s.world_ctx->world->contains(s.drag_cast.caster)) {
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
    f32 inv = 1.0f / s.ui_scale;  // ui_scale clamped > 0 at its sole writer (set_ui_scale)
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
    f32 yaw = s.world_ctx->camera_yaw_rad;
    f32 cyaw = std::cos(yaw), syaw = std::sin(yaw);
    glm::vec3 right{cyaw, syaw, 0.0f};
    glm::vec3 forward{-syaw, cyaw, 0.0f};
    f32 ddx = px - s.drag_cast.press_x;
    f32 ddy = py - s.drag_cast.press_y;
    // World-anchored (RTS): derive the drag point from the caster's position
    // AT PRESS, so the destination stays put as the unit walks. Action preset
    // uses the live caster so the aim follows the hero.
    glm::vec3 origin = s.drag_cast.world_anchored
        ? glm::vec3{s.drag_cast.press_caster_x, s.drag_cast.press_caster_y, s.drag_cast.press_caster_z}
        : glm::vec3{s.drag_cast.caster_x, s.drag_cast.caster_y, s.drag_cast.caster_z};
    glm::vec3 drag = origin + right * (ddx * SENS) - forward * (ddy * SENS);
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
                if (!hinfo) continue;
                // Honor the drag's widget_kinds mask (WC3-style): a candidate's
                // category must be an accepted widget kind — not hard-coded to
                // Unit, or crates could never snap.
                u32 cand_kind = 0;
                switch (hinfo->category) {
                    case simulation::Category::Unit:         cand_kind = simulation::widget_kind::Unit; break;
                    case simulation::Category::Destructable: cand_kind = simulation::widget_kind::Destructable; break;
                    case simulation::Category::Item:         cand_kind = simulation::widget_kind::Item; break;
                    default: break;
                }
                if ((s.drag_cast.widget_kinds & cand_kind) == 0) continue;
                simulation::Unit cand{id};
                if (cand.id == caster_unit.id) {
                    // Commands never self-snap. Abilities use the
                    // filter's `self_` flag.
                    if (is_command || !def->target_filter.self_) continue;
                }
                // Commands always reject dead units — Move-on-corpse can't
                // follow, Attack-on-corpse can't attack. Abilities run the
                // full filter (alive/dead flags handled inside).
                if (is_command && world.dead_states.has(id)) continue;
                // Command attackability: match desktop — only snap a target
                // some selected unit can actually hit. Destructables always
                // gate on their widget bit (crate=debris yes, tree no).
                // Units gate on their MoveType layer (a ground-only force
                // can't snap a flyer) ONLY for attack commands — Move/Follow
                // must still snap any unit, including allies it can't attack.
                if (is_command) {
                    bool is_attack = (s.drag_cast.command_id == "attack" ||
                                      s.drag_cast.command_id == "attack_move");
                    bool gate = (hinfo->category == simulation::Category::Destructable)
                                || is_attack;
                    if (gate && s.world_ctx->selection) {
                        bool any_can_hit = false;
                        for (auto u : s.world_ctx->selection->selected()) {
                            const auto* cb = world.combats.get(u.id);
                            if (cb && simulation::can_attack_target(world, cb->target_mask, cand)) {
                                any_can_hit = true; break;
                            }
                        }
                        if (!any_can_hit) continue;
                    }
                }
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
                if (simulation::is_non_null_handle(s.drag_cast.snapped_target)) fire_at_target = true;
                else if (s.drag_cast.accept_point) fire_at_target = true;
            } else if (s.drag_cast.accept_point) {
                fire_at_target = true;
            }
        }
        if (fire_no_target && s.inventory_use_fn && !s.drag_cast.ability_id.empty()) {
            s.inventory_use_fn(s.drag_cast.inventory_item_id, s.drag_cast.ability_id);
        } else if (fire_at_target && s.inventory_use_at_target_fn) {
            u32 target_uid = simulation::is_non_null_handle(s.drag_cast.snapped_target)
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
            if (simulation::is_non_null_handle(s.focus_target_unit) && s.world_ctx &&
                s.world_ctx->world &&
                s.world_ctx->world->contains(s.focus_target_unit)) {
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
        if (simulation::is_non_null_handle(s.focus_target_unit) && s.world_ctx && s.world_ctx->world &&
            s.world_ctx->world->contains(s.focus_target_unit)) {
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
            } else if (simulation::is_non_null_handle(s.drag_cast.snapped_target)) {
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
        simulation::is_null_handle(s.drag_cast.snapped_target)) {
        commit = false;
        // Released a widget-only cast with no valid target under the
        // finger — explain it (bare `target`; no single offending unit).
        emit_order_error("target", "");
    }
    if (commit) {
        u32 target_uid = simulation::is_non_null_handle(s.drag_cast.snapped_target)
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
    if (dc.inventory_slot >= 0 && !dc.inventory_targetable) return out;
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
        // Lift the arrow origin to the caster's feet for air units; 0 for
        // ground. caster_z stays ground Z (ground decals read that).
        if (m_impl->world_ctx && m_impl->world_ctx->world) {
            out.caster_fly_height =
                simulation::unit_fly_height(*m_impl->world_ctx->world, dc.caster.id);
        }
        out.drag_x     = dc.drag_world_x;
        out.drag_y     = dc.drag_world_y;
        out.drag_z     = dc.drag_world_z;
        out.range      = dc.range;
        // Widget-snapped this frame? Hybrid forms can switch between
        // widget and point each frame as the cursor moves.
        out.is_unit_target = simulation::is_non_null_handle(dc.snapped_target);

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

        if (simulation::is_non_null_handle(dc.snapped_target) && m_impl->world_ctx &&
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
    if (!ctx.world || !ctx.abilities || !ctx.selection || !ctx.screen_to_world) return out;

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
    f32 s = m_impl->ui_scale;  // clamped > 0 at its sole writer (set_ui_scale)
    f32 mx = m_impl->pointer_x * s;
    f32 my = m_impl->pointer_y * s;

    glm::vec3 ground{};
    if (ctx.screen_to_world(mx, my, ground)) {
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
            simulation::Unit cand{id};
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
        if (simulation::is_non_null_handle(best)) {
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
                if (!command_bar_slot_applies(s, slot.command)) continue;
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
                if (auto rej = slot_cast_reject(*s.world_ctx, unit_id, *inst, *def)) {
                    emit_order_error(rej->base, rej->specifier, rej->args);
                    continue;
                }

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
                    if (inst.item_only()) continue;

                    const auto* def = s.world_ctx->abilities->get(inst.ability_id);
                    if (!def || def->hotkey.empty()) continue;
                    if (!is_castable_form(def->form)) continue;  // passive/aura aren't triggerable

                    bool down   = input::InputBindings::resolve_key(def->hotkey, input);
                    bool& prev  = s.hidden_hotkey_prev[def->hotkey];
                    bool rising = down && !prev;
                    prev = down;
                    if (!rising) continue;
                    if (claimed.count(def->hotkey)) continue;
                    if (auto rej = slot_cast_reject(*s.world_ctx, unit_id, inst, *def)) {
                        emit_order_error(rej->base, rej->specifier, rej->args);
                        continue;
                    }

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
        if (!def || def->hidden || inst.item_only()) continue;
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
// Why the selected unit can't trigger this slot's ability right now, or
// nullopt when it can. A non-castable form (passive/aura) is never
// "rejected" — it returns nullopt so those icons aren't suppressed.
// Cooldown is checked before cost, matching issue_order's order. The
// cost walk reuses the canonical ability_can_afford (no HUD-local copy)
// so the dim state and the reject reason agree with the sim.
static std::optional<OrderReject> slot_cast_reject(
        const WorldContext& ctx, u32 unit_id,
        const simulation::AbilityInstance& inst,
        const simulation::AbilityDef& def) {
    if (!is_castable_form(def.form)) return std::nullopt;
    if (inst.cooldown_remaining > 0.0f) {
        OrderReject r{"cooldown", "", {}};
        r.args.emplace("cooldown_remaining",
            std::to_string(static_cast<i32>(std::ceil(inst.cooldown_remaining))));
        return r;
    }
    if (!ctx.world) return std::nullopt;
    std::string lacking;
    if (!simulation::ability_can_afford(*ctx.world, unit_id,
                                        def.level_data(inst.level).cost, &lacking)) {
        OrderReject r{"cost", lacking, {}};
        r.args.emplace("resource", lacking);
        return r;
    }
    return std::nullopt;
}

// Can the selected unit trigger this slot's ability right now? Thin bool
// wrapper over slot_cast_reject for the icon-dim / hotkey-gate call sites
// that don't need the reason.
static bool slot_castable_now(const WorldContext& ctx, u32 unit_id,
                              const simulation::AbilityInstance& inst,
                              const simulation::AbilityDef& def) {
    return !slot_cast_reject(ctx, unit_id, inst, def).has_value();
}

// ── Inventory hit-testing ───────────────────────────────────────────────
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

static i32 pickup_bar_hit_test(const Hud::Impl& s, f32 x, f32 y) {
    const auto& cfg = s.pickup_bar_cfg;
    if (!cfg.enabled || !s.pickup_bar_rt.visible) return -1;
    const u32 count = std::min(static_cast<u32>(cfg.slots.size()),
                               static_cast<u32>(s.pickup_bar_rt.entries.size()));
    for (u32 i = 0; i < count; ++i) {
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
    if (simulation::is_null_handle(item) || !s.world_ctx || !s.world_ctx->world) return false;
    const auto* info = s.world_ctx->world->item_infos.get(item.id);
    if (!info) return false;
    out_item = item;
    out_info = info;
    if (s.world_ctx->types) out_def = s.world_ctx->types->get_item_type(info->type_id);
    return true;
}

// can see what's missing.

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
    f32 inv = 1.0f / s.ui_scale;  // ui_scale clamped > 0 at its sole writer (set_ui_scale)
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
    // hit-test them first so clicks beat anything underneath. Priority is the
    // reverse of draw order (pickup_bar > inventory > command_bar > action_bar
    // > minimap > joystick > tree) so the topmost-drawn slot wins where two
    // composites overlap — what you see on top is what you tap. Joystick is
    // LAST of the composites so explicit UI (buttons, bar slots, minimap dots)
    // inside an otherwise-blank activation region still wins. `on_joystick`
    // also fires when slot 0 (the primary finger) has already captured the
    // stick — that suppresses tree / drag-select from the same finger while
    // dragging the knob.
    //
    // Touch hover gate: on mobile, "no finger on screen" means "no
    // hover" — but the platform layer keeps the last touch coords in
    // mouse_x/y after release, so the hit-tests below would otherwise
    // keep resolving to whichever slot the finger lifted from. That
    // breaks tooltip dismissal (won't clear until you tap elsewhere)
    // and falsely treats rapid retaps as a continuous dwell over the
    // same slot (timer never resets between taps, tooltip pops). We
    // allow the hit-test only while a finger is actually down, OR on
    // the release frame itself so the lift-fixup above can still
    // resolve "released over this slot" for click-firing.
    const bool allow_hit_test = !s.is_mobile || button_down || s.pointer_down_prev;
    i32  pickup_slot = allow_hit_test ? pickup_bar_hit_test(s, x, y) : -1;
    i32  inv_slot    = (allow_hit_test && pickup_slot < 0) ? inventory_hit_test(s, x, y) : -1;
    i32  cmd_slot    = (allow_hit_test && pickup_slot < 0 && inv_slot < 0) ? command_bar_hit_test(s, x, y) : -1;
    i32  bar_slot    = (allow_hit_test && pickup_slot < 0 && inv_slot < 0 && cmd_slot < 0)
                         ? action_bar_hit_test(s, x, y) : -1;
    bool on_minimap  = allow_hit_test && pickup_slot < 0 && inv_slot < 0 && cmd_slot < 0 &&
                        bar_slot < 0 && minimap_hit_test(s, x, y);
    bool on_joystick = allow_hit_test && pickup_slot < 0 && inv_slot < 0 && cmd_slot < 0 &&
                       bar_slot < 0 && !on_minimap &&
                       (s.joystick_rt.captured_slot == 0
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
    if (pickup_slot != s.pickup_bar_hover_slot) {
        if (s.pickup_bar_hover_slot >= 0 &&
            static_cast<u32>(s.pickup_bar_hover_slot) < s.pickup_bar_cfg.slots.size()) {
            s.pickup_bar_cfg.slots[s.pickup_bar_hover_slot].hovered = false;
        }
        if (pickup_slot >= 0) {
            s.pickup_bar_cfg.slots[pickup_slot].hovered = true;
        }
        s.pickup_bar_hover_slot = pickup_slot;
    }

    // Tooltip arming. The pointer dwelling on a slot for `delay_ms`
    // pops the tooltip; any change (different slot / left every slot)
    // resets the timer. Desktop uses 250ms — snappy hover. Mobile uses
    // 500ms — AoV-like long-press. On mobile the hit-test gate above
    // makes "no finger down" report no slot, so a finger lift naturally
    // clears the source and the timer; while held, sliding past the
    // slot edges still fails the hit-test and clears it too.
    Impl::TooltipState::Source kind = Impl::TooltipState::Source::None;
    i32 kind_idx = -1;
    if      (bar_slot >= 0)    { kind = Impl::TooltipState::Source::ActionBar;  kind_idx = bar_slot; }
    else if (cmd_slot >= 0)    { kind = Impl::TooltipState::Source::CommandBar; kind_idx = cmd_slot; }
    else if (inv_slot >= 0)    { kind = Impl::TooltipState::Source::Inventory;  kind_idx = inv_slot; }
    else if (pickup_slot >= 0) { kind = Impl::TooltipState::Source::PickupBar;  kind_idx = pickup_slot; }
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
    if (bar_slot < 0 && cmd_slot < 0 && inv_slot < 0 && pickup_slot < 0 &&
        !on_minimap && !on_joystick) {
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
            } else if (target < 0 && s.world_ctx && s.world_ctx->screen_to_world
                       && s.inventory_drop_fn) {
                glm::vec3 wp;
                // Picker takes physical pixels; convert dp → physical.
                f32 sx = x * s.ui_scale;
                f32 sy = y * s.ui_scale;
                if (s.world_ctx->screen_to_world(sx, sy, wp)) {
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
                        s.drag_cast.caster_x    = tf->position.x;
                        s.drag_cast.caster_y    = tf->position.y;
                        s.drag_cast.caster_z    = tf->position.z;
                        s.drag_cast.press_caster_x = tf->position.x;
                        s.drag_cast.press_caster_y = tf->position.y;
                        s.drag_cast.press_caster_z = tf->position.z;
                        s.drag_cast.world_anchored = (s.preset != "action");
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
                        s.drag_cast.caster_x    = tf->position.x;
                        s.drag_cast.caster_y    = tf->position.y;
                        s.drag_cast.caster_z    = tf->position.z;
                        s.drag_cast.press_caster_x = tf->position.x;
                        s.drag_cast.press_caster_y = tf->position.y;
                        s.drag_cast.press_caster_z = tf->position.z;
                        s.drag_cast.world_anchored = (s.preset != "action");
                        s.drag_cast.drag_world_x = tf->position.x;
                        s.drag_cast.drag_world_y = tf->position.y;
                        s.drag_cast.drag_world_z = tf->position.z;
                        s.drag_cast.ability_id.clear();
                        s.drag_cast.command_id  = slot.command;
                        s.drag_cast.range       = 0;
                        // Both Move and Attack snap to widgets on mobile:
                        // units AND destructables (crates). Move-on-unit →
                        // Follow; Attack-on-unit/crate → Attack (friendly fire
                        // / debris allowed); release on ground falls back to
                        // the point-target order. Hybrid form (widget-accepting
                        // + point fall-through) so the snap loop runs and
                        // release on ground commits the point order. The snap
                        // loop applies attackability (won't snap trees).
                        s.drag_cast.form        = simulation::AbilityForm::Target;
                        s.drag_cast.widget_kinds = simulation::widget_kind::Unit
                                                 | simulation::widget_kind::Destructable;
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
                        s.drag_cast.caster_x    = tf->position.x;
                        s.drag_cast.caster_y    = tf->position.y;
                        s.drag_cast.caster_z    = tf->position.z;
                        s.drag_cast.press_caster_x = tf->position.x;
                        s.drag_cast.press_caster_y = tf->position.y;
                        s.drag_cast.press_caster_z = tf->position.z;
                        s.drag_cast.world_anchored = (s.preset != "action");
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
        } else if (pickup_slot >= 0) {
            s.pickup_bar_pressed_slot = pickup_slot;
            s.pickup_bar_cfg.slots[pickup_slot].pressed = true;
        } else if (on_minimap) {
            // Minimap press — start a drag that pans the camera. Each
            // pointer move while held re-fires minimap_jump_fn (handled
            // below the press/release edges); release clears the latch.
            // Silently ignore if the session isn't fully up yet.
            if (s.world_ctx && s.world_ctx->terrain && s.minimap_jump_fn) {
                s.minimap_dragging = true;
                f32 wx = 0.0f, wy = 0.0f;
                minimap_screen_to_world(s.minimap_cfg.rect, *s.world_ctx->terrain,
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
        // Touch long-press lifts an item with the finger still down, so the
        // drop commits here on release rather than on the next press edge.
        if (s.is_mobile && s.held_item_slot >= 0) {
            i32 target = inv_slot;
            if (target >= 0 && target != s.held_item_slot) {
                if (s.inventory_swap_fn) s.inventory_swap_fn(s.held_item_slot, target);
            } else if (target < 0 && s.world_ctx && s.world_ctx->screen_to_world
                       && s.inventory_drop_fn) {
                glm::vec3 wp;
                if (s.world_ctx->screen_to_world(x * s.ui_scale, y * s.ui_scale, wp)) {
                    s.inventory_drop_fn(s.held_item_id, s.held_item_slot, wp);
                }
            }
            s.held_item_slot = -1;
            s.held_item_id   = UINT32_MAX;
            s.held_item_icon.clear();
            s.pointer_down_prev = button_down;
            return;
        }
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
                        if (unit_id != UINT32_MAX) {
                            if (auto rej = slot_cast_reject(*s.world_ctx, unit_id, *inst, *def)) {
                                emit_order_error(rej->base, rej->specifier, rej->args);
                            } else {
                                s.action_bar_cast_fn(inst->ability_id);
                            }
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
        } else if (s.pickup_bar_pressed_slot >= 0) {
            u32 idx = static_cast<u32>(s.pickup_bar_pressed_slot);
            bool over = pickup_slot == s.pickup_bar_pressed_slot;
            if (idx < s.pickup_bar_cfg.slots.size()) {
                s.pickup_bar_cfg.slots[idx].pressed = false;
            }
            if (over && s.pickup_fn && idx < s.pickup_bar_rt.entries.size()) {
                const auto entry = s.pickup_bar_rt.entries[idx];
                if (s.world_ctx && s.world_ctx->world &&
                    s.world_ctx->world->contains(entry.unit) &&
                    s.world_ctx->world->contains(entry.item)) {
                    s.pickup_fn(entry.unit, entry.item);
                }
            }
            s.pickup_bar_pressed_slot = -1;
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
            minimap_screen_to_world(s.minimap_cfg.rect, *s.world_ctx->terrain,
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
        || m_impl->pickup_bar_hover_slot    >= 0
        || m_impl->pickup_bar_pressed_slot  >= 0
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


void Hud::set_marquee_style(const MarqueeStyle& style) {
    if (m_impl) m_impl->marquee_style = style;
}

const Hud::MarqueeStyle& Hud::marquee_style() const {
    static const MarqueeStyle kEmpty{};
    return m_impl ? m_impl->marquee_style : kEmpty;
}

} // namespace uldum::hud
