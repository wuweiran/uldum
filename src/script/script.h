#pragma once

#include "core/types.h"
#include "simulation/handle_types.h"

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// Forward declare sol types to avoid including sol2 in the header
namespace sol { class state; }

namespace uldum::simulation { class Simulation; struct World; class AbilityRegistry; class CommandSystem; class SelectionState; }
namespace uldum::map { class MapManager; }
namespace uldum::audio { class AudioEngine; }
namespace uldum::hud   { class Hud; }
namespace uldum::i18n  { class LocaleManager; }

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
        u32         region_id  = UINT32_MAX;  // UINT32_MAX = any region
        std::string node_id;                  // empty = any node
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
              audio::AudioEngine* audio = nullptr);

    // Lighting / environment callback. Host SetSunDirection invokes
    // this after broadcasting the network packet so the host's own
    // renderer reflects the change. App installs the callback; the
    // dedicated server leaves it unset.
    using SetSunDirectionFn = std::function<void(f32 x, f32 y, f32 z)>;
    void set_sun_direction_fn(SetSunDirectionFn fn) { m_set_sun_direction_fn = std::move(fn); }

    void set_attach_point_fn(AttachPointFn fn) { m_attach_fn = std::move(fn); }

    // Locale resolver for the L(key, args) Lua helper. Optional — if unset,
    // L() returns a handle that resolves to the literal key string.
    void set_locale_manager(i18n::LocaleManager* mgr) { m_i18n = mgr; }

    // Callback fired when Lua calls EndGame(winner, stats_json).
    using EndGameFn = std::function<void(u32 winner_id, std::string_view stats_json)>;
    void set_end_game_fn(EndGameFn fn) { m_end_game_fn = std::move(fn); }

    // Callback fired when a unit's attribute/state/ability changes (for network sync).
    using UnitUpdateFn = std::function<void(u32 entity_id, const std::vector<u8>& packet)>;
    void set_unit_update_fn(UnitUpdateFn fn) { m_unit_update_fn = std::move(fn); }

    // Non-entity-scoped broadcast — audio cues, sun direction,
    // free-position PlayEffect, future global notifications. Server-
    // side only; clients never set this. Offline: callback is empty;
    // bindings still apply locally on the host.
    using BroadcastFn = std::function<void(const std::vector<u8>& packet)>;
    void set_broadcast_fn(BroadcastFn fn) { m_broadcast_fn = std::move(fn); }

    // Per-player effect lifecycle hooks. All effects — burst-style and
    // long-lived — share the same Create/Destroy code path; the
    // dispatcher runs a visibility delta each tick and fires deliver
    // when a player gains vision, destroy when they lose vision (and
    // again at lifetime expiry, fanned out across every player
    // currently rendering it). The server_id is a stable handle the
    // host assigned at registration time.
    using EffectDeliverFn = std::function<void(u32 player_id, u32 server_id,
                                                std::string_view name,
                                                u32 entity_id,
                                                glm::vec3 pos,
                                                std::string_view attach_point)>;
    using EffectDestroyFn = std::function<void(u32 player_id, u32 server_id)>;
    void set_effect_deliver_fn(EffectDeliverFn fn) { m_effect_deliver_fn = std::move(fn); }
    void set_effect_destroy_fn(EffectDestroyFn fn) { m_effect_destroy_fn = std::move(fn); }

    // Player count for the per-tick visibility scan. Host populates
    // this after init_alliances; offline / dedicated builds set to 1.
    void set_player_count(u32 count) { m_player_count = count; }

    // Callback fired when Lua calls LoadScene(name). The actual switch
    // (entity teardown, terrain reload, script reload, main() call)
    // lives at the App layer where map/asset/sim/script are all in
    // scope; this callback just forwards the request.
    using SceneSwitchFn = std::function<void(std::string_view scene_name)>;
    void set_scene_switch_fn(SceneSwitchFn fn) { m_scene_switch_fn = std::move(fn); }

    // Per-player scripted-camera routing. App installs these; the
    // implementation decides whether to apply locally (own player or
    // WC3-style camera. App routes the `players_mask` (parsed from
    // Lua's `players` arg via parse_players_mask) per set bit. Pitch/
    // yaw are radians on the C++ side; Lua converts from degrees.
    // Lua surface: GetCameraSetup, CameraSetupApply,
    // CameraSetTargetPosition, CameraSetSourceDistance,
    // CameraSetTargetController, CameraShake.
    using CameraApplySetupFn         = std::function<void(u32 players_mask,
                                                           f32 tx, f32 ty, f32 tz,
                                                           f32 distance,
                                                           f32 pitch_rad, f32 yaw_rad,
                                                           f32 duration)>;
    using CameraSetTargetPositionFn  = std::function<void(u32 players_mask,
                                                           f32 x, f32 y, f32 z, f32 duration)>;
    using CameraSetSourceDistanceFn  = std::function<void(u32 players_mask,
                                                           f32 distance, f32 duration)>;
    using CameraShakeFn              = std::function<void(u32 players_mask,
                                                           f32 intensity, f32 duration)>;
    using CameraSetTargetControllerFn = std::function<void(u32 players_mask,
                                                            simulation::Unit unit)>;
    void set_camera_apply_setup_fn        (CameraApplySetupFn fn)         { m_camera_apply_setup_fn          = std::move(fn); }
    void set_camera_set_target_position_fn(CameraSetTargetPositionFn fn)  { m_camera_set_target_position_fn  = std::move(fn); }
    void set_camera_set_source_distance_fn(CameraSetSourceDistanceFn fn)  { m_camera_set_source_distance_fn  = std::move(fn); }
    void set_camera_shake_fn              (CameraShakeFn fn)              { m_camera_shake_fn                = std::move(fn); }
    void set_camera_set_target_controller_fn(CameraSetTargetControllerFn fn) { m_camera_set_target_controller_fn = std::move(fn); }


    // Connect input systems (call after input is initialized, before scripts run).
    void set_input(simulation::SelectionState* selection, simulation::CommandSystem* commands);

    // Connect the HUD so the Lua bindings under `bind_hud_api` can mutate
    // node content (SetLabelText, SetBarFill, etc.) and create text tags.
    // Call before init() — the binding registration needs the pointer live.
    void set_hud(hud::Hud* hud) { m_hud = hud; }

    void shutdown();
    void update(float dt);

    // Lua-driven sim pause (PauseGame/UnpauseGame). Read by App's
    // should_tick gate so scripts can freeze gameplay during dialogs,
    // cutscenes, etc. Independent of the network's reconnect-pause.
    bool is_paused() const { return m_paused; }
    void set_paused(bool p) { m_paused = p; }

    // True when the engine is running offline (no network). Set once
    // by App at init based on launch mode; surfaced to Lua via
    // IsSinglePlayer().
    void set_singleplayer(bool sp) { m_singleplayer = sp; }
    bool is_singleplayer() const   { return m_singleplayer; }

    // Game-speed multiplier consumed by App's tick loop (1.0 = normal,
    // 2.0 = double, 0 = paused). Single-player only — SetGameSpeed
    // from Lua warns + no-ops in MP because mutating sim cadence on
    // one peer would desync the others.
    f32  game_speed() const { return m_game_speed; }
    void set_game_speed(f32 v) { m_game_speed = v; }

    // Fire a game event — evaluates all triggers registered for this event.
    void fire_event(std::string_view event_name, u32 unit_id = UINT32_MAX,
                    std::string_view ability_id = "", u32 player_id = UINT32_MAX,
                    u32 region_id = UINT32_MAX, std::string_view node_id = "");

    // Fire a HUD node event (button press, etc.) tagged with the issuing
    // player. Sets the node-id context so Lua can read `GetTriggerNode()`
    // inside the action, then dispatches through fire_event with the
    // player-id filter.
    void fire_node_event(std::string_view event_name, u32 player_id,
                         std::string_view node_id);

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
    void set_context_item(simulation::Item it)   { m_ctx_item = it; }
    void set_context_node_id(std::string id) { m_ctx_node_id = std::move(id); }
    void set_context_projectile(u32 id) { m_ctx_projectile = id; }
    void set_context_region_id(u32 id) { m_ctx_region_id = id; }
    void set_context_order_type(std::string s) { m_ctx_order_type = std::move(s); }
    void set_context_order_target_unit(u32 id) { m_ctx_order_target_unit = id; }
    void set_context_order_target_x(f32 x)     { m_ctx_order_target_x = x; }
    void set_context_order_target_y(f32 y)     { m_ctx_order_target_y = y; }

    // Configure Lua package.path so require() resolves from these directories.
    // Searched in order: scene scripts, shared scripts, engine scripts.
    void set_script_paths(std::string_view scene_scripts,
                          std::string_view shared_scripts,
                          std::string_view engine_scripts);

    // Set the save data directory for SaveData/LoadData persistence.
    void set_save_path(std::string_view save_dir);

    // Load and execute a Lua file
    bool load_script(std::string_view path);

    // Call a global Lua function by name
    // Calls a top-level Lua function by name. Returns true if the function
    // existed and returned without a Lua error; false if the function was
    // not defined or threw.
    bool call_function(std::string_view name);

    // Set a Lua global from a JSON value. Used by the worker to inject
    // the orchestrator-supplied `GAME_SESSION` blob before `main()` runs.
    // JSON arrays become 1-indexed Lua tables; objects become string-
    // keyed tables; scalars convert directly. Caller is responsible for
    // doing this between init and main().
    void set_global_from_json(std::string_view name, const nlohmann::json& value);

