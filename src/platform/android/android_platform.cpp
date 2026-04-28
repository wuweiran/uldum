#include "platform/android/android_platform.h"
#include "core/log.h"

#include <android/asset_manager.h>
#include <android/log.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/native_window.h>
#include <game-activity/GameActivity.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

#include <algorithm>

namespace uldum::platform {

static constexpr const char* TAG = "Platform";

// android_main stashes the android_app* here before it creates the App; our
// init() reads and clears it. Module-private so nothing else depends on it.
namespace {
android_app* g_pending_app = nullptr;
}

void set_pending_android_app(android_app* app) {
    g_pending_app = app;
}

// Static dispatcher — native_app_glue invokes this when a lifecycle command
// fires. `app->userData` carries our AndroidPlatform*, set during init().
static void on_app_cmd(android_app* app, int32_t cmd) {
    auto* self = static_cast<AndroidPlatform*>(app->userData);
    if (!self) return;

    switch (cmd) {
    case APP_CMD_INIT_WINDOW: {
        ANativeWindow* win = app->window;
        if (win) {
            u32 w = static_cast<u32>(ANativeWindow_getWidth(win));
            u32 h = static_cast<u32>(ANativeWindow_getHeight(win));
            self->on_surface_changed(win, w, h);
            log::info(TAG, "APP_CMD_INIT_WINDOW {}x{}", w, h);
        }
        break;
    }
    case APP_CMD_TERM_WINDOW:
        log::info(TAG, "APP_CMD_TERM_WINDOW");
        self->on_surface_lost();
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        if (app->window) {
            u32 w = static_cast<u32>(ANativeWindow_getWidth(app->window));
            u32 h = static_cast<u32>(ANativeWindow_getHeight(app->window));
            self->on_surface_changed(app->window, w, h);
        }
        break;
    case APP_CMD_WINDOW_INSETS_CHANGED:
        // Android dispatches this whenever the activity's
        // onApplyWindowInsets fires — i.e. the moment the system bars'
        // sizes change (first layout pass populating zeros to real
        // values, rotation, gesture-nav show/hide, multi-window). Flag
        // a "resize" without touching the surface so the app's main
        // loop re-runs the HUD inset push next iteration.
        log::info(TAG, "APP_CMD_WINDOW_INSETS_CHANGED");
        self->on_insets_changed();
        break;
    case APP_CMD_DESTROY:
        log::info(TAG, "APP_CMD_DESTROY");
        // poll_events returns false next tick, exiting the main loop.
        // (The private m_quit_requested flag is set by a method on
        // AndroidPlatform; we expose it indirectly via on_surface_lost
        // here for now — real destroy handling comes when App cooperates.)
        self->on_surface_lost();
        break;
    default:
        break;
    }
}

std::unique_ptr<Platform> Platform::create() {
    return std::make_unique<AndroidPlatform>();
}

AndroidPlatform::~AndroidPlatform() {
    shutdown();
}

bool AndroidPlatform::init(const Config& /*config*/) {
    if (!g_pending_app) {
        log::error(TAG, "AndroidPlatform::init called without a pending android_app — "
                        "did android_main run?");
        return false;
    }

    m_app = g_pending_app;
    g_pending_app = nullptr;

    // android_main pumps events before calling App::init, so by the time we
    // get here the native_app_glue has already received APP_CMD_INIT_WINDOW
    // and populated app->window. Sync that state into our class members now
    // — our on_surface_changed callback wouldn't have fired because
    // app->onAppCmd wasn't installed yet.
    if (m_app->window) {
        u32 w = static_cast<u32>(ANativeWindow_getWidth(m_app->window));
        u32 h = static_cast<u32>(ANativeWindow_getHeight(m_app->window));
        on_surface_changed(m_app->window, w, h);
        log::info(TAG, "Initial surface synced: {}x{}", w, h);
    }

    m_app->userData = this;
    m_app->onAppCmd = &on_app_cmd;
    // GameActivity delivers motion events through a double-buffered
    // input queue (`android_app_swap_input_buffers`) rather than a
    // callback — poll_events() drains it each frame.

    log::info(TAG, "AndroidPlatform initialized — GameActivity callbacks wired");
    return true;
}

void AndroidPlatform::shutdown() {
    if (m_app) {
        m_app->userData = nullptr;
        m_app->onAppCmd = nullptr;
        m_app = nullptr;
    }
    m_window = nullptr;
    m_width = m_height = 0;
}

bool AndroidPlatform::poll_events() {
    // Clear edge flags once per frame so only new presses/releases
    // register. Matches Win32 platform behavior.
    m_input.mouse_left_pressed   = false;
    m_input.mouse_left_released  = false;
    m_input.mouse_right_pressed  = false;
    m_input.mouse_right_released = false;

    // Drain all pending Android commands without blocking. Returning control
    // quickly keeps the game loop responsive.
    if (m_app) {
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(/*timeoutMs=*/0, nullptr, nullptr,
                                 reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(m_app, source);
            // If the Activity requested destroy during dispatch, bail now.
            if (m_app->destroyRequested) {
                m_quit_requested = true;
                break;
            }
        }
        drain_input_buffer();
    }
    return !m_quit_requested;
}

void AndroidPlatform::drain_input_buffer() {
    android_input_buffer* buf = android_app_swap_input_buffers(m_app);
    if (!buf) return;

    for (u64 i = 0; i < buf->motionEventsCount; ++i) {
        const GameActivityMotionEvent& ev = buf->motionEvents[i];

        // Action code + pointer index live in `action` — decode per the
        // classic Android MotionEvent format so we can distinguish
        // POINTER_DOWN/UP (multi-touch) from DOWN/UP (primary finger).
        const i32 action    = ev.action & AMOTION_EVENT_ACTION_MASK;
        const i32 p_index   = (ev.action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                                >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

        // Update the full touch roster. GameActivity gives us every live
        // pointer in `pointers[0..pointerCount)`; we mirror that into
        // the platform-agnostic InputState with a small cap.
        const u32 live = std::min<u32>(ev.pointerCount, InputState::MAX_TOUCHES);
        m_input.touch_count = live;
        for (u32 p = 0; p < live; ++p) {
            m_input.touch_x[p] = GameActivityPointerAxes_getX(&ev.pointers[p]);
            m_input.touch_y[p] = GameActivityPointerAxes_getY(&ev.pointers[p]);
        }

        // Primary-finger state mirrors into the mouse API so HUD and
        // preset code that only knows about mice still works. The
        // primary finger is always the first entry in `pointers`.
        if (live > 0) {
            m_input.mouse_x = m_input.touch_x[0];
            m_input.mouse_y = m_input.touch_y[0];
        }

        switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
            // First finger touched the screen.
            m_input.mouse_left         = true;
            m_input.mouse_left_pressed = true;
            break;
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            // An additional finger joined; no mouse-state change.
            (void)p_index;
            break;
        case AMOTION_EVENT_ACTION_UP:
            // Last finger lifted.
            m_input.mouse_left          = false;
            m_input.mouse_left_released = true;
            m_input.touch_count         = 0;
            break;
        case AMOTION_EVENT_ACTION_POINTER_UP:
            // Secondary finger lifted; live count already updated above.
            break;
        case AMOTION_EVENT_ACTION_CANCEL:
            // System yanked the gesture (e.g. notification shade). Treat
            // like release so held state doesn't stick.
            m_input.mouse_left          = false;
            m_input.mouse_left_released = true;
            m_input.touch_count         = 0;
            break;
        default:
            break;
        }
    }
    android_app_clear_motion_events(buf);
    // Key events are drained + cleared so the queue doesn't overflow;
    // actual key handling comes with the keyboard phase.
    android_app_clear_key_events(buf);
}

bool AndroidPlatform::was_resized() {
    bool r = m_resized_flag;
    m_resized_flag = false;
    return r;
}

AndroidPlatform::SafeInsets AndroidPlatform::safe_insets() const {
    SafeInsets out{};
    if (!m_app || !m_app->activity) return out;
    ARect r{};
    // SYSTEM_BARS = union of status_bars + nav_bars + caption_bar.
    // The union of notch / rounded-corner areas is separate
    // (DISPLAY_CUTOUT + WATERFALL) and could be merged in if maps
    // complain about overlap on edge-case devices.
    GameActivity_getWindowInsets(m_app->activity,
                                 GAMECOMMON_INSETS_TYPE_SYSTEM_BARS, &r);
    out.left   = static_cast<f32>(r.left);
    out.top    = static_cast<f32>(r.top);
    out.right  = static_cast<f32>(r.right);
    out.bottom = static_cast<f32>(r.bottom);
    return out;
}

f32 AndroidPlatform::ui_scale() const {
    if (!m_app || !m_app->config) return 1.0f;
    // Android's dp spec baseline (1/160 inch per unit). Gives integer
    // ui_scale at every standard density bucket except hdpi:
    //   mdpi 160 → 1, xhdpi 320 → 2, xxhdpi 480 → 3, xxxhdpi 640 → 4.
    // hdpi at 240 lands at 1.5× — rare on modern devices. Sentinels
    // (DEFAULT/NONE/ANY) fall back to mdpi.
    int32_t d = AConfiguration_getDensity(m_app->config);
    if (d == ACONFIGURATION_DENSITY_DEFAULT ||
        d == ACONFIGURATION_DENSITY_NONE    ||
        d == ACONFIGURATION_DENSITY_ANY) {
        d = 160;
    }
    return static_cast<f32>(d) / 160.0f;
}

void* AndroidPlatform::native_window_handle() const {
    return m_window;
}

void* AndroidPlatform::asset_manager() const {
    // app->activity->assetManager is an AAssetManager*; returned as void* so
    // callers in platform-agnostic code don't need <android/asset_manager.h>.
    return m_app ? m_app->activity->assetManager : nullptr;
}

std::vector<std::string> AndroidPlatform::list_files(std::string_view prefix) const {
    std::vector<std::string> out;
    if (!m_app || !m_app->activity || !m_app->activity->assetManager) return out;
    AAssetManager* mgr = m_app->activity->assetManager;
    std::string p(prefix);
    AAssetDir* dir = AAssetManager_openDir(mgr, p.c_str());
    if (!dir) return out;
    while (const char* name = AAssetDir_getNextFileName(dir)) {
        out.emplace_back(name);
    }
    AAssetDir_close(dir);
    return out;
}

void AndroidPlatform::on_surface_changed(ANativeWindow* window, u32 w, u32 h) {
    m_window = window;
    if (w != m_width || h != m_height) {
        m_width = w;
        m_height = h;
        m_resized_flag = true;
    }
}

void AndroidPlatform::on_surface_lost() {
    m_window = nullptr;
    m_resized_flag = true;  // RHI will see 0x0 on next was_resized and tear down.
}

void AndroidPlatform::on_insets_changed() {
    // Reuse the resize latch — App's resize handler already calls
    // refresh_safe_insets() (and on_viewport_resized) so the HUD will
    // pick up the new values on the next frame. Cheaper than adding a
    // dedicated flag + path through the platform interface.
    m_resized_flag = true;
}

} // namespace uldum::platform
