#pragma once

#include "core/types.h"

#include <functional>
#include <memory>
#include <string_view>

namespace uldum::platform {

struct Config {
    std::string_view title = "Uldum Engine";
    u32 width  = 1280;
    u32 height = 720;
};

struct InputState {
    bool key_escape = false;
    bool key_w = false;
    bool key_a = false;
    bool key_s = false;
    bool key_d = false;
    bool key_q = false;  // down
    bool key_e = false;  // up
    bool key_h = false;
    bool key_p = false;
    bool key_f1 = false;
    bool key_f2 = false;
    bool key_f3 = false;
    bool key_up = false;
    bool key_down = false;
    bool key_left = false;
    bool key_right = false;

    // Number keys (0-9) for control groups
    bool key_num[10] = {};

    // Letter keys A-Z (key_letter[0] = A, key_letter[25] = Z)
    bool key_letter[26] = {};

    // Modifiers
    bool key_shift = false;
    bool key_ctrl  = false;
    bool key_alt   = false;

    // Mouse
    f32  mouse_x = 0;    // pixel position
    f32  mouse_y = 0;
    f32  mouse_dx = 0;   // delta since last frame
    f32  mouse_dy = 0;
    f32  scroll_delta = 0;
    bool mouse_left = false;
    bool mouse_right = false;
    bool mouse_middle = false;

    // Mouse press edges (true only on the frame the button was pressed/released)
    bool mouse_left_pressed  = false;
    bool mouse_left_released = false;
    bool mouse_right_pressed  = false;
    bool mouse_right_released = false;
};

class Platform {
public:
    virtual ~Platform() = default;

    virtual bool init(const Config& config) = 0;
    virtual void shutdown() = 0;

    // Returns false when the OS requests quit (WM_QUIT, etc.)
    virtual bool poll_events() = 0;

    virtual const InputState& input() const = 0;

    virtual u32 width() const = 0;
    virtual u32 height() const = 0;
    virtual bool was_resized() = 0;

    // Vulkan surface creation needs these
    virtual void* native_window_handle() const = 0;
    virtual void* native_instance_handle() const = 0;

    // Optional message hook — called before default processing.
    // Return true to consume the message (skip default handling).
    // Args: native_window, msg, wparam, lparam
    using MessageHook = std::function<bool(void*, u32, uintptr_t, intptr_t)>;
    void set_message_hook(MessageHook hook) { m_message_hook = std::move(hook); }

    static std::unique_ptr<Platform> create();

protected:
    MessageHook m_message_hook;
};

} // namespace uldum::platform
