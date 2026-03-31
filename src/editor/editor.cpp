#include "editor/editor.h"
#include "core/log.h"

namespace uldum::editor {

static constexpr const char* TAG = "Editor";
static bool s_first_update = true;

bool Editor::init() {
    log::info(TAG, "Editor initialized (stub) — ImGui terrain editor pending");
    return true;
}

void Editor::shutdown() {
    log::info(TAG, "Editor shut down (stub)");
}

void Editor::set_active(bool active) {
    if (m_active != active) {
        m_active = active;
        log::info(TAG, "Editor {} (stub)", active ? "activated" : "deactivated");
    }
}

void Editor::update() {
    if (!m_active) return;
    if (s_first_update) {
        log::trace(TAG, "update (stub) — will process brush input, tool selection here");
        s_first_update = false;
    }
}

void Editor::render() {
    if (!m_active) return;
    // Future: ImGui draw calls for editor UI
}

} // namespace uldum::editor
