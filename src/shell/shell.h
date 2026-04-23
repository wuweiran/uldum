#pragma once

#include "core/types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

// Forward-declare RmlUi types — keep the header light. Engine code that uses
// Shell doesn't need to include RmlCore directly.
namespace Rml {
    class Context;
    class ElementDocument;
}

namespace uldum::rhi { class VulkanRhi; }

namespace uldum::shell {

// Shell owns the single Rml::Context for the application and the three
// interface implementations (render, system, file). It's created once at
// engine init (game builds only) and lives alongside the Renderer.
//
// Lifecycle per frame:
//   shell.update(dt)    — advances time, handles any deferred work
//   shell.render()      — RmlUi calls into our RenderInterface to draw
//   shell.handle_*()    — called from platform input dispatch
class Shell {
public:
    Shell();
    ~Shell();
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    // Set up RmlUi, install our interface implementations, create the single
    // Context sized to the window. Must be called after the Vulkan RHI is up
    // (the RenderInterface holds a pointer to it).
    bool init(rhi::VulkanRhi& rhi, u32 window_w, u32 window_h);
    void shutdown();

    // Window resize hook — reforms the RmlUi context dimensions.
    void on_resize(u32 w, u32 h);

    // Per-frame:
    void update(f32 dt);
    // `cmd_buf` is a VkCommandBuffer (erased to opaque pointer here to avoid
    // pulling vulkan.h into every translation unit that includes shell.h;
    // shell.cpp / render_interface.cpp cast it back internally). Call this
    // inside an active render pass on the same cmd buffer the 3D scene draws
    // into — Shell renders into the same color attachment as an overlay.
    void render(void* cmd_buf, u32 width, u32 height);

    // Input plumbing — platform layer forwards events here. Positions in
    // window-pixel coords.
    void on_mouse_move(i32 x, i32 y);
    void on_mouse_button(u32 button, bool down);
    void on_mouse_wheel(f32 delta);
    void on_key(u32 key, bool down);
    void on_text(std::string_view utf8);

    // Document loading. Replaces the current document if one is already
    // shown. Returns nullptr on failure; errors go to log.
    Rml::ElementDocument* load_document(std::string_view rml_path);

    // Hide + close the currently-shown document. Call when leaving a Shell
    // screen (e.g. Menu → Playing) so menu geometry isn't redrawn behind
    // gameplay.
    void hide_current_document();

    // Install a handler that fires when the user clicks any RML element
    // with an id — the handler receives that id (e.g. "play", "quit").
    // Buttons on RML use id attributes; this is the minimal event API.
    using ClickHandler = std::function<void(std::string_view id)>;
    void set_click_handler(ClickHandler handler);

    // Update the text content of an element by id in the current document.
    // Used for settings screens where a button label reflects state
    // ("Sound: ON" / "Sound: OFF"). Silently no-ops if the id isn't found.
    void set_element_text(std::string_view id, std::string_view text);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace uldum::shell
