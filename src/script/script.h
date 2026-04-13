#pragma once

#include "core/types.h"

#include <glm/vec3.hpp>

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
namespace uldum::render { class EffectRegistry; class EffectManager; }
namespace uldum::audio { class AudioEngine; }
namespace uldum::input { class SelectionState; class CommandSystem; }

namespace uldum::script {

struct Timer {
    f32  interval     = 0;
    f32  remaining    = 0;
    bool repeating    = false;
    bool alive        = true;
    u32  id           = 0;
    std::function<void()> callback;
};

// Fixed priority levels — higher fires first. Used as bucket index.
enum class TriggerPriority : u8 {
    Low    = 0,
    Normal = 1,
    High   = 2,
    Count  = 3,
};

struct Trigger {
    u32             id = 0;
    TriggerPriority priority = TriggerPriority::Normal;
    bool            alive = true;

    struct EventBinding {
        std::string event_name;
        u32         unit_id    = UINT32_MAX;  // UINT32_MAX = any unit
        u32         player_id  = UINT32_MAX;  // UINT32_MAX = any player
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

    // Callback to get attachment point position in model-local space.
    using AttachPointFn = std::function<glm::vec3(u32 entity_id, std::string_view bone_name)>;

    bool init(simulation::Simulation& sim, map::MapManager& map,
              render::EffectRegistry* effects = nullptr,
              render::EffectManager* effect_mgr = nullptr,
              audio::AudioEngine* audio = nullptr);

    void set_attach_point_fn(AttachPointFn fn) { m_attach_fn = std::move(fn); }

    // Callback fired when Lua calls EndGame(winner, stats_json).
    using EndGameFn = std::function<void(u32 winner_id, std::string_view stats_json)>;
    void set_end_game_fn(EndGameFn fn) { m_end_game_fn = std::move(fn); }

    // Connect input systems (call after input is initialized, before scripts run).
    void set_input(input::SelectionState* selection, input::CommandSystem* commands);

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
    void set_context_damage_type(std::string_view t) { m_ctx_damage_type = t; }
    const std::string& get_context_damage_type() const { return m_ctx_damage_type; }
    void set_context_killer(u32 id)        { m_ctx_killer = id; }
    void set_context_heal_source(u32 id)   { m_ctx_heal_source = id; }
    void set_context_heal_target(u32 id)   { m_ctx_heal_target = id; }
    void set_context_heal_amount(f32 v)    { m_ctx_heal_amount = v; }
    f32  get_context_heal_amount() const   { return m_ctx_heal_amount; }
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
    void bind_input_api();

    u32 next_trigger_id() { return ++m_next_trigger; }
    u32 next_timer_id()   { return ++m_next_timer; }

    std::unique_ptr<sol::state> m_lua;
    simulation::Simulation*     m_sim = nullptr;
    map::MapManager*            m_map = nullptr;
    render::EffectRegistry*  m_effects    = nullptr;
    render::EffectManager*   m_effect_mgr = nullptr;
    audio::AudioEngine*      m_audio      = nullptr;
    AttachPointFn            m_attach_fn;
    EndGameFn                m_end_game_fn;

    // Input (set via set_input)
    input::SelectionState*   m_selection = nullptr;
    input::CommandSystem*    m_commands  = nullptr;

    // Order event context
    std::string m_ctx_order_type;
    f32  m_ctx_order_target_x    = 0;
    f32  m_ctx_order_target_y    = 0;
    u32  m_ctx_order_target_unit = UINT32_MAX;
    u32  m_ctx_order_player      = UINT32_MAX;
    bool m_ctx_order_queued      = false;
    bool m_ctx_order_cancelled   = false;

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
    std::string m_ctx_damage_type;
    u32  m_ctx_killer            = UINT32_MAX;
    u32  m_ctx_heal_source       = UINT32_MAX;
    u32  m_ctx_heal_target       = UINT32_MAX;
    f32  m_ctx_heal_amount       = 0;
    u32  m_ctx_spell_target_unit = UINT32_MAX;
    f32  m_ctx_spell_target_x    = 0;
    f32  m_ctx_spell_target_y    = 0;
};

} // namespace uldum::script
