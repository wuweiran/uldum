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
    handle_camera(ctx, dt);
}

void RtsPreset::queue_ability(std::string_view ability_id) {
    m_pending_ability.assign(ability_id);
}

// ── Selection ────────────────────────────────────────────────────────────

void RtsPreset::handle_selection(const InputContext& ctx) {
    auto& input = ctx.input;
    auto& sel = ctx.selection;

    // Skip selection when in targeting mode (ability or attack-move), and
    // when the pointer is currently captured by the HUD (hovering a slot,
    // open menu, etc.) so clicks on UI don't also drag a selection box.
    bool targeting = m_attack_move_mode || m_targeting_ability;
    if (ctx.hud_captured) return;

    // Start box drag on left press
    if (input.mouse_left_pressed && !targeting) {
        m_box_dragging = false;
        m_box_start_x = input.mouse_x;
        m_box_start_y = input.mouse_y;
    }

    // Detect drag threshold while holding left
    if (input.mouse_left && !m_box_dragging && !targeting) {
        f32 dx = input.mouse_x - m_box_start_x;
        f32 dy = input.mouse_y - m_box_start_y;
        if (std::sqrt(dx * dx + dy * dy) > BOX_DRAG_THRESHOLD) {
            m_box_dragging = true;
        }
    }

    // Left release: either box select or click select
    if (input.mouse_left_released && !targeting) {
        if (m_box_dragging) {
            // Box select — own units in rectangle
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
            // Click select — only own units
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
    }
}

// ── Orders ───────────────────────────────────────────────────────────────

void RtsPreset::handle_orders(const InputContext& ctx) {
    auto& input = ctx.input;
    auto& sel = ctx.selection;

    // HUD captures: skip pointer-driven orders (targeting click, attack-
    // move click, right-click order). Key-driven cancels (Escape out of
    // targeting) still run so the user can always bail out of a mode.
    if (ctx.hud_captured) {
        if (m_targeting_ability && input.key_escape) {
            m_targeting_ability = false;
            m_targeting_ability_id.clear();
        }
        return;
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

    // Escape cancels targeting mode
    if (m_targeting_ability && input.key_escape) {
        m_targeting_ability = false;
        m_targeting_ability_id.clear();
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
    }
    // Passive / Aura / Channel: not directly triggerable from here.
}

// ── Camera ───────────────────────────────────────────────────────────────

void RtsPreset::handle_camera(const InputContext& ctx, f32 dt) {
    auto& input = ctx.input;
    auto& camera = ctx.camera;

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
