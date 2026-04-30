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

    // A press is "blocked" from becoming a selection press when its
    // outcome belongs to someone else. Two flavors:
    //   • press-time:  the HUD owns the press, OR a multi-touch
    //                  gesture is active, OR a targeting mode is
    //                  armed. Any of these on the press edge means
    //                  this cycle isn't a selection cycle at all.
    //   • ongoing:     once a Selection cycle is alive, only blockers
    //                  that didn't exist at press time should kill
    //                  it — and even then, only ones that genuinely
    //                  hijack the gesture (a second finger landing,
    //                  a targeting mode firing). Sliding the cursor
    //                  over a HUD widget mid-drag is just transient
    //                  hover; it must not abort the marquee, or
    //                  drag-select feels broken whenever the box
    //                  crosses an action bar / minimap / etc.
    auto is_press_blocked = [&]() {
        return ctx.hud_captured
            || input.touch_count >= 2
            || is_targeting();
    };
    auto is_ongoing_blocker = [&]() {
        return input.touch_count >= 2
            || is_targeting();
    };

    // ── Press edge: classify the cycle ──────────────────────────────
    if (input.mouse_left_pressed) {
        if (is_press_blocked()) {
            m_press_intent = PressIntent::Ignored;
        } else {
            m_press_intent = PressIntent::Selection;
            m_box_dragging = false;
            m_box_start_x = input.mouse_x;
            m_box_start_y = input.mouse_y;
        }
    }

    // ── Mid-cycle demotion: ongoing blocker arrived while button is
    // held. Selection → Ignored is the only allowed transition; once
    // Ignored, the cycle stays Ignored even if the blocker clears.
    if (m_press_intent == PressIntent::Selection && is_ongoing_blocker()) {
        m_press_intent = PressIntent::Ignored;
        m_box_dragging = false;
    }

    // ── Drag promotion (Selection only) ────────────────────────────
    if (m_press_intent == PressIntent::Selection
        && input.mouse_left && !m_box_dragging) {
        f32 dx = input.mouse_x - m_box_start_x;
        f32 dy = input.mouse_y - m_box_start_y;
        if (std::sqrt(dx * dx + dy * dy) > BOX_DRAG_THRESHOLD) {
            m_box_dragging = true;
        }
    }
    if (m_box_dragging) {
        m_box_end_x = input.mouse_x;
        m_box_end_y = input.mouse_y;
    }

    // ── Release: commit if Selection, then end the cycle ───────────
    if (input.mouse_left_released) {
        if (m_press_intent == PressIntent::Selection) {
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
                // WC3-style click select: prefer own units when multiple
                // units overlap the cursor (so your hero wins over a
                // neutral grunt next to him), but fall through to any
                // unit if no own unit is under the click. Foreign-unit
                // selection is a "view" — orders won't fire on it
                // because the order pipeline validates ownership.
                // Shift-click only stacks own units (WC3 behavior); a
                // shift-click on a foreign unit while you have own
                // units selected does nothing.
                auto unit = ctx.picker.pick_unit(input.mouse_x, input.mouse_y, sel.player());
                if (!unit.is_valid()) {
                    unit = ctx.picker.pick_target(input.mouse_x, input.mouse_y);
                }
                if (unit.is_valid()) {
                    auto* own = ctx.simulation.world().owners.get(unit.id);
                    bool is_own = own && own->player.id == sel.player().id;
                    if (input.key_shift && is_own) sel.toggle(unit);
                    else                           sel.select(unit);
                }
                // No unit found: don't change selection
            }
        }
        m_box_dragging = false;
        m_press_intent = PressIntent::None;
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
        if (input.key_escape) cancel_targeting();
        return;
    }

    // Drop ability targeting mode if the armed ability is no longer
    // castable — typically because the previous cast just resolved and
    // its cooldown started, but also covers selection switching to a
    // unit that doesn't own this ability, the ability being removed
    // by Lua, the unit dying, etc. Without this, the indicator lingers
    // and the next click commits a Cast that the simulation then
    // silently drops at submit-time.
    if (m_target_mode == TargetingMode::Ability) {
        bool still_castable = false;
        if (!sel.empty()) {
            auto caster = sel.selected().front();
            if (auto* aset = ctx.simulation.world().ability_sets.get(caster.id)) {
                for (const auto& a : aset->abilities) {
                    if (a.ability_id == m_target_ability_id) {
                        still_castable = (a.cooldown_remaining <= 0.0f);
                        break;
                    }
                }
            }
        }
        if (!still_castable) cancel_targeting();
    }

    // Ability targeting: hotkey was pressed, now left-click to pick target
    if (m_target_mode == TargetingMode::Ability && input.mouse_left_pressed) {
        if (!sel.empty()) {
            const auto* def = ctx.simulation.abilities().get(m_target_ability_id);
            if (def) {
                if (def->form == simulation::AbilityForm::TargetUnit) {
                    auto target = ctx.picker.pick_target(input.mouse_x, input.mouse_y);
                    // Reject targets that fail the ability's target_filter
                    // (e.g. clicking an enemy with an ally-only heal). The
                    // targeting mode stays armed so the player can retry —
                    // matches the WC3 "click goes ping" behavior on
                    // invalid targets.
                    bool target_ok = false;
                    if (target.is_valid()) {
                        auto caster = sel.selected().front();
                        target_ok = ctx.simulation.target_filter_passes(
                            def->target_filter, caster, target);
                    }
                    if (target_ok) {
                        GameCommand cmd;
                        cmd.player = sel.player();
                        cmd.units  = sel.selected();
                        cmd.order  = simulation::orders::Cast{m_target_ability_id, target, {}};
                        cmd.queued = input.key_shift;
                        ctx.commands.submit(cmd);
                        cancel_targeting();
                    }
                    return;
                } else if (def->form == simulation::AbilityForm::TargetPoint) {
                    glm::vec3 world_pos;
                    if (ctx.picker.screen_to_world(input.mouse_x, input.mouse_y, world_pos)) {
                        GameCommand cmd;
                        cmd.player = sel.player();
                        cmd.units  = sel.selected();
                        cmd.order  = simulation::orders::Cast{m_target_ability_id, {}, world_pos};
                        cmd.queued = input.key_shift;
                        ctx.commands.submit(cmd);
                    }
                }
            }
        }
        cancel_targeting();
        return;
    }

    // Escape cancels any targeting mode (ability / move / attack-move).
    if (is_targeting() && input.key_escape) {
        cancel_targeting();
        return;
    }

    // Move-targeting mode (command_bar "move"): next left-click on the
    // ground commits a Move order. If the click misses terrain
    // (shouldn't happen in-bounds, but defensive), exit mode anyway.
    if (m_target_mode == TargetingMode::Move && input.mouse_left_pressed) {
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
        cancel_targeting();
        return;
    }

    // Attack-move: A pressed → next left-click: unit = Attack (force-
    // attack, ally or enemy), ground = AttackMove. Self-attack is
    // rejected at the simulation layer.
    if (m_target_mode == TargetingMode::AttackMove && input.mouse_left_pressed) {
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
                // A-click on a unit always reads as hostile — flash red.
                if (ctx.target_ping_fn) {
                    if (auto* t = ctx.simulation.world().transforms.get(target.id)) {
                        ctx.target_ping_fn(target, t->position,
                                           InputContext::TargetPingKind::Enemy);
                    }
                }
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
        cancel_targeting();
        return;
    }

    // Right-click while in any targeting mode = cancel; no smart order
    // is issued. Matches WC3 / SC2 behavior where a stray right-click
    // bails out of targeting rather than firing a Move at the mouse
    // point.
    if (input.mouse_right_pressed && is_targeting()) {
        cancel_targeting();
        return;
    }

    // Right click: smart order
    if (input.mouse_right_pressed && !sel.empty()) {
        // Item check first — items have smaller selection radii than
        // units, and a unit standing on top of an item shouldn't steal
        // the click (the player almost always wants to pick up the
        // item in that case). pick_item already excludes carried items
        // so dropped-near-self items still resolve cleanly.
        auto picked_item = ctx.picker.pick_item(input.mouse_x, input.mouse_y);
        if (picked_item.is_valid()) {
            GameCommand cmd;
            cmd.player = sel.player();
            cmd.units  = sel.selected();
            cmd.order  = simulation::orders::PickupItem{picked_item};
            cmd.queued = input.key_shift;
            ctx.commands.submit(cmd);
            // Pickup ping — flash yellow at the item.
            if (ctx.target_ping_fn) {
                if (auto* t = ctx.simulation.world().transforms.get(picked_item.id)) {
                    ctx.target_ping_fn(simulation::Unit{picked_item}, t->position,
                                       InputContext::TargetPingKind::Item);
                }
            }
            return;
        }

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
                // Friendly unit — Follow. Same Move order, just with
                // the target_unit slot filled in instead of a fixed
                // point. The simulation re-resolves the goal each
                // tick from the unit's current position, so we trail
                // them naturally; ends only when the target dies /
                // vanishes or a new order replaces this one. Cluster
                // radius is a small follow distance so we don't push
                // into the followed unit's collision circle.
                simulation::orders::Move m;
                m.target_unit = simulation::Unit{target};
                m.range       = 96.0f;   // follow distance — beyond collision_radius * 2
                cmd.order     = std::move(m);
            }
            ctx.commands.submit(cmd);
            // Target ping — red on enemy, green on friendly. Position
            // sampled from the target's transform so the ring lands
            // exactly under the model.
            if (ctx.target_ping_fn) {
                if (auto* t = ctx.simulation.world().transforms.get(target.id)) {
                    ctx.target_ping_fn(target, t->position,
                        is_enemy ? InputContext::TargetPingKind::Enemy
                                 : InputContext::TargetPingKind::Ally);
                }
            }
        } else {
            // Clicking on ground — move order. No target ping (it
            // would clutter every right-click; only entity targets
            // need the visual confirmation).
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

    // Attack-move mode (A hotkey).
    if (bindings.action_pressed("attack_move", input)) {
        set_target_mode(TargetingMode::AttackMove);
    }

    // Escape cancels every targeting mode (the same exit path
    // handle_orders uses; duplicated here for the case where input
    // never reached handle_orders this frame, e.g. hud_captured).
    if (input.key_escape) cancel_targeting();

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
        set_target_mode(TargetingMode::Ability, ability_id);
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
    // have two primed at once. `set_target_mode` (and `cancel_targeting`
    // for instant commands) handles the swap atomically.
    if (command_id == "stop") {
        cancel_targeting();
        GameCommand cmd;
        cmd.player = sel.player();
        cmd.units  = sel.selected();
        cmd.order  = simulation::orders::Stop{};
        ctx.commands.submit(cmd);
    } else if (command_id == "hold_position") {
        cancel_targeting();
        GameCommand cmd;
        cmd.player = sel.player();
        cmd.units  = sel.selected();
        cmd.order  = simulation::orders::HoldPosition{};
        ctx.commands.submit(cmd);
    } else if (command_id == "attack" || command_id == "attack_move") {
        set_target_mode(TargetingMode::AttackMove);
    } else if (command_id == "move") {
        set_target_mode(TargetingMode::Move);
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
