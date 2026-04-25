#include "hud/world.h"
#include "hud/hud.h"
#include "hud/text_tag.h"

#include "simulation/world.h"
#include "simulation/components.h"
#include "simulation/fog_of_war.h"
#include "simulation/type_registry.h"
#include "render/camera.h"
#include "input/picking.h"
#include "input/selection.h"
#include "map/terrain_data.h"
#include "core/log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include <algorithm>

namespace uldum::hud {

static constexpr const char* TAG = "HUD.World";

// Config + context storage. We keep these in static function-local storage
// per-Hud-instance by storing them inside Hud::Impl — see hud.cpp. For now,
// pass them in as arguments from hud.cpp's draw_world_overlays_impl.

// Visibility test for a given bar template given a unit's state fraction,
// hover, and selection flags.
static bool passes_visibility(const std::vector<VisibilityPolicy>& policies,
                              f32  fraction,
                              bool is_hovered,
                              bool is_selected) {
    if (policies.empty()) return true;  // default Always
    for (auto p : policies) {
        switch (p) {
            case VisibilityPolicy::Always:   return true;
            case VisibilityPolicy::NotFull:  if (fraction < 1.0f) return true; break;
            case VisibilityPolicy::Hovered:  if (is_hovered)      return true; break;
            case VisibilityPolicy::Selected: if (is_selected)     return true; break;
        }
    }
    return false;
}

// Look up a state's (current, max) from World. Returns false if the entity
// doesn't have this state (so the bar for it is skipped).
static bool read_state(const simulation::World& world, u32 id,
                       const std::string& state_id,
                       f32& current, f32& max) {
    if (state_id == "health") {
        const auto* h = world.healths.get(id);
        if (!h || h->max <= 0.0f) return false;
        current = h->current;
        max     = h->max;
        return true;
    }
    const auto* block = world.state_blocks.get(id);
    if (!block) return false;
    auto it = block->states.find(state_id);
    if (it == block->states.end() || it->second.max <= 0.0f) return false;
    current = it->second.current;
    max     = it->second.max;
    return true;
}

// Internal: project a world-space point to screen pixels. Returns false if
// the point is behind the camera (w <= 0) or outside the viewport.
static bool project_to_screen(const glm::mat4& vp, glm::vec3 world,
                              u32 screen_w, u32 screen_h,
                              f32& out_x, f32& out_y) {
    glm::vec4 clip = vp * glm::vec4(world, 1.0f);
    if (clip.w <= 0.001f) return false;  // behind camera
    f32 ndc_x = clip.x / clip.w;
    f32 ndc_y = clip.y / clip.w;
    // Vulkan NDC: [-1, 1] with y down. Map to viewport pixels with y down.
    out_x = (ndc_x * 0.5f + 0.5f) * static_cast<f32>(screen_w);
    out_y = (ndc_y * 0.5f + 0.5f) * static_cast<f32>(screen_h);
    // Reject anything fully outside the viewport — saves the batcher work.
    if (out_x < -100.0f || out_x > static_cast<f32>(screen_w) + 100.0f) return false;
    if (out_y < -100.0f || out_y > static_cast<f32>(screen_h) + 100.0f) return false;
    return true;
}

// Is the unit at (world_x, world_y) hidden by fog for `player`?
static bool is_fogged(const simulation::FogOfWar& fog,
                      const map::TerrainData& terrain,
                      simulation::Player player,
                      f32 world_x, f32 world_y) {
    if (!fog.enabled()) return false;
    // Terrain is centered; its world_to_tile handles that.
    auto tile = terrain.world_to_tile(world_x, world_y);
    u32 tx = static_cast<u32>(tile.x);
    u32 ty = static_cast<u32>(tile.y);
    if (tx >= fog.tiles_x() || ty >= fog.tiles_y()) return false;
    return !fog.is_visible(player, tx, ty);
}

// Internal implementation — called from hud.cpp's draw_world_overlays
// after it plucks the config + context out of Impl. Kept here so hud.cpp
// doesn't need to include simulation / render / input / map.
void draw_entity_bars_impl(Hud& hud,
                           u32 screen_w, u32 screen_h,
                           const WorldOverlayConfig& cfg,
                           const WorldContext& ctx,
                           f32 alpha) {
    if (!cfg.entity_bars_enabled)      return;
    if (!ctx.world || !ctx.camera)     return;
    if (cfg.entity_bars.bars.empty())  return;

    const auto& ebc   = cfg.entity_bars;
    const auto& world = *ctx.world;
    const glm::mat4 vp = ctx.camera->view_projection();

    const map::TerrainData* terrain = ctx.terrain;

    // Optional: current hovered unit (for Hovered visibility policy).
    u32 hovered_id = UINT32_MAX;
    if (ctx.picker) {
        // TODO: picker would need mouse coords injected; for now assume
        // the picker tracks its own state or we skip hover until wired.
    }

    // Optional: selected ids (for Selected visibility policy).
    const input::SelectionState* selection = ctx.selection;

    // Walk all entities with a Transform.
    auto ids  = world.transforms.ids();
    auto data = world.transforms.data();
    u32 count = world.transforms.count();
    for (u32 i = 0; i < count; ++i) {
        u32 id = ids[i];
        const auto& tf = data[i];

        // Skip fogged entities. Skip entities that are not units (we only
        // draw bars for units with health/state blocks — the checks below
        // filter non-unit entities naturally via state lookup).
        if (ctx.fog && terrain && is_fogged(*ctx.fog, *terrain, ctx.local_player,
                                            tf.position.x, tf.position.y)) continue;
        // Skip dead units — their corpse is still in the world but shouldn't
        // advertise HP/mana bars.
        if (world.dead_states.get(id)) continue;

        // Interpolated world position for the bar anchor — matches how
        // the renderer (and selection circles) draw moving units so the
        // bar tracks smoothly instead of snapping per tick.
        glm::vec3 anchor_world = tf.interp_position(alpha)
                               + glm::vec3(0.0f, 0.0f, ebc.z_offset);
        f32 cx = 0.0f, cy = 0.0f;
        if (!project_to_screen(vp, anchor_world, screen_w, screen_h, cx, cy)) continue;

        bool is_hovered  = (hovered_id == id);
        bool is_selected = false;
        if (selection) {
            for (const auto& u : selection->selected()) {
                if (u.id == id) { is_selected = true; break; }
            }
        }

        // Stack bars vertically starting at (cx, cy). Each bar is centered
        // horizontally on cx; the first bar's top sits at cy; subsequent
        // bars are spaced below.
        f32 stack_y = cy;
        for (const auto& bar : ebc.bars) {
            f32 cur = 0.0f, max = 0.0f;
            if (!read_state(world, id, bar.state_id, cur, max)) continue;

            f32 fraction = (max > 0.0f) ? (cur / max) : 0.0f;
            if (fraction < 0.0f) fraction = 0.0f;
            if (fraction > 1.0f) fraction = 1.0f;

            if (!passes_visibility(bar.visibility, fraction, is_hovered, is_selected)) {
                // Don't advance stack_y — the bar slot is omitted entirely
                // (other bars in the stack close up). Matches WC3: if mana
                // bar is hidden the health bar is at its normal position.
                continue;
            }

            Rect bg_rect{
                cx - static_cast<f32>(ebc.width) * 0.5f,
                stack_y,
                static_cast<f32>(ebc.width),
                static_cast<f32>(ebc.height)
            };
            hud.draw_rect(bg_rect, bar.style.bg_color);

            Rect fill_rect = bg_rect;
            fill_rect.w = bg_rect.w * fraction;
            if (fill_rect.w > 0.0f) hud.draw_rect(fill_rect, bar.style.fill_color);

            stack_y += static_cast<f32>(ebc.height) + static_cast<f32>(ebc.spacing);
        }
    }
}

// ── Hover-triggered unit name label ───────────────────────────────────────
// Asks Picker for the unit under the cursor; if one exists, isn't fogged,
// and its type defines a non-empty `display_name`, draws a centered label
// (with optional bg pill) above the unit. Single label per frame.

void draw_unit_name_label_impl(Hud& hud,
                               u32 screen_w, u32 screen_h,
                               const WorldOverlayConfig& cfg,
                               const WorldContext& ctx,
                               f32 alpha) {
    if (!cfg.name_label_enabled) return;
    if (!ctx.world || !ctx.camera || !ctx.picker || !ctx.types) return;

    f32 mx = hud.pointer_x();
    f32 my = hud.pointer_y();
    if (mx <= 0.0f && my <= 0.0f) return;  // no pointer seen this frame

    // Picker still operates in physical framebuffer pixels (its screen
    // size is set from m_platform->width/height). HUD pointer coords
    // were divided by ui_scale on entry to live in dp space — convert
    // back so the picker projects correctly. On desktop ui_scale ≈ 1
    // so this is a no-op; on Android xxhdpi (~2.6×) it matters a lot.
    f32 s = hud.ui_scale();
    if (s <= 0.0f) s = 1.0f;
    f32 picker_x = mx * s;
    f32 picker_y = my * s;

    simulation::Unit hovered = ctx.picker->pick_target(picker_x, picker_y);
    if (hovered.id == simulation::Unit{}.id) return;  // nothing under cursor

    const auto& world = *ctx.world;
    const auto* tf    = world.transforms.get(hovered.id);
    const auto* hinfo = world.handle_infos.get(hovered.id);
    if (!tf || !hinfo) return;
    if (world.dead_states.get(hovered.id)) return;
    if (ctx.fog && ctx.terrain &&
        is_fogged(*ctx.fog, *ctx.terrain, ctx.local_player,
                  tf->position.x, tf->position.y)) {
        return;
    }

    const auto* type_def = ctx.types->get_unit_type(hinfo->type_id);
    if (!type_def || type_def->display_name.empty()) return;
    const std::string& text = type_def->display_name;

    // Project anchor to screen.
    glm::vec3 anchor_world = tf->interp_position(alpha)
                           + glm::vec3(0.0f, 0.0f, cfg.name_label.z_offset);
    const glm::mat4 vp = ctx.camera->view_projection();
    f32 cx = 0.0f, cy = 0.0f;
    if (!project_to_screen(vp, anchor_world, screen_w, screen_h, cx, cy)) return;

    const auto& nl = cfg.name_label;
    f32 text_w     = hud.text_width_px(text, nl.px_size);
    f32 line_h     = hud.text_line_height_px(nl.px_size);
    f32 ascent     = hud.text_ascent_px(nl.px_size);

    // Center text horizontally at cx; anchor the text's bottom of cap
    // height near cy (label sits centered around the projected point).
    f32 x_left     = cx - text_w * 0.5f;
    f32 y_baseline = cy + ascent - line_h * 0.5f;

    if (nl.style.has_bg && text_w > 0.0f && line_h > 0.0f) {
        Rect bg_rect{
            cx - text_w * 0.5f - nl.style.bg_pad_x,
            cy - line_h * 0.5f - nl.style.bg_pad_y,
            text_w + 2.0f * nl.style.bg_pad_x,
            line_h + 2.0f * nl.style.bg_pad_y
        };
        hud.draw_rect(bg_rect, nl.style.bg_color);
    }
    hud.draw_text(x_left, y_baseline, text, nl.style.color, nl.px_size);
}

} // namespace uldum::hud
