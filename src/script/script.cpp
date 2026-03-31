#include "script/script.h"
#include "core/log.h"

namespace uldum::script {

static constexpr const char* TAG = "Script";
static bool s_first_update = true;

bool ScriptEngine::init() {
    log::info(TAG, "ScriptEngine initialized (stub) — Lua 5.4 VM + sol2 bindings pending");
    return true;
}

void ScriptEngine::shutdown() {
    log::info(TAG, "ScriptEngine shut down (stub)");
}

void ScriptEngine::update(float dt) {
    if (s_first_update) {
        log::trace(TAG, "update (stub) dt={:.4f}s — will run triggers and coroutines here", dt);
        s_first_update = false;
    }
}

} // namespace uldum::script