private:
    void bind_api();
    void bind_trigger_api();
    void bind_timer_api();
    void bind_input_api();
    void bind_save_api();
    void bind_hud_api();

    u32 next_trigger_id() { return ++m_next_trigger; }
    u32 next_timer_id()   { return ++m_next_timer; }

    std::unique_ptr<sol::state> m_lua;
    simulation::Simulation*     m_sim = nullptr;
    map::MapManager*            m_map = nullptr;
    audio::AudioEngine*      m_audio      = nullptr;
    i18n::LocaleManager*     m_i18n       = nullptr;
    AttachPointFn            m_attach_fn;
    EndGameFn                m_end_game_fn;
    UnitUpdateFn             m_unit_update_fn;
    BroadcastFn              m_broadcast_fn;
    EffectDeliverFn          m_effect_deliver_fn;
    EffectDestroyFn          m_effect_destroy_fn;
    SetSunDirectionFn        m_set_sun_direction_fn;
    u32                      m_player_count = 1;

    // Server-side tracker for active effects. Every effect — burst or
    // continuous — lives in the same list and follows the same Create
    // / Destroy lifecycle (WC3 convention: the author always destroys
    // the handle, even for one-shot bursts via the
    // `DestroyEffect(CreateEffect(...))` idiom). `delivered` is the
    // players the effect is currently rendering for — the dispatcher
    // adds on vision gain, removes on vision loss.
    struct ActiveEffect {
        u32         server_id;
        std::string name;
        glm::vec3   position;
        u32         entity_id;     // UINT32_MAX = free-position
        std::string attach_point;
        // Per-player target mask (bit N = player N). UINT32_MAX = broadcast.
        // ANDed with fog visibility at dispatch time, so a player outside
        // the mask never receives the effect even if they could see it.
        u32         target_mask = UINT32_MAX;
        std::unordered_set<u32> delivered;
    };
    std::vector<ActiveEffect> m_active_effects;
    u32                       m_next_effect_id = 1;

    // Visibility check for a single (player, effect) pair. Centralised
    // so the per-tick dispatcher and DestroyEffect's late-delivery
    // pass agree on what "visible" means.
    bool effect_visible_to(const ActiveEffect& e, u32 player_id) const;
    SceneSwitchFn            m_scene_switch_fn;
    CameraApplySetupFn          m_camera_apply_setup_fn;
    CameraSetTargetPositionFn   m_camera_set_target_position_fn;
    CameraSetSourceDistanceFn   m_camera_set_source_distance_fn;
    CameraShakeFn               m_camera_shake_fn;
    CameraSetTargetControllerFn m_camera_set_target_controller_fn;

    // Input (set via set_input)
    simulation::SelectionState* m_selection = nullptr;
    simulation::CommandSystem* m_commands = nullptr;

    // HUD (set via set_hud)
    hud::Hud*                m_hud       = nullptr;

    // Persistent save data (SaveData/LoadData)
    std::string m_save_path;
    nlohmann::json m_save_data;
    bool m_save_dirty = false;
    void flush_save_data();

    // Order event context
    std::string m_ctx_order_type;
    f32  m_ctx_order_target_x    = 0;
    f32  m_ctx_order_target_y    = 0;
    u32  m_ctx_order_target_unit = UINT32_MAX;
    u32  m_ctx_order_player      = UINT32_MAX;
    bool m_ctx_order_queued      = false;

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
    simulation::Item m_ctx_item{};  // for item events / item-sourced casts (GetTriggerItem)
    std::string m_ctx_node_id;   // which HUD node fired the event (button_pressed etc.)
    u32 m_ctx_region_id = UINT32_MAX;  // which region fired region_enter / region_leave
    u32 m_ctx_projectile = UINT32_MAX;  // projectile entity for PROJECTILE_HIT / PROJECTILE_DESTROYED

    bool m_paused        = false;  // PauseGame()/UnpauseGame() — App reads via is_paused()
    bool m_singleplayer  = false;  // IsSinglePlayer() — App sets once at init
    f32  m_game_speed    = 1.0f;   // SetGameSpeed — App's tick loop reads via game_speed()
};

} // namespace uldum::script
