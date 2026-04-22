// Android entry point. native_app_glue calls android_main on a background
// thread once the Activity is attached. This file is the GameActivity → App
// bridge: it stashes the android_app struct where AndroidPlatform::init can
// find it, pumps the event loop until the native surface is ready, then runs
// the App's init → run → shutdown lifecycle the same way the desktop entry
// points do.

#include "app/app.h"
#include "core/log.h"

#include <game-activity/native_app_glue/android_native_app_glue.h>

namespace uldum::platform {
// Implemented in src/platform/android/android_platform.cpp. Called from here
// before App::init, read once by AndroidPlatform::init.
void set_pending_android_app(struct android_app* app);
}

// Drains the ALooper once with the given timeout, dispatching any events.
// Returns false if the Activity requested destroy (caller should bail).
static bool pump_events(struct android_app* app, int timeout_ms) {
    android_poll_source* source = nullptr;
    while (ALooper_pollOnce(timeout_ms, nullptr, nullptr,
                             reinterpret_cast<void**>(&source)) >= 0) {
        if (source) source->process(app, source);
        if (app->destroyRequested) return false;
        timeout_ms = 0;  // only block on the first pass
    }
    return !app->destroyRequested;
}

extern "C" void android_main(struct android_app* app) {
    uldum::log::info("App", "android_main entering");

    // Hand the native app struct to AndroidPlatform via a module-private slot
    // so platform::Platform::create() + init() can pick it up without having
    // to change the Platform factory API.
    uldum::platform::set_pending_android_app(app);

    // Vulkan surface creation (in RHI init) needs an ANativeWindow, which
    // isn't available until APP_CMD_INIT_WINDOW fires on the glue's command
    // queue. Pump events until app->window is populated; native_app_glue's
    // internal handler sets it during source->process without needing our
    // own onAppCmd callback (AndroidPlatform installs its own later).
    while (!app->destroyRequested && !app->window) {
        if (!pump_events(app, /*timeout_ms=*/-1)) break;
    }
    if (app->destroyRequested) {
        uldum::log::info("App", "android_main exiting — activity destroyed before surface ready");
        return;
    }

    uldum::LaunchArgs args;
#ifdef ULDUM_ANDROID_DEV
    // Dev APK (libuldum_dev.so, "Uldum Dev"): engine iteration build. Every
    // maps/*.uldmap/ from the engine repo is bundled in APK assets; we load
    // test_map by default and auto-start since there's no dev console on
    // Android yet. auto_start = true drives Menu → Lobby → Loading → Playing
    // without UI interaction (same as `uldum_dev --map ...` on desktop),
    // avoiding the black screen that would come from sitting at Menu with
    // nothing to render. A future imgui-based dev console on Android will
    // replace the auto-start with a map picker + lobby.
    args.map_path   = "maps/test_map.uldmap";
    args.auto_start = true;
#else
    // Game APK: should read default_map from game.json in APK assets.
    // TODO (Phase 15d remaining): plumb AAssetManager → game.json → map_path.
    // Interim: LaunchArgs default matches desktop (maps/test_map.uldmap).
#endif

    uldum::App game;
    if (!game.init(args)) {
        uldum::log::error("App", "Engine initialization failed on Android");
        // Wait for destroy so the Activity doesn't loop-restart us.
        while (!app->destroyRequested) pump_events(app, /*timeout_ms=*/-1);
        return;
    }

    game.run();
    game.shutdown();

    uldum::log::info("App", "android_main exiting");
}
