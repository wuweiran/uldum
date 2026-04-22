#include "ui/shell.h"
#include "ui/render_interface.h"
#include "ui/system_interface.h"
#include "ui/file_interface.h"
#include "core/log.h"

#include <RmlUi/Core.h>
#include <vulkan/vulkan.h>

namespace uldum::ui {

static constexpr const char* TAG = "UI";

struct Shell::Impl {
    RenderInterface* render     = nullptr;
    SystemInterface* system     = nullptr;
    FileInterface*   file       = nullptr;
    Rml::Context*    context    = nullptr;
    bool             initialized = false;
};

Shell::Shell() : m_impl(std::make_unique<Impl>()) {}

Shell::~Shell() {
    shutdown();
}

bool Shell::init(rhi::VulkanRhi& rhi, u32 window_w, u32 window_h) {
    auto& impl = *m_impl;
    if (impl.initialized) return true;

    // Interfaces must outlive Rml::Initialise(). RmlUi stores raw pointers
    // to them and dispatches across its static globals, so we own them and
    // release in shutdown after Rml::Shutdown().
    impl.render = new RenderInterface(rhi);
    impl.system = new SystemInterface();
    impl.file   = new FileInterface();

    if (!impl.render->init()) {
        log::error(TAG, "UI RenderInterface::init failed");
        return false;
    }
    Rml::SetRenderInterface(impl.render);
    Rml::SetSystemInterface(impl.system);
    Rml::SetFileInterface(impl.file);

    if (!Rml::Initialise()) {
        log::error(TAG, "Rml::Initialise() failed");
        return false;
    }

    // Load the game project's fonts. Hardwired to sample_game's path for
    // now; 16b can scan a list from game.json or auto-discover under shell/fonts/.
    if (!Rml::LoadFontFace("shell/fonts/Lato-Regular.ttf")) {
        log::warn(TAG, "Shell UI font failed to load (text won't render)");
    }

    impl.context = Rml::CreateContext("main",
        Rml::Vector2i(static_cast<int>(window_w), static_cast<int>(window_h)));
    if (!impl.context) {
        log::error(TAG, "Rml::CreateContext failed");
        Rml::Shutdown();
        return false;
    }

    impl.initialized = true;
    log::info(TAG, "Shell UI initialized ({}x{})", window_w, window_h);
    return true;
}

void Shell::shutdown() {
    auto& impl = *m_impl;
    if (!impl.initialized) return;

    Rml::Shutdown();

    delete impl.render; impl.render = nullptr;
    delete impl.system; impl.system = nullptr;
    delete impl.file;   impl.file   = nullptr;
    impl.context = nullptr;
    impl.initialized = false;
}

void Shell::on_resize(u32 w, u32 h) {
    if (!m_impl->context) return;
    m_impl->context->SetDimensions(
        Rml::Vector2i(static_cast<int>(w), static_cast<int>(h)));
}

void Shell::update(f32 /*dt*/) {
    // RmlUi drives its own time via SystemInterface::GetElapsedTime(). The
    // per-frame Update() advances animations, transitions, and dispatches
    // deferred events.
    if (m_impl->context) m_impl->context->Update();
}

void Shell::render(void* cmd_buf, u32 width, u32 height) {
    auto& impl = *m_impl;
    if (!impl.context) return;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(cmd_buf);
    VkExtent2D extent{width, height};
    impl.render->begin_frame(cmd, extent);
    impl.context->Render();
}

void Shell::on_mouse_move(i32 x, i32 y) {
    if (m_impl->context) m_impl->context->ProcessMouseMove(x, y, /*keymod*/ 0);
}

void Shell::on_mouse_button(u32 button, bool down) {
    if (!m_impl->context) return;
    if (down) m_impl->context->ProcessMouseButtonDown(static_cast<int>(button), 0);
    else      m_impl->context->ProcessMouseButtonUp  (static_cast<int>(button), 0);
}

void Shell::on_mouse_wheel(f32 delta) {
    if (m_impl->context) m_impl->context->ProcessMouseWheel(-delta, 0);
}

void Shell::on_key(u32 key, bool down) {
    if (!m_impl->context) return;
    auto k = static_cast<Rml::Input::KeyIdentifier>(key);
    if (down) m_impl->context->ProcessKeyDown(k, 0);
    else      m_impl->context->ProcessKeyUp  (k, 0);
}

void Shell::on_text(std::string_view utf8) {
    if (!m_impl->context || utf8.empty()) return;
    m_impl->context->ProcessTextInput(Rml::String(utf8.data(), utf8.size()));
}

Rml::ElementDocument* Shell::load_document(std::string_view rml_path) {
    if (!m_impl->context) return nullptr;
    auto* doc = m_impl->context->LoadDocument(Rml::String(rml_path.data(), rml_path.size()));
    if (!doc) {
        log::error(TAG, "Failed to load RML document '{}'", rml_path);
        return nullptr;
    }
    doc->Show();
    log::info(TAG, "Loaded RML document '{}'", rml_path);
    return doc;
}

} // namespace uldum::ui
