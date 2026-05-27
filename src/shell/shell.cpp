#include "shell/shell.h"
#include "shell/render_interface.h"
#include "shell/system_interface.h"
#include "shell/file_interface.h"
#include "core/log.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/EventListener.h>
#include <vulkan/vulkan.h>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include <filesystem>
#include <string>
#include <vector>

namespace uldum::shell {

static constexpr const char* TAG = "UI";

// Platform-specific list of well-known system font paths — used as
// fallback faces under the game's primary shell font so codepoints
// outside the primary's coverage (CJK, emoji, Arabic, …) still render.
// Mirrors HUD's `collect_system_font_paths` in src/hud/font.cpp; kept
// duplicated rather than extracted to avoid a shell → hud dep just for
// a constant list.
static std::vector<std::string> collect_system_font_paths() {
    std::vector<std::string> paths;
#if defined(_WIN32)
    std::string fonts_dir;
    {
        char buf[MAX_PATH];
        UINT n = GetWindowsDirectoryA(buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) fonts_dir = std::string(buf, n) + "/Fonts/";
        else                        fonts_dir = "C:/Windows/Fonts/";
    }
    paths = {
        fonts_dir + "msyh.ttc",        // CJK Simplified — Microsoft YaHei
        fonts_dir + "msjh.ttc",        // CJK Traditional — Microsoft JhengHei
        fonts_dir + "YuGothR.ttc",     // Japanese — Yu Gothic
        fonts_dir + "malgun.ttf",      // Korean — Malgun Gothic
        fonts_dir + "Nirmala.ttf",     // Indic
        fonts_dir + "Leelawui.ttf",    // Thai / Lao
        fonts_dir + "seguiemj.ttf",    // Emoji
    };
#elif defined(__APPLE__)
    paths = {
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/HiraginoSans.ttc",
        "/System/Library/Fonts/AppleSDGothicNeo.ttc",
        "/System/Library/Fonts/Devanagari MT.ttc",
        "/System/Library/Fonts/ThonburiUI.ttc",
        "/System/Library/Fonts/Apple Color Emoji.ttc",
    };
#elif defined(__ANDROID__)
    paths = {
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/NotoSansDevanagari-Regular.ttf",
        "/system/fonts/NotoSansThai-Regular.ttf",
        "/system/fonts/NotoColorEmoji.ttf",
    };
#elif defined(__linux__)
    paths = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
    };
#endif
    return paths;
}

// RmlUi event listener that dispatches element clicks to a C++ std::function.
// Attached to every loaded document — RmlUi bubbles events up to the root.
class ShellClickListener final : public Rml::EventListener {
public:
    Shell::ClickHandler handler;
    void ProcessEvent(Rml::Event& event) override {
        if (!handler) return;
        auto* el = event.GetTargetElement();
        if (!el) return;
        const Rml::String& id = el->GetId();
        if (id.empty()) return;
        handler(std::string_view(id.c_str(), id.size()));
    }
};

struct Shell::Impl {
    RenderInterface* render     = nullptr;
    SystemInterface* system     = nullptr;
    FileInterface*   file       = nullptr;
    Rml::Context*    context    = nullptr;
    Rml::ElementDocument* document = nullptr;
    std::unique_ptr<ShellClickListener> click_listener;
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

    // Load the game project's fonts. Hardwired to sample_game's path
    // for now; 16b can scan a list from game.json or auto-discover
    // under shell/fonts/. Two project conventions matter:
    //   1. Path is all-lowercase. AssetManager normalizes lookups to
    //      lowercase (package format is case-insensitive), but Android's
    //      AAssetManager_open is case-sensitive, so any uppercase
    //      letters in the filename break Android resolution silently.
    //   2. Family name comes from the TTF's metadata, NOT the file name.
    //      sample_game's lato-regular.ttf reports family "LatoLatin",
    //      so RCSS must use `font-family: "LatoLatin"`. Check
    //      RmlUi's "Loaded font face '<name>'" log line to confirm
    //      the registered name; mismatched RCSS produces empty text
    //      without a hard error.
    if (!Rml::LoadFontFace("shell/fonts/lato-regular.ttf")) {
        log::warn(TAG, "Shell UI font failed to load (text won't render)");
    }

    // Layer system fonts underneath as fallback faces — same idea as
    // the HUD's font chain. Each call registers the file as a fallback
    // (not the primary), so RmlUi consults it only when the game's
    // primary face has no glyph for the requested codepoint. CJK,
    // emoji, Arabic, etc. all render this way even though the project
    // only ships a Latin-coverage font. Missing files (e.g. on a
    // minimal Linux install) silently skip.
    for (const auto& p : collect_system_font_paths()) {
        // RmlUi reads via the registered FileInterface; our impl
        // (file_interface.cpp) falls through to a plain std::ifstream
        // on miss, so absolute OS paths like "C:/Windows/Fonts/..."
        // resolve correctly. Skip files that aren't present — some
        // entries in the chain are optional Windows fonts (Nirmala
        // for Indic) that aren't installed on every machine. Without
        // the pre-check RmlUi logs ERROR on every miss; we want
        // missing-fallback to be quiet.
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) continue;
        Rml::LoadFontFace(p, /*fallback_face=*/true);
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

void Shell::render(rhi::CommandList& cmd, u32 width, u32 height) {
    auto& impl = *m_impl;
    if (!impl.context) return;
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
    auto& impl = *m_impl;
    if (!impl.context) return nullptr;
    // Replace any existing document so Shell owns exactly one at a time.
    hide_current_document();

    auto* doc = impl.context->LoadDocument(Rml::String(rml_path.data(), rml_path.size()));
    if (!doc) {
        log::error(TAG, "Failed to load RML document '{}'", rml_path);
        return nullptr;
    }
    doc->Show();

    // If a click handler is registered, attach the listener to this doc's
    // root. `bubbles=true` (actually capture_phase=false + bubbles impl by
    // the third arg meaning 'in_capture_phase') → clicks on descendants
    // bubble up and fire here.
    if (impl.click_listener) {
        doc->AddEventListener("click", impl.click_listener.get(), false);
    }

    impl.document = doc;
    log::info(TAG, "Loaded RML document '{}'", rml_path);
    return doc;
}

void Shell::hide_current_document() {
    auto& impl = *m_impl;
    if (!impl.document) return;
    if (impl.click_listener) {
        impl.document->RemoveEventListener("click", impl.click_listener.get(), false);
    }
    impl.document->Close();
    impl.document = nullptr;
}

void Shell::set_element_text(std::string_view id, std::string_view text) {
    auto& impl = *m_impl;
    if (!impl.document) return;
    auto* el = impl.document->GetElementById(Rml::String(id.data(), id.size()));
    if (!el) return;
    el->SetInnerRML(Rml::String(text.data(), text.size()));
}

void Shell::set_click_handler(ClickHandler handler) {
    auto& impl = *m_impl;
    if (!impl.click_listener) {
        impl.click_listener = std::make_unique<ShellClickListener>();
    }
    impl.click_listener->handler = std::move(handler);

    // If a document is already shown (handler set after load), attach now.
    if (impl.document) {
        impl.document->AddEventListener("click", impl.click_listener.get(), false);
    }
}

} // namespace uldum::shell
