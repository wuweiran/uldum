#pragma once

#include "input/input_preset.h"

#include <string>

namespace uldum::input {

// Action-RPG / MOBA-ish preset: single hero locked to selection, WASD
// for continuous directional movement (via `orders::MoveDirection`),
// abilities on the shared HUD action bar (settings-chosen hotkey mode
// — positional is the natural fit for this preset). Left-click is
// reserved for ability-target commits; right-click unused. Auto-attack
// happens automatically when the simulation's combat system finds an
// enemy in range — there's no click-to-attack input here.
//
// The camera is not moved by this preset; callers (the app) follow the
// controlled hero separately. Selection is expected to already contain
// the hero — Lua's `SetControlledUnit(hero)` does this at scene start
// and is never cleared by this preset.
class ActionPreset : public InputPreset {
public:
    void update(const InputContext& ctx, f32 dt) override;

    void queue_ability(std::string_view ability_id) override;
    void queue_command(std::string_view command_id) override;
    std::string_view targeting_ability_id() const override {
        return m_targeting_ability ? std::string_view{m_targeting_ability_id}
                                    : std::string_view{};
    }
    bool show_selection_circles() const override { return false; }

private:
    void handle_movement(const InputContext& ctx);
    // Returns true when the frame's left-click was consumed (committing
    // or cancelling a targeted cast). Caller skips handle_movement on a
    // consumed click — otherwise the no-WASD frame would emit a Stop
    // and override the Cast we just issued.
    bool handle_targeting(const InputContext& ctx);
    // Left-click in the world (no ability armed, not over UI) sets the
    // HUD's focus_target manually. Hitting an enemy locks focus on it;
    // hitting empty terrain releases the lock so auto-acquire resumes.
    void handle_focus_click(const InputContext& ctx);
    void handle_camera_gestures(const InputContext& ctx);
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
    std::string m_pending_command;

    // Two-finger pan/pinch state, mirrors RtsPreset. Edge-triggered:
    // first frame with two fingers latches the reference centroid /
    // distance; subsequent frames apply frame-to-frame deltas.
    bool m_had_two_finger    = false;
    f32  m_prev_centroid_x   = 0.0f;
    f32  m_prev_centroid_y   = 0.0f;
    f32  m_prev_pinch_dist   = 0.0f;
    // True for any frame this preset's gestures (two-finger) or the
    // HUD's minimap drag is panning the camera. handle_camera_follow
    // consults this to skip its snap-to-hero step so the player's
    // gesture isn't undone the same frame.
    bool m_camera_user_panning = false;
};

} // namespace uldum::input
