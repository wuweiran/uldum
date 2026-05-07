#include "input/action_preset.h"
#include "hud/hud.h"
#include "simulation/order.h"
#include "simulation/ability_def.h"
#include "simulation/world.h"
#include "core/log.h"

#include <cmath>

namespace uldum::input {

static constexpr const char* TAG = "ActionInput";

void ActionPreset::update(const InputContext& ctx, f32 /*dt*/) {
    // Targeting first so a click commits the armed ability before any
    // other handler sees it. Movement runs only when the click wasn't
    // consumed — otherwise the click frame's "no WASD" reading would
    // emit a Stop and override the Cast we just submitted. Queued
    // ability flushes last; camera follow runs after so it sees any
    // position change the tick will produce.
    bool click_consumed = handle_targeting(ctx);
    if (!click_consumed) handle_focus_click(ctx);
    if (!click_consumed) handle_movement(ctx);

    if (!m_pending_ability.empty()) {
        std::string id = std::move(m_pending_ability);
        m_pending_ability.clear();
        dispatch_ability(ctx, id, false);
    }

    if (!m_pending_command.empty()) {
        std::string id = std::move(m_pending_command);
        m_pending_command.clear();
        // Action preset's command bar fires through here. Targets resolve
        // via the HUD's focus_target so a tap on "Attack" switches the
        // unit to whatever the reticle is on. Without focus we drop the
        // command — desktop attack-targeting via "click slot, then click
        // world" isn't part of the Action preset's mental model.
        if (id == "stop") {
            GameCommand cmd;
            cmd.player = ctx.selection.player();
            cmd.units  = ctx.selection.selected();
            cmd.order  = simulation::orders::Stop{};
            ctx.commands.submit(cmd);
        } else if (id == "hold_position") {
            GameCommand cmd;
            cmd.player = ctx.selection.player();
            cmd.units  = ctx.selection.selected();
            cmd.order  = simulation::orders::HoldPosition{};
            ctx.commands.submit(cmd);
        } else if ((id == "attack" || id == "attack_move") && ctx.hud) {
            auto focus = ctx.hud->focus_target();
            if (focus.is_valid() && ctx.simulation.world().validate(focus) &&
                !ctx.selection.empty()) {
                GameCommand cmd;
                cmd.player = ctx.selection.player();
                cmd.units  = ctx.selection.selected();
                cmd.order  = simulation::orders::Attack{focus};
                ctx.commands.submit(cmd);
            }
        }
    }

    handle_camera_gestures(ctx);
    handle_camera_follow(ctx);
}

void ActionPreset::queue_ability(std::string_view ability_id) {
    m_pending_ability.assign(ability_id);
}

void ActionPreset::queue_command(std::string_view command_id) {
    m_pending_command.assign(command_id);
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
    if (mag < 0.001f) return;     // no input — emit nothing
    if (mag > 1.0f)   dir /= mag;  // cap at unit length

    // Don't translate input into MoveDirection while the lead unit is
    // committed to a cast. Phases:
    //   - None              → not casting, emit normally.
    //   - MovingToTarget    → unit walking to the cast point; the
    //                         player redirecting with the stick is
    //                         meant to override the approach, so we
    //                         emit MoveDirection (which cancels the
    //                         cast in issue_order, same as any new
    //                         order would).
    //   - TurningToFace /
    //     CastPoint /
    //     Backswing         → cast is committed; suppress emission
    //                         until it finishes. When cast_state
    //                         returns to None next tick, normal
    //                         emission resumes if the player is still
    //                         holding the stick.
    auto lead = sel.selected().front();
    auto& world = ctx.simulation.world();
    auto* aset = world.ability_sets.get(lead.id);
    if (aset) {
        if (aset->cast_state != simulation::CastState::None &&
            aset->cast_state != simulation::CastState::MovingToTarget) {
            return;
        }
    }

    // Active intent in the unit's order queue takes precedence over
    // the joystick. The HUD's drag-cast / attack-button taps write
    // oq->current synchronously via issue_order this same render
    // frame, before handle_movement runs. Without this gate, the
    // joystick's MoveDirection would clobber the just-issued Cast /
    // Attack / AttackMove on the next sim tick.
    //
    // Only checks while cast_state == None; once the sim begins
    // processing a cast and enters MovingToTarget, the player driving
    // the stick is meant to override the approach (and the cast_state
    // gate above already lets MovingToTarget through).
    if (aset && aset->cast_state == simulation::CastState::None) {
        if (auto* oq = world.order_queues.get(lead.id)) {
            if (oq->current.has_value()) {
                const auto& payload = oq->current->payload;
                if (std::get_if<simulation::orders::Cast>(&payload)       ||
                    std::get_if<simulation::orders::Attack>(&payload)     ||
                    std::get_if<simulation::orders::AttackMove>(&payload)) {
                    return;
                }
            }
        }
    }

    // Emit a fresh MoveDirection each render frame the player is
    // holding movement input. The sim consumes the order in one tick
    // (system_movement clears oq->current after applying), so when the
    // player releases keys this function stops emitting and the unit
    // naturally idles next tick. No latch tracking, no Stop emission.
    GameCommand cmd;
    cmd.player = sel.player();
    cmd.units  = sel.selected();
    cmd.order  = simulation::orders::MoveDirection{dir};
    ctx.commands.submit(cmd);
}

// ── Ability targeting ────────────────────────────────────────────────────
// Mirrors RtsPreset: once armed (from dispatch_ability), the next left-
// click commits the cast on that target (or ground point). Esc cancels.
// Gated by hud_captured so clicking a different slot while armed doesn't
// double-fire.

bool ActionPreset::handle_targeting(const InputContext& ctx) {
    auto& input = ctx.input;
    auto& sel   = ctx.selection;
    if (!m_targeting_ability) return false;

    if (ctx.hud_captured) {
        if (input.key_escape) {
            m_targeting_ability = false;
            m_targeting_ability_id.clear();
        }
        return false;
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
        return true;
    }

    if (input.key_escape) {
        m_targeting_ability = false;
        m_targeting_ability_id.clear();
    }
    return false;
}

// ── Focus-target world-click ─────────────────────────────────────────────
// When no ability is armed, a left-click in the world either locks focus
// on the clicked unit (manual override) or releases the lock by clicking
// empty terrain. The reticle + tap-fire ability resolution both read
// HUD-side focus_target.

void ActionPreset::handle_focus_click(const InputContext& ctx) {
    if (m_targeting_ability) return;            // armed cast owns the click
    if (ctx.hud_captured) return;               // pointer is over a HUD widget
    if (!ctx.hud) return;                       // no HUD = no focus state to set
    auto& input = ctx.input;
    if (!input.mouse_left_pressed) return;

    // Pick a unit at the click point. The picker returns an invalid
    // handle if no unit was hit (terrain or void).
    auto target = ctx.picker.pick_target(input.mouse_x, input.mouse_y);
    if (target.is_valid()) {
        // Don't lock focus on the hero itself — useless and would make
        // the reticle sit on the player.
        auto& sel = ctx.selection;
        if (!sel.empty() && target.id == sel.selected().front().id) return;
        ctx.hud->set_focus_target(target);
    } else {
        // Click on empty terrain — release any manual lock so auto
        // resumes choosing.
        ctx.hud->clear_focus_target();
    }
}

// ── Camera gestures ──────────────────────────────────────────────────────
// Mobile two-finger pan + pinch zoom (mirroring RtsPreset). The minimap
// drag path is owned by the HUD and moves the camera through the
// minimap_jump_fn callback; here we only need to know it's happening so
// handle_camera_follow can skip its snap-to-hero step.

void ActionPreset::handle_camera_gestures(const InputContext& ctx) {
    auto& input = ctx.input;
    auto& camera = ctx.camera;

    m_camera_user_panning = false;

    // Two-finger pan/pinch only when neither finger is occupying a HUD
    // widget. Without this gate, "joystick + ability button" lands two
    // touches on the screen and the centroid jitter from the player
    // tapping the ability gets read as a pan delta — the camera lurches
    // on every cast. hud_joystick_active covers the joystick (any
    // finger); hud_captured covers the rest of the HUD. If either is
    // set, fall through to the m_had_two_finger reset below so the
    // gesture doesn't re-engage with stale centroid state when the
    // player lifts the HUD finger.
    bool hud_in_use = ctx.hud_joystick_active || ctx.hud_captured;
    if (input.touch_count >= 2 && !hud_in_use) {
        f32 cx = 0.5f * (input.touch_x[0] + input.touch_x[1]);
        f32 cy = 0.5f * (input.touch_y[0] + input.touch_y[1]);
        f32 dx = input.touch_x[0] - input.touch_x[1];
        f32 dy = input.touch_y[0] - input.touch_y[1];
        f32 dist = std::sqrt(dx * dx + dy * dy);
        if (m_had_two_finger) {
            f32 pdx = cx - m_prev_centroid_x;
            f32 pdy = cy - m_prev_centroid_y;
            camera.pan(pdx, pdy);

            constexpr f32 PINCH_SCALE = 0.02f;
            f32 zoom_delta = (dist - m_prev_pinch_dist) * PINCH_SCALE;
            if (zoom_delta != 0.0f) camera.zoom(zoom_delta);
        }
        m_prev_centroid_x = cx;
        m_prev_centroid_y = cy;
        m_prev_pinch_dist = dist;
        m_had_two_finger  = true;
        m_camera_user_panning = true;
        return;
    }
    m_had_two_finger = false;

    if (ctx.hud_minimap_dragging) {
        m_camera_user_panning = true;
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
    // Suspend follow while the player is actively gesturing the camera
    // (two-finger pan/pinch, or minimap drag). Snap-to-hero resumes the
    // frame after they release.
    if (m_camera_user_panning) return;

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
