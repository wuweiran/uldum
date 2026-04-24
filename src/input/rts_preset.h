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
    std::string_view targeting_ability_id() const override {
        return m_targeting_ability ? std::string_view{m_targeting_ability_id}
                                    : std::string_view{};
    }

private:
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

    // Box selection state
    bool  m_box_dragging = false;
    f32   m_box_start_x = 0;
    f32   m_box_start_y = 0;
    static constexpr f32 BOX_DRAG_THRESHOLD = 4.0f; // pixels before drag starts

    // Attack-move mode: press A, then left-click target
    bool  m_attack_move_mode = false;

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
};

} // namespace uldum::input
