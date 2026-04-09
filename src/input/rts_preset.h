#pragma once

#include "input/input_preset.h"

namespace uldum::input {

// WC3-style RTS controls: click select, box drag, right-click orders,
// control groups, edge pan, scroll zoom, hotkeys.
class RtsPreset : public InputPreset {
public:
    void update(const InputContext& ctx, f32 dt) override;

private:
    void handle_selection(const InputContext& ctx);
    void handle_orders(const InputContext& ctx);
    void handle_hotkeys(const InputContext& ctx);
    void handle_camera(const InputContext& ctx, f32 dt);

    // Box selection state
    bool  m_box_dragging = false;
    f32   m_box_start_x = 0;
    f32   m_box_start_y = 0;
    static constexpr f32 BOX_DRAG_THRESHOLD = 4.0f; // pixels before drag starts

    // Attack-move mode: press A, then left-click target
    bool  m_attack_move_mode = false;

    // Edge pan
    static constexpr f32 EDGE_PAN_MARGIN = 10.0f; // pixels from screen edge
    static constexpr f32 EDGE_PAN_SPEED = 800.0f;  // base pan speed

    // Track previous key state for edge detection
    bool m_prev_key_s = false;
    bool m_prev_key_h = false;
    bool m_prev_key_a = false;
    bool m_prev_key_p = false;
    bool m_prev_key_num[10] = {};
    bool m_prev_key_f1 = false;
    bool m_prev_key_f2 = false;
    bool m_prev_key_f3 = false;
};

} // namespace uldum::input
