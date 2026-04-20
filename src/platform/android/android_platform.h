#pragma once

#include "platform/platform.h"

// Forward-declare native types to avoid pulling in <android/native_window.h>
// / <game-activity/*.h> into non-Android translation units.
struct ANativeWindow;
struct android_app;

namespace uldum::platform {

// Android platform layer. Integrates with GameActivity via native_app_glue —
// `android_main` (in src/app/android_main.cpp) stashes the `android_app*`
// before creating the App, then our init() picks it up, installs the
// lifecycle/input callbacks, and bridges them to this class.
class AndroidPlatform final : public Platform {
public:
    AndroidPlatform() = default;
    ~AndroidPlatform() override;

    bool init(const Config& config) override;
    void shutdown() override;

    bool poll_events() override;

    const InputState& input() const override { return m_input; }

    u32  width() const override  { return m_width; }
    u32  height() const override { return m_height; }
    bool was_resized() override;

    // Vulkan's VK_KHR_android_surface only needs an ANativeWindow*.
    void* native_window_handle() const override;
    void* native_instance_handle() const override { return nullptr; }

    // AAssetManager* from GameActivity (app->activity->assetManager).
    // AssetManager mounts this to read APK-bundled packages.
    void* asset_manager() const override;

    // Called by the GameActivity glue's onAppCmd dispatcher.
    void on_surface_changed(ANativeWindow* window, u32 w, u32 h);
    void on_surface_lost();

private:
    // android_main stashes the android_app* in a file-scope variable
    // (android_platform.cpp) before App::init runs. init() reads it from
    // there, installs callbacks, and stores the pointer.
    android_app*   m_app = nullptr;
    ANativeWindow* m_window = nullptr;
    InputState     m_input;
    u32            m_width  = 0;
    u32            m_height = 0;
    bool           m_resized_flag = false;
    bool           m_quit_requested = false;
};

} // namespace uldum::platform
