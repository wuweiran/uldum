#pragma once

#include <string_view>

namespace uldum::script {

class ScriptEngine {
public:
    bool init();
    void shutdown();
    void update(float dt);

    // Future API:
    // void load_script(std::string_view path);
    // void call_function(std::string_view name);
    // void register_trigger(const TriggerDef& def);
    // void fire_event(Event event, EventArgs args);
    // void bind_engine_api();  // exposes CreateUnit, DamageTarget, etc. to Lua
};

} // namespace uldum::script
