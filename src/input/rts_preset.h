#pragma once

#include "input/input_preset.h"

#include <string>

namespace uldum::input {

// WC3-style RTS controls: click select, box drag, right-click orders,
// control groups, edge pan, scroll zoom, hotkeys.
class RtsPreset : public InputPreset {
public:
    void update(const InputContext& ctx, f32 dt) override;
    void queue_ability(std::string_view ability_id) override;
    void queue_command(std::string_view command_id) override;
    std::string_view targeting_ability_id() const override {
        return m_targeting_ability ? std::string_view{m_targeting_ability_id}
                                    : std::string_view{};
    }
    std::string_view active_command_id() const override {
        // "attack" / "move" are the canonical ids the command_bar
        // matches against; attack-move maps to "attack" for the same
        // highlight treatment.
        if (m_attack_move_mode)    return "attack";
        if (m_move_targeting_mode) return "move";
        return {};
    }

    BoxSelection box_selection() const override {
        BoxSelection s;
        s.active = m_box_dragging;
        s.x0 = m_box_start_x;
        s.y0 = m_box_start_y;
        s.x1 = m_box_end_x;
        s.y1 = m_box_end_y;
        return s;
    }

private:
    // Each left-press/release pair is one "press cycle" with a single
    // intent, set at the press edge and read at release. None is the
    // resting state between cycles. Selection commits selection
    // changes on release; Ignored does nothing world-side (the press
    // is owned by HUD / targeting / multi-touch). A Selection cycle
    // can demote to Ignored mid-cycle if a blocker arrives (HUD
    // pointer-enter, second finger lands), but never the reverse.
    enum class PressIntent : u8 { None, Selection, Ignored };

    void handle_selection(const InputContext& ctx);
    void handle_orders(const InputContext& ctx);
    void handle_hotkeys(const InputContext& ctx);
    void handle_camera(const InputContext& ctx, f32 dt);

    // Shared dispatch used by both keyboard hotkeys and HUD slot clicks.
    // Verifies the selection's lead unit owns this ability, then either
    // submits an Instant/Toggle Cast command outright or enters
    // target-picking mode (TargetUnit / TargetPoint). Passive/Aura
    // abilities and unknown ids are ignored.
    void dispatch_ability(const InputContext& ctx,
                          std::string_view ability_id,
                          bool queued_modifier);

    // Command-bar dispatch. Stop / HoldPosition submit immediately;
    // Attack / AttackMove flip `m_attack_move_mode` on; Move flips
    // `m_move_targeting_mode` on. Unknown ids are ignored.
    void dispatch_command(const InputContext& ctx,
                          std::string_view command_id);

    // Box selection state
    bool  m_box_dragging = false;
    f32   m_box_start_x = 0;
    f32   m_box_start_y = 0;
    f32   m_box_end_x   = 0;
    f32   m_box_end_y   = 0;
    // Active press cycle's intent. Set on press edge, demoted to
    // Ignored if a blocker arrives mid-cycle, reset to None on
    // release. Replaces the older "suppress-drag-until-release"
    // latch, which couldn't distinguish "press happened while
    // blocked" from "release frame happened to be unblocked."
    PressIntent m_press_intent = PressIntent::None;
    static constexpr f32 BOX_DRAG_THRESHOLD = 4.0f; // pixels before drag starts

    // Attack-move mode: press A, then left-click target
    bool  m_attack_move_mode = false;

    // Move-to-point mode: Command-bar "move" tapped, waiting for the
    // ground click that commits the order. Mirrors attack-move's shape
    // except the commit is Move, not Attack/AttackMove.
    bool  m_move_targeting_mode = false;

    // Ability targeting mode: hotkey pressed, waiting for target click
    bool        m_targeting_ability = false;
    std::string m_targeting_ability_id;

    // Edge pan
    static constexpr f32 EDGE_PAN_MARGIN = 10.0f; // pixels from screen edge
    static constexpr f32 EDGE_PAN_SPEED = 800.0f;  // base pan speed

    // Control group edge detection (hardcoded, not configurable)
    bool m_prev_key_num[10] = {};

    // Queued ability request from the HUD (slot click / slot hotkey).
    // Processed at the end of the next update() against that frame's
    // context, then cleared. Empty when no request is pending.
    std::string m_pending_ability;

    // Queued command request from the command_bar (tap on a command
    // slot). Processed alongside m_pending_ability in update().
    std::string m_pending_command;

    // Two-finger gesture state (mobile camera pan + pinch zoom). Held
    // across frames so deltas are well-defined; reset whenever the
    // user drops below two fingers so the next re-engage doesn't
    // flash-translate the camera.
    bool m_had_two_finger    = false;
    f32  m_prev_centroid_x   = 0;
    f32  m_prev_centroid_y   = 0;
    f32  m_prev_pinch_dist   = 0;
};

} // namespace uldum::input
