#include "input/rts_preset.h"
#include "simulation/order.h"
#include "simulation/ability_def.h"
#include "core/log.h"

#include <cmath>

namespace uldum::input {

static constexpr const char* TAG = "RtsInput";

// Helper: check if key just pressed this frame (rising edge) — for hardcoded keys only
static bool key_pressed(bool current, bool& prev) {
    bool pressed = current && !prev;
    prev = current;
    return pressed;
}

void RtsPreset::update(const InputContext& ctx, f32 dt) {
    handle_selection(ctx);
    handle_orders(ctx);
    handle_hotkeys(ctx);
    // Service any HUD-button cast requests queued last frame. Runs after
    // handle_orders so a targeting-mode click started last frame still
    // resolves first, and before handle_camera because this call may
    // submit a command or enter targeting mode that subsequent frame-
    // based logic should see.
    if (!m_pending_ability.empty()) {
        std::string id = std::move(m_pending_ability);
        m_pending_ability.clear();
        dispatch_ability(ctx, id, false);
    }
    if (!m_pending_command.empty()) {
        std::string id = std::move(m_pending_command);
        m_pending_command.clear();
        dispatch_command(ctx, id);
    }
    handle_camera(ctx, dt);
}

void RtsPreset::queue_ability(std::string_view ability_id) {
    m_pending_ability.assign(ability_id);
}

void RtsPreset::queue_command(std::string_view command_id) {
    m_pending_command.assign(command_id);
}

// ── Selection ────────────────────────────────────────────────────────────

void RtsPreset::handle_selection(const InputContext& ctx) {
    auto& input = ctx.input;
    auto& sel = ctx.selection;

    // A drag-select is a real thing only when ALL of these are true:
    //   - left button / primary touch is held,
    //   - not in an ability-targeting or attack-move mode,
    //   - the HUD isn't claiming the pointer (slot / minimap / menu),
    //   - we're not in a multi-touch gesture (pan / pinch).
    // Any of these flipping mid-drag cancels the drag. A press that
    // arrived while any were true is suppressed through to release —
    // that's what stops a click-to-cast (or a 2-finger pan) from
    // flashing a marquee once the blocking condition clears but the
    // finger is still down.
    bool targeting    = m_attack_move_mode
                      || m_move_targeting_mode
                      || m_targeting_ability;
    bool multi_touch  = input.touch_count >= 2;
    bool drag_blocked = targeting || ctx.hud_captured || multi_touch;

    // Release always runs so the suppression latch + drag state can
    // clear regardless of whether we're still over the HUD etc.
    if (input.mouse_left_released) {
        m_suppress_drag_until_release = false;
    }

    // Press edge: prime a fresh drag, or arm suppression.
    if (input.mouse_left_pressed) {
        if (drag_blocked) {
            m_suppress_drag_until_release = true;
        } else {
            m_box_dragging = false;
            m_box_start_x = input.mouse_x;
            m_box_start_y = input.mouse_y;
            m_suppress_drag_until_release = false;
        }
    }

    // Mid-drag cancel: user started a valid drag, then a blocker
    // appeared (e.g. second finger landed, or pointer entered the HUD).
    // Stop dragging and latch suppression so the marquee doesn't
    // reappear when the blocker clears while the press is still held.
    if (m_box_dragging && drag_blocked) {
        m_box_dragging = false;
        m_suppress_drag_until_release = true;
    }

    // Threshold promotion from "press down" to "dragging".
    if (input.mouse_left && !m_box_dragging && !drag_blocked
        && !m_suppress_drag_until_release) {
        f32 dx = input.mouse_x - m_box_start_x;
        f32 dy = input.mouse_y - m_box_start_y;
        if (std::sqrt(dx * dx + dy * dy) > BOX_DRAG_THRESHOLD) {
            m_box_dragging = true;
        }
    }
    // Keep `m_box_end_x/y` live while dragging so the HUD marquee can
    // follow the pointer smoothly via box_selection().
    if (m_box_dragging) {
        m_box_end_x = input.mouse_x;
        m_box_end_y = input.mouse_y;
    }

    // Release commits the selection change. Blocked / suppressed
    // presses don't become selection changes — the user was casting,
    // gesturing, or poking a button, none of which should reshape the
    // selection on release.
    if (input.mouse_left_released && !targeting && !ctx.hud_captured
        && !m_suppress_drag_until_release) {
        if (m_box_dragging) {
            auto units = ctx.picker.pick_units_in_box(
                m_box_start_x, m_box_start_y,
                input.mouse_x, input.mouse_y,
                sel.player());

            if (input.key_shift) {
                for (auto& u : units) {
                    if (!sel.is_selected(u) && sel.count() < MAX_SELECTION) {
                        sel.toggle(u);
                    }
                }
            } else if (!units.empty()) {
                sel.select_multiple(std::move(units));
            }
            // Empty box drag: don't change selection
        } else {
            auto unit = ctx.picker.pick_unit(input.mouse_x, input.mouse_y, sel.player());
            if (unit.is_valid()) {
                if (input.key_shift) {
                    sel.toggle(unit);
                } else {
                    sel.select(unit);
                }
            }
            // No unit found: don't change selection
        }
        m_box_dragging = false;
    } else if (input.mouse_left_released) {
        // Still want to drop the drag flag on a release that didn't
        // commit — otherwise a subsequent non-blocked press would see
        // stale dragging=true for an instant.
        m_box_dragging = false;
    }
}

// ── Orders ───────────────────────────────────────────────────────────────

void RtsPreset::handle_orders(const InputContext& ctx) {
    auto& input = ctx.input;
    auto& sel = ctx.selection;

    // HUD captures: skip pointer-driven orders (targeting click, attack-
    // move click, right-click order). Key-driven cancels (Escape out of
    // any targeting mode) still run so the user can always bail out.
    if (ctx.hud_captured) {
        if (input.key_escape) {
            m_targeting_ability = false;
            m_targeting_ability_id.clear();
            m_move_targeting_mode = false;
            m_attack_move_mode    = false;
        }
        return;
    }

    // Drop ability targeting mode if the armed ability is no longer
    // castable — typically because the previous cast just resolved and
    // its cooldown started, but also covers selection switching to a
    // unit that doesn't own this ability, the ability being removed
    // by Lua, the unit dying, etc. Without this, the indicator lingers
    // and the next click commits a Cast that the simulation then
    // silently drops at submit-time.
    if (m_targeting_ability) {
        bool still_castable = false;
        if (!sel.empty()) {
            auto caster = sel.selected().front();
            if (auto* aset = ctx.simulation.world().ability_sets.get(caster.id)) {
                for (const auto& a : aset->abilities) {
                    if (a.ability_id == m_targeting_ability_id) {
                        still_castable = (a.cooldown_remaining <= 0.0f);
                        break;
                    }
                }
            }
        }
        if (!still_castable) {
            m_targeting_ability = false;
            m_targeting_ability_id.clear();
        }
    }

    // Ability targeting: hotkey was pressed, now left-click to pick target
    if (m_targeting_ability && input.mouse_left_pressed) {
        if (!sel.empty()) {
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
        }
        m_targeting_ability = false;
        m_targeting_ability_id.clear();
        return;
    }

    // Escape cancels ability targeting
    if (m_targeting_ability && input.key_escape) {
        m_targeting_ability = false;
        m_targeting_ability_id.clear();
        return;
    }

    // Move-targeting mode (command_bar "move"): next left-click on the
    // ground commits a Move order. If the click misses terrain
    // (shouldn't happen in-bounds, but defensive), exit mode anyway.
    if (m_move_targeting_mode && input.mouse_left_pressed) {
        if (!sel.empty()) {
            glm::vec3 world_pos;
            if (ctx.picker.screen_to_world(input.mouse_x, input.mouse_y, world_pos)) {
                GameCommand cmd;
                cmd.player = sel.player();
                cmd.units  = sel.selected();
                cmd.order  = simulation::orders::Move{world_pos};
                cmd.queued = input.key_shift;
                ctx.commands.submit(cmd);
            }
        }
        m_move_targeting_mode = false;
        return;
    }
    if (m_move_targeting_mode && input.key_escape) {
        m_move_targeting_mode = false;
        return;
    }

    // Attack-move: A pressed → next left-click: unit = Attack, ground = AttackMove
    if (m_attack_move_mode && input.mouse_left_pressed) {
        if (!sel.empty()) {
            auto target = ctx.picker.pick_target(input.mouse_x, input.mouse_y);
            if (target.is_valid()) {
                // A-click on unit: force Attack
                GameCommand cmd;
                cmd.player = sel.player();
                cmd.units  = sel.selected();
                cmd.order  = simulation::orders::Attack{target};
                cmd.queued = input.key_shift;
                ctx.commands.submit(cmd);
            } else {
                // A-click on ground: AttackMove
                glm::vec3 world_pos;
                if (ctx.picker.screen_to_world(input.mouse_x, input.mouse_y, world_pos)) {
                    GameCommand cmd;
                    cmd.player = sel.player();
                    cmd.units  = sel.selected();
                    cmd.order  = simulation::orders::AttackMove{world_pos};
                    cmd.queued = input.key_shift;
                    ctx.commands.submit(cmd);
                }
            }
        }
        m_attack_move_mode = false;
        return;
    }

    // Right-click while in any targeting mode = cancel; no smart order
    // is issued. Matches WC3 / SC2 behavior where a stray right-click
    // bails out of targeting rather than firing a Move at the mouse
    // point. The recompute on m_targeting_ability above (uncastable
    // exit) means a right-click here only consumes the click when
    // we're actually still aiming something.
    if (input.mouse_right_pressed &&
        (m_targeting_ability || m_move_targeting_mode || m_attack_move_mode)) {
        m_targeting_ability   = false;
        m_targeting_ability_id.clear();
        m_move_targeting_mode = false;
        m_attack_move_mode    = false;
        return;
    }

    // Right click: smart order
    if (input.mouse_right_pressed && !sel.empty()) {
        // Check if clicking on a unit
        auto target = ctx.picker.pick_target(input.mouse_x, input.mouse_y);

        if (target.is_valid()) {
            // Clicking on a unit — determine if enemy or ally
            auto* target_owner = ctx.simulation.world().owners.get(target.id);
            bool is_enemy = false;
            if (target_owner) {
                is_enemy = ctx.simulation.is_enemy(sel.player(), target_owner->player);
            }

            GameCommand cmd;
            cmd.player = sel.player();
            cmd.units  = sel.selected();
            cmd.queued = input.key_shift;

            if (is_enemy) {
                cmd.order = simulation::orders::Attack{simulation::Unit{target}};
            } else {
                // Friendly unit — move to their position (follow not implemented, use move)
                auto* t = ctx.simulation.world().transforms.get(target.id);
                if (t) {
                    cmd.order = simulation::orders::Move{t->position};
                }
            }
            ctx.commands.submit(cmd);
        } else {
            // Clicking on ground — move order
            glm::vec3 world_pos;
            if (ctx.picker.screen_to_world(input.mouse_x, input.mouse_y, world_pos)) {
                GameCommand cmd;
                cmd.player = sel.player();
                cmd.units  = sel.selected();
                cmd.order  = simulation::orders::Move{world_pos};
                cmd.queued = input.key_shift;
                ctx.commands.submit(cmd);
            }
        }
    }
}

// ── Hotkeys ──────────────────────────────────────────────────────────────

void RtsPreset::handle_hotkeys(const InputContext& ctx) {
    auto& input = ctx.input;
    auto& sel = ctx.selection;
    auto& bindings = ctx.bindings;

    // Stop
    if (bindings.action_pressed("stop", input) && !sel.empty()) {
        GameCommand cmd;
        cmd.player = sel.player();
        cmd.units  = sel.selected();
        cmd.order  = simulation::orders::Stop{};
        cmd.queued = input.key_shift;
        ctx.commands.submit(cmd);
    }

    // Hold position
    if (bindings.action_pressed("hold", input) && !sel.empty()) {
        GameCommand cmd;
        cmd.player = sel.player();
        cmd.units  = sel.selected();
        cmd.order  = simulation::orders::HoldPosition{};
        cmd.queued = input.key_shift;
        ctx.commands.submit(cmd);
    }

    // Attack-move mode
    if (bindings.action_pressed("attack_move", input)) {
        m_attack_move_mode = true;
    }

    // Escape — cancel attack-move mode
    if (input.key_escape) {
        m_attack_move_mode = false;
    }

    // Control groups: Ctrl+N assign, N recall (hardcoded)
    for (u32 i = 0; i < 10; ++i) {
        if (key_pressed(input.key_num[i], m_prev_key_num[i])) {
            if (input.key_ctrl) {
                sel.assign_group(i);
            } else {
                sel.recall_group(i);
            }
        }
    }

    // Select hero 1/2/3
    auto select_hero = [&](u32 hero_index, const std::string& action) {
        if (!bindings.action_pressed(action, input)) return;
        auto& world = ctx.simulation.world();
        u32 found = 0;
        for (u32 i = 0; i < world.classifications.count(); ++i) {
            u32 id = world.classifications.ids()[i];
            auto& flags = world.classifications.data()[i].flags;
            if (!simulation::has_classification(flags, "hero")) continue;

            auto* own = world.owners.get(id);
            if (!own || own->player.id != sel.player().id) continue;

            if (found == hero_index) {
                auto* info = world.handle_infos.get(id);
                if (info) {
                    simulation::Unit u;
                    u.id = id;
                    u.generation = info->generation;
                    sel.select(u);
                }
                return;
            }
            found++;
        }
    };
    select_hero(0, "select_hero_1");
    select_hero(1, "select_hero_2");
    select_hero(2, "select_hero_3");

    // Ability hotkeys: dispatched by the HUD action bar. The HUD scans
    // slot hotkeys in its own handle_action_bar_keys pass (called
    // before this update) and routes into queue_ability(), which the
    // trailing flush in update() consumes via dispatch_ability.
}

void RtsPreset::dispatch_ability(const InputContext& ctx,
                                 std::string_view ability_id,
                                 bool queued_modifier) {
    auto& sel = ctx.selection;
    if (sel.empty()) return;

    // Require the lead unit to actually own this ability — HUD clicks
    // could otherwise bypass ownership if the caller spoofed the id.
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
        m_targeting_ability = true;
        m_targeting_ability_id = ability_id;
        m_attack_move_mode = false;
        m_move_targeting_mode = false;
    }
    // Passive / Aura / Channel: not directly triggerable from here.
}

