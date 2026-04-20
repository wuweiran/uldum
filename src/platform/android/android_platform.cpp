#include "platform/android/android_platform.h"
#include "core/log.h"

#include <android/log.h>
#include <android/native_window.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

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
    // TODO: m_app->onInputEvent = &on_input_event;  — touch/key handling
    // comes with Phase 16c mobile UI.

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
    }
    return !m_quit_requested;
}

bool AndroidPlatform::was_resized() {
    bool r = m_resized_flag;
    m_resized_flag = false;
    return r;
}

void* AndroidPlatform::native_window_handle() const {
    return m_window;
}

void* AndroidPlatform::asset_manager() const {
    // app->activity->assetManager is an AAssetManager*; returned as void* so
    // callers in platform-agnostic code don't need <android/asset_manager.h>.
    return m_app ? m_app->activity->assetManager : nullptr;
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

} // namespace uldum::platform
