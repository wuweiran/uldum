#pragma once

#include "input/input_preset.h"

#include <string>

namespace uldum::input {

// Action-RPG / MOBA-ish preset: single hero locked to selection, WASD
// for continuous directional movement (via `orders::MoveDirection`),
// left-click as primary attack, right-click unused, abilities on the
// shared HUD action bar (settings-chosen hotkey mode — positional is
// the natural fit for this preset).
//
// The camera is not moved by this preset; callers (the app) follow the
// controlled hero separately. Selection is expected to already contain
// the hero — Lua's `SetControlledUnit(hero)` does this at scene start
// and is never cleared by this preset.
class ActionPreset : public InputPreset {
public:
    void update(const InputContext& ctx, f32 dt) override;

    void queue_ability(std::string_view ability_id) override;
    std::string_view targeting_ability_id() const override {
        return m_targeting_ability ? std::string_view{m_targeting_ability_id}
                                    : std::string_view{};
    }
    bool show_selection_circles() const override { return false; }

private:
    void handle_movement(const InputContext& ctx);
    void handle_attack_click(const InputContext& ctx);
    void handle_targeting(const InputContext& ctx);
    void handle_camera_follow(const InputContext& ctx);

    // Same dispatch path the RTS preset uses — checks ability ownership,
    // then Instant/Toggle → immediate Cast, Target* → enter targeting
    // mode. Kept as a duplicate until a second shared preset forces a
    // hoist into a common helper.
    void dispatch_ability(const InputContext& ctx,
                          std::string_view ability_id,
                          bool queued_modifier);

    // Targeting state.
    bool        m_targeting_ability    = false;
    std::string m_targeting_ability_id;

    // Queued from HUD; flushed at end of update().
    std::string m_pending_ability;

    // Last-issued movement direction — re-issuing an identical
    // MoveDirection command each frame would flood the command system /
    // network. We only send on change (including start / stop).
    glm::vec2 m_last_move_dir{0.0f, 0.0f};
    bool      m_last_move_valid = false;
};

} // namespace uldum::input