void RtsPreset::dispatch_command(const InputContext& ctx,
                                 std::string_view command_id) {
    auto& sel = ctx.selection;
    if (sel.empty()) {
        log::info(TAG, "dispatch_command: '{}' ignored (empty selection)",
                  std::string(command_id));
        return;
    }
    log::info(TAG, "dispatch_command: '{}'", std::string(command_id));

    // Commands are mutually exclusive — tapping a new one replaces any
    // targeting mode currently armed, so the user can never accidentally
    // have two primed at once.
    auto reset_modes = [&]() {
        m_move_targeting_mode = false;
        m_attack_move_mode    = false;
        m_targeting_ability   = false;
        m_targeting_ability_id.clear();
    };

    if (command_id == "stop") {
        reset_modes();
        GameCommand cmd;
        cmd.player = sel.player();
        cmd.units  = sel.selected();
        cmd.order  = simulation::orders::Stop{};
        ctx.commands.submit(cmd);
    } else if (command_id == "hold_position") {
        reset_modes();
        GameCommand cmd;
        cmd.player = sel.player();
        cmd.units  = sel.selected();
        cmd.order  = simulation::orders::HoldPosition{};
        ctx.commands.submit(cmd);
    } else if (command_id == "attack" || command_id == "attack_move") {
        reset_modes();
        m_attack_move_mode = true;
    } else if (command_id == "move") {
        reset_modes();
        m_move_targeting_mode = true;
    } else {
        log::warn(TAG, "dispatch_command: unknown id '{}'",
                  std::string(command_id));
    }
    // `patrol` deferred — needs a Patrol order path plus an alternating
    // waypoint-collection mode.
}

