#include "input/action_preset.h"
#include "simulation/order.h"
#include "simulation/ability_def.h"
#include "core/log.h"

#include <cmath>

namespace uldum::input {

static constexpr const char* TAG = "ActionInput";

void ActionPreset::update(const InputContext& ctx, f32 /*dt*/) {
    // Ordering mirrors RtsPreset: run targeting-mode resolution first
    // (so a click commits the armed ability before anything else sees
    // that same click), then attack clicks, then movement, then the
    // queued-ability flush last. Camera follow runs at the end so it
    // sees any position change the tick will produce.
    handle_targeting(ctx);
    handle_attack_click(ctx);
    handle_movement(ctx);

    if (!m_pending_ability.empty()) {
        std::string id = std::move(m_pending_ability);
        m_pending_ability.clear();
        dispatch_ability(ctx, id, false);
    }

    handle_camera_follow(ctx);
}

void ActionPreset::queue_ability(std::string_view ability_id) {
    m_pending_ability.assign(ability_id);
}

// ── Movement ─────────────────────────────────────────────────────────────
// WASD in world axes (W=+Y, S=-Y, D=+X, A=-X). Diagonals normalize so the
// hero isn't faster on the diagonal. Camera-relative movement would need
// the current yaw projected in, but Action preset assumes a fixed-pose
// camera so world-axis WASD reads as "up / down / left / right on screen".

void ActionPreset::handle_movement(const InputContext& ctx) {
    auto& input = ctx.input;
    auto& sel   = ctx.selection;
    if (sel.empty()) return;

    glm::vec2 dir{0.0f, 0.0f};
    if (input.key_letter['W' - 'A']) dir.y += 1.0f;
    if (input.key_letter['S' - 'A']) dir.y -= 1.0f;
    if (input.key_letter['D' - 'A']) dir.x += 1.0f;
    if (input.key_letter['A' - 'A']) dir.x -= 1.0f;

    // Virtual stick — screen Y grows downward, world +Y is forward, so
    // flip Y when folding the stick into movement. X maps 1:1. Stick
    // magnitude already encodes speed (after deadzone rescale in
    // joystick_update), so we add the raw components to the key input
    // and only normalize when the sum exceeds unit length — keys +
    // stick can't drive faster than full speed.
    dir.x += ctx.joystick_x;
    dir.y += -ctx.joystick_y;

    f32 mag = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (mag > 1.0f)        dir /= mag;     // cap at unit length
    else if (mag < 0.001f) dir = {0.0f, 0.0f};

    // Re-issue only on change. MoveDirection is latched — the sim keeps
    // applying the direction tick after tick without further commands.
    const f32 eps = 1e-4f;
    bool same = m_last_move_valid
             && std::fabs(dir.x - m_last_move_dir.x) < eps
             && std::fabs(dir.y - m_last_move_dir.y) < eps;
    if (same) return;

    GameCommand cmd;
    cmd.player = sel.player();
    cmd.units  = sel.selected();
    if (dir.x == 0.0f && dir.y == 0.0f) {
        // Release → Stop clears the latched MoveDirection so the hero
        // halts. (Stop also cancels any other order, which is what we
        // want here — the preset doesn't stack orders.)
        cmd.order = simulation::orders::Stop{};
    } else {
        cmd.order = simulation::orders::MoveDirection{dir};
    }
    ctx.commands.submit(cmd);
    m_last_move_dir   = dir;
    m_last_move_valid = true;
}

// ── Primary attack ───────────────────────────────────────────────────────
// Left-click on a unit → Attack that unit. On ground → AttackMove to the
// ground point, so the hero will engage anything it runs into on the way.
// Skipped when the HUD captures the pointer (slot click) or targeting
// mode owns the click (handled separately in handle_targeting).

void ActionPreset::handle_attack_click(const InputContext& ctx) {
    if (ctx.hud_captured) return;
    if (m_targeting_ability) return;
    auto& input = ctx.input;
    auto& sel   = ctx.selection;
    if (!input.mouse_left_pressed || sel.empty()) return;

    auto target = ctx.picker.pick_target(input.mouse_x, input.mouse_y);
    GameCommand cmd;
    cmd.player = sel.player();
    cmd.units  = sel.selected();
    if (target.is_valid()) {
        cmd.order = simulation::orders::Attack{simulation::Unit{target}};
    } else {
        glm::vec3 world_pos;
        if (!ctx.picker.screen_to_world(input.mouse_x, input.mouse_y, world_pos)) return;
        cmd.order = simulation::orders::AttackMove{world_pos};
    }
    cmd.queued = input.key_shift;
    ctx.commands.submit(cmd);
    // Attack command overrides any latched movement — force a re-issue
    // next time WASD changes so we don't skip the edge.
    m_last_move_valid = false;
}

// ── Ability targeting ────────────────────────────────────────────────────
// Mirrors RtsPreset: once armed (from dispatch_ability), the next left-
// click commits the cast on that target (or ground point). Esc cancels.
// Gated by hud_captured so clicking a different slot while armed doesn't
// double-fire.

void ActionPreset::handle_targeting(const InputContext& ctx) {
    auto& input = ctx.input;
    auto& sel   = ctx.selection;
    if (!m_targeting_ability) return;

    if (ctx.hud_captured) {
        if (input.key_escape) {
            m_targeting_ability = false;
            m_targeting_ability_id.clear();
        }
        return;
    }

    if (input.mouse_left_pressed && !sel.empty()) {
        const auto* def = ctx.simulation.abilities().get(m_targeting_ability_id);
        if (def) {
            if (def->form == simulation::AbilityForm::TargetUnit) {
                auto target = ctx.picker.pick_target(input.mouse_x, input.mouse_y);
                if (target.is_valid()) {
                    GameCommand cmd;
                    cmd.player = sel.player();
                    cmd.units  = sel.selected();
                    cmd.order  = simulation::orders::Cast{m_targeting_ability_id, target, {}};
                    cmd.queued = input.key_shift;
                    ctx.commands.submit(cmd);
                }
            } else if (def->form == simulation::AbilityForm::TargetPoint) {
                glm::vec3 world_pos;
                if (ctx.picker.screen_to_world(input.mouse_x, input.mouse_y, world_pos)) {
                    GameCommand cmd;
                    cmd.player = sel.player();
                    cmd.units  = sel.selected();
                    cmd.order  = simulation::orders::Cast{m_targeting_ability_id, {}, world_pos};
                    cmd.queued = input.key_shift;
                    ctx.commands.submit(cmd);
                }
            }
        }
        m_targeting_ability = false;
        m_targeting_ability_id.clear();
        m_last_move_valid = false;   // resume WASD next change
        return;
    }

    if (input.key_escape) {
        m_targeting_ability = false;
        m_targeting_ability_id.clear();
    }
}

// ── Camera follow ────────────────────────────────────────────────────────
// Each frame, translate the camera so its ground-focus point coincides
// with the controlled hero's XY. Pitch, yaw, and height (Z) are
// preserved — the map author's authored pose just slides sideways as
// the hero moves. No interpolation against sim alpha yet; at 16 Hz
// tick this reads as acceptable for a test bed and matches how the
// minimap-jump works.

void ActionPreset::handle_camera_follow(const InputContext& ctx) {
    auto& sel = ctx.selection;
    if (sel.empty()) return;

    const auto& world = ctx.simulation.world();
    u32 lead_id = sel.selected().front().id;
    const auto* tf = world.transforms.get(lead_id);
    if (!tf) return;

    auto& cam = ctx.camera;
    glm::vec3 pos = cam.position();
    glm::vec3 fwd = cam.forward_dir();
    // Need a forward ray that hits the ground — otherwise we have no
    // way to compute the focus point. A near-horizontal camera would
    // degenerate here; skip follow in that case.
    if (fwd.z >= -0.001f) return;

    f32 t = -pos.z / fwd.z;
    glm::vec3 focus  = pos + t * fwd;
    glm::vec3 offset = pos - focus;

    // Use the same interp position the renderer is about to draw this
    // unit at so camera + unit stay pixel-aligned across the sub-tick.
    glm::vec3 hero_interp = tf->interp_position(ctx.alpha);
    glm::vec3 hero_xy{ hero_interp.x, hero_interp.y, 0.0f };
    cam.set_pose(hero_xy + offset, cam.pitch(), cam.yaw());
}

void ActionPreset::dispatch_ability(const InputContext& ctx,
                                    std::string_view ability_id,
                                    bool queued_modifier) {
    auto& sel = ctx.selection;
    if (sel.empty()) return;

    auto& world = ctx.simulation.world();
    u32 lead_id = sel.selected().front().id;
    const auto* aset = world.ability_sets.get(lead_id);
    if (!aset) return;
    bool owns = false;
    for (const auto& inst : aset->abilities) {
        if (inst.ability_id == ability_id) { owns = true; break; }
    }
    if (!owns) return;

    const auto* def = ctx.simulation.abilities().get(std::string(ability_id));
    if (!def) return;

    if (def->form == simulation::AbilityForm::Instant ||
        def->form == simulation::AbilityForm::Toggle) {
        GameCommand cmd;
        cmd.player = sel.player();
        cmd.units  = sel.selected();
        cmd.order  = simulation::orders::Cast{std::string(ability_id), {}, {}};
        cmd.queued = queued_modifier;
        ctx.commands.submit(cmd);
    } else if (def->form == simulation::AbilityForm::TargetUnit ||
               def->form == simulation::AbilityForm::TargetPoint) {
        m_targeting_ability    = true;
        m_targeting_ability_id = ability_id;
    }
    // Passive / Aura / Channel: not directly triggerable from here.
}

} // namespace uldum::input
