#pragma once

#include "core/types.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

// Forward declare sol types to avoid including sol2 in the header
namespace sol { class state; }

namespace uldum::simulation { class Simulation; struct World; class AbilityRegistry; }
namespace uldum::map { class MapManager; }

namespace uldum::script {

struct Timer {
    f32  interval     = 0;
    f32  remaining    = 0;
    bool repeating    = false;
    bool alive        = true;
    u32  id           = 0;
    std::function<void()> callback;
};

struct Trigger {
    u32  id = 0;
    bool alive = true;

    struct EventBinding {
        std::string event_name;
        u32         unit_id    = UINT32_MAX;  // UINT32_MAX = global
        std::string ability_id;                // empty = any ability
        u32         player_id  = UINT32_MAX;   // UINT32_MAX = any player
    };

    std::vector<EventBinding>            events;
    std::vector<std::function<bool()>>   conditions;
    std::vector<std::function<void()>>   actions;
    std::vector<u32>                     owned_timers;

};

class ScriptEngine {
public:
    ScriptEngine();
    ~ScriptEngine();  // defined in .cpp where sol::state is complete

    bool init(simulation::Simulation& sim, map::MapManager& map);
    void shutdown();
    void update(float dt);

    // Fire a game event — evaluates all triggers registered for this event.
    void fire_event(std::string_view event_name, u32 unit_id = UINT32_MAX,
                    std::string_view ability_id = "", u32 player_id = UINT32_MAX);

    // Set event context before firing (combat events set these)
    void set_context_unit(u32 id)     { m_ctx_unit = id; }
    void set_context_player(u32 id)   { m_ctx_player = id; }
    void set_context_ability(const std::string& id) { m_ctx_ability = id; }
    void set_context_damage_source(u32 id) { m_ctx_damage_source = id; }
    void set_context_damage_target(u32 id) { m_ctx_damage_target = id; }
    void set_context_damage_amount(f32 v)  { m_ctx_damage_amount = v; }
    f32  get_context_damage_amount() const { return m_ctx_damage_amount; }
    void set_context_killer(u32 id)   { m_ctx_killer = id; }
    void set_context_spell_target_unit(u32 id) { m_ctx_spell_target_unit = id; }
    void set_context_spell_target_x(f32 x) { m_ctx_spell_target_x = x; }
    void set_context_spell_target_y(f32 y) { m_ctx_spell_target_y = y; }

    // Load and execute a Lua file
    bool load_script(std::string_view path);

    // Call a global Lua function by name
    void call_function(std::string_view name);

private:
    void bind_api();
    void bind_trigger_api();
    void bind_timer_api();

    u32 next_trigger_id() { return ++m_next_trigger; }
    u32 next_timer_id()   { return ++m_next_timer; }

    std::unique_ptr<sol::state> m_lua;
    simulation::Simulation*     m_sim = nullptr;
    map::MapManager*            m_map = nullptr;

    // Triggers
    std::unordered_map<u32, Trigger> m_triggers;
    u32 m_next_trigger = 0;

    // Timers
    std::unordered_map<u32, Timer> m_timers;
    u32 m_next_timer = 0;

    // Event context (set before firing events, read by Lua context functions)
    std::string m_ctx_event;
    u32  m_ctx_unit              = UINT32_MAX;
    u32  m_ctx_player            = UINT32_MAX;
    std::string m_ctx_ability;
    u32  m_ctx_damage_source     = UINT32_MAX;
    u32  m_ctx_damage_target     = UINT32_MAX;
    f32  m_ctx_damage_amount     = 0;
    u32  m_ctx_killer            = UINT32_MAX;
    u32  m_ctx_spell_target_unit = UINT32_MAX;
    f32  m_ctx_spell_target_x    = 0;
    f32  m_ctx_spell_target_y    = 0;
};

} // namespace uldum::script