// ── Camera ───────────────────────────────────────────────────────────────

void RtsPreset::handle_camera(const InputContext& ctx, f32 dt) {
    auto& input = ctx.input;
    auto& camera = ctx.camera;

    // ── Mobile: two-finger gestures (pan + pinch zoom) ─────────────────
    // Handled first so keyboard / edge-pan paths don't fight on devices
    // that expose both. Gesture state is edge-triggered: the first frame
    // with two fingers just records the reference centroid + distance;
    // subsequent frames apply the frame-to-frame delta. Dropping below
    // two fingers clears the latch so re-engaging doesn't flash-
    // translate the camera by the cumulative gap.
    if (input.touch_count >= 2) {
        f32 cx = 0.5f * (input.touch_x[0] + input.touch_x[1]);
        f32 cy = 0.5f * (input.touch_y[0] + input.touch_y[1]);
        f32 dx = input.touch_x[0] - input.touch_x[1];
        f32 dy = input.touch_y[0] - input.touch_y[1];
        f32 dist = std::sqrt(dx * dx + dy * dy);
        if (m_had_two_finger) {
            // Pan: map-panning convention — a two-finger drag pulls the
            // world along with the fingers. camera.pan's Y already
            // matches screen-down = camera-back without flipping, so
            // passing the raw delta keeps fingers-down → world scrolls
            // down under them.
            f32 pdx = cx - m_prev_centroid_x;
            f32 pdy = cy - m_prev_centroid_y;
            camera.pan(pdx, pdy);

            // Pinch: finger distance grows → zoom in. The scaling
            // factor converts pixel-distance-deltas into the existing
            // scroll-wheel "ticks" the camera consumes.
            constexpr f32 PINCH_SCALE = 0.02f;
            f32 zoom_delta = (dist - m_prev_pinch_dist) * PINCH_SCALE;
            if (zoom_delta != 0.0f) camera.zoom(zoom_delta);
        }
        m_prev_centroid_x = cx;
        m_prev_centroid_y = cy;
        m_prev_pinch_dist = dist;
        m_had_two_finger  = true;
        return;  // while gesturing, suppress keyboard/edge pan below
    }
    m_had_two_finger = false;

    // Arrow keys: pan camera
    f32 pan_x = 0, pan_y = 0;
    if (input.key_left)  pan_x = -1.0f;
    if (input.key_right) pan_x =  1.0f;
    if (input.key_up)    pan_y = -1.0f;  // up arrow = forward
    if (input.key_down)  pan_y =  1.0f;  // down arrow = backward

    // Edge pan: only for shipped game builds, not dev
#ifndef ULDUM_DEBUG
    if (input.mouse_x < EDGE_PAN_MARGIN)     pan_x = -1.0f;
    if (input.mouse_x > w - EDGE_PAN_MARGIN) pan_x =  1.0f;
    if (input.mouse_y < EDGE_PAN_MARGIN)     pan_y = -1.0f;
    if (input.mouse_y > h - EDGE_PAN_MARGIN) pan_y =  1.0f;
#endif

    if (pan_x != 0 || pan_y != 0) {
        f32 speed = EDGE_PAN_SPEED * dt;
        camera.pan(-pan_x * speed, -pan_y * speed);
    }

    // Virtual-stick pan. Same sign convention as arrow-key pan (stick
    // right = pan right = camera.pan(-dx)). Runs independently of the
    // keyboard pan so a player holding both could sum them — cheap and
    // harmless.
    if (ctx.joystick_x != 0.0f || ctx.joystick_y != 0.0f) {
        f32 speed = EDGE_PAN_SPEED * dt;
        camera.pan(-ctx.joystick_x * speed, -ctx.joystick_y * speed);
    }

    // Middle mouse drag: pan camera
    if (input.mouse_middle) {
        camera.pan(input.mouse_dx, input.mouse_dy);
    }

    // Scroll: zoom
    if (input.scroll_delta != 0) {
        camera.zoom(input.scroll_delta);
    }
}

} // namespace uldum::input
