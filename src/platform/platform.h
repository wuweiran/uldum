#pragma once

#include "core/types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

    // Multi-touch state (mobile). First finger is mirrored into the
    // mouse_* fields above so UI that only knows about mice still
    // reacts. Gestures (two-finger pan / pinch zoom) read `touch_count`
    // and the `touch_x/y` arrays directly. Unused slots are zero.
    // Desktop platforms leave `touch_count` at 0.
    static constexpr u32 MAX_TOUCHES = 4;
    u32  touch_count = 0;
    f32  touch_x[MAX_TOUCHES] = {};
    f32  touch_y[MAX_TOUCHES] = {};
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

    // Physical pixels per dp (density-independent pixel, 1 dp = 1/160
    // inch — Android's real-world unit). HUD authoring happens in dp;
    // the platform layer reports this ratio so the engine multiplies
    // dp → px uniformly at render time. Platforms without a density
    // reading return 1.0 (treats the display as 160 DPI).
    virtual f32 ui_scale() const { return 1.0f; }

    // True on touch-first platforms (Android) — used by the HUD loader
    // to gate mobile-only composites like the virtual joystick so a
    // single hud.json can describe layouts for all platforms. Desktop
    // returns false by default.
    virtual bool is_mobile() const { return false; }

    // Safe-area insets (physical pixels) describing regions obstructed
    // by system UI / hardware (status bar, nav bar, notch, rounded
    // corners). HUD anchor resolution excludes these so bars never
    // land under the cutout on a phone. Desktop platforms without
    // system bars return all zeros.
    struct SafeInsets {
        f32 left   = 0.0f;
        f32 top    = 0.0f;
        f32 right  = 0.0f;
        f32 bottom = 0.0f;
    };
    virtual SafeInsets safe_insets() const { return {}; }

    // Vulkan surface creation needs these
    virtual void* native_window_handle() const = 0;
    virtual void* native_instance_handle() const = 0;

    // AAssetManager* on Android (for reading assets bundled in the APK),
    // nullptr on desktop. Returned as void* so callers compile on both.
    // Used by AssetManager to mount the APK's asset root.
    virtual void* asset_manager() const { return nullptr; }

    // Enumerate regular files directly under a directory (no recursion).
    // `prefix` is interpreted relative to the platform's asset root —
    // CWD on desktop, the APK's `assets/` dir on Android. Returns the
    // file basenames (no directory part). Used by the dev console to
    // discover bundled `.uldmap` archives without each call site
    // hand-rolling filesystem-vs-AAssetManager dispatch.
    virtual std::vector<std::string> list_files(std::string_view prefix) const = 0;

    // Optional message hook — called before default processing.
    // Return true to consume the message (skip default handling).
    // Args: native_window, msg, wparam, lparam
    using MessageHook = std::function<bool(void*, u32, uintptr_t, intptr_t)>;
    void set_message_hook(MessageHook hook) { m_message_hook = std::move(hook); }

    // Show / hide the OS pointer cursor. Used when the engine wants to
    // draw its own cursor texture (targeting mode reticle, etc.). The
    // call is idempotent — repeated identical calls are a no-op so
    // gameplay code doesn't need to track state. Default no-op for
    // platforms without a system cursor (Android).
    virtual void set_cursor_visible(bool /*visible*/) {}

    static std::unique_ptr<Platform> create();

protected:
    MessageHook m_message_hook;
};

} // namespace uldum::platform
