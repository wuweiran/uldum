#include "app/app.h"
#include "core/log.h"
#include "hud/node.h"
#include "hud/hud_loader.h"
#include "hud/world.h"
#include "hud/text_tag.h"
#include "hud/action_bar.h"

#ifdef ULDUM_SHELL_UI
#include "shell/shell.h"
#include <nlohmann/json.hpp>
#endif

#ifdef ULDUM_DEV_UI
#include "app/dev_console.h"
#endif

#include <chrono>
#include <functional>

namespace uldum {

App::App()  = default;
App::~App() = default;

static constexpr const char* TAG = "App";
static constexpr float TICK_RATE = 32.0f;  // real-time ticks per second (always constant)
static constexpr float TICK_DT  = 1.0f / TICK_RATE;  // real-time interval between ticks

// Simple FNV-1a hash for map identity
static u32 hash_string(std::string_view s) {
    u32 h = 2166136261u;
    for (char c : s) { h ^= static_cast<u8>(c); h *= 16777619u; }
    return h;
}

simulation::World& App::active_world() {
    if (m_args.net_mode == network::Mode::Client)
        return m_network.client_world();
    return m_server.simulation().world();
}

bool App::init(const LaunchArgs& args) {
    m_args = args;
    log::info(TAG, "=== Initializing Uldum Engine ===");

    const char* mode_str = "Offline";
    if (args.net_mode == network::Mode::Host) mode_str = "Host";
    else if (args.net_mode == network::Mode::Client) mode_str = "Client";
    log::info(TAG, "Network mode: {}", mode_str);

#ifdef ULDUM_DEBUG
    log::set_level(log::Level::Trace);
    log::info(TAG, "Debug mode — verbose logging enabled");
#else
    log::set_level(log::Level::Info);
#endif

    // Platform
    m_platform = platform::Platform::create();
    platform::Config platform_config{};
    platform_config.title  = "Uldum Engine";
    platform_config.width  = 960;
    platform_config.height = 540;

    if (!m_platform->init(platform_config)) {
        log::error(TAG, "Platform init failed");
        return false;
    }

    // RHI
    rhi::Config rhi_config{};
#ifdef ULDUM_DEBUG
    rhi_config.enable_validation = true;
#else
    rhi_config.enable_validation = false;
#endif

    if (!m_rhi.init(rhi_config, *m_platform)) {
        log::error(TAG, "RHI init failed");
        return false;
    }

    // Asset manager — on Android, hand over the APK AAssetManager so engine.uldpak
    // and .uldmap files can be read from APK assets. Platform::asset_manager()
    // returns nullptr on desktop so this is a no-op there.
    if (!m_asset.init("engine", m_platform->asset_manager())) {
        log::error(TAG, "AssetManager init failed");
        return false;
    }

    // Verify asset pipeline with config load
    {
        auto cfg = m_asset.load_config("config/engine.json");
        if (cfg.is_valid()) {
            auto* doc = m_asset.get(cfg);
            if (doc && doc->data.contains("engine")) {
                auto& eng = doc->data["engine"];
                log::info(TAG, "Config loaded — engine: {} v{}",
                          eng.value("name", "?"), eng.value("version", "?"));
            }
        } else {
            log::error(TAG, "Engine config load FAILED");
        }
    }

    // Renderer
    if (!m_renderer.init(m_rhi)) {
        log::error(TAG, "Renderer init failed");
        return false;
    }

    // HUD — custom retained-mode UI for in-game overlays. Initialized once
    // alongside the renderer; lives across sessions.
    if (!m_hud.init(m_rhi)) {
        log::error(TAG, "HUD init failed");
        return false;
    }

    // Selection circles — ground rings under selected units. 3D decals,
    // rendered inside the main pass after the 3D scene but before HUD.
    if (!m_selection_circles.init(m_rhi)) {
        log::error(TAG, "Selection circles init failed");
        return false;
    }

    // HUD nodes are loaded from each map's `hud.json` at session start
    // (see start_session). App::init leaves the HUD tree empty.

    // Audio
    if (!m_audio.init()) {
        log::error(TAG, "AudioEngine init failed");
        return false;
    }

    // Settings wiring. Subsystems subscribe to the keys they care about;
    // the Shell UI flips values via click handlers (or, in Tier 2, via
    // Lua / data binding). Defaults are applied by calling set() once
    // so listeners get a consistent initial state.
    m_settings.subscribe("audio.master_enabled", [this](const settings::Value& v) {
        bool enabled = std::get_if<bool>(&v) ? std::get<bool>(v) : true;
        m_audio.set_volume(audio::Channel::Master, enabled ? 1.0f : 0.0f);
    });
    m_settings.set("audio.master_enabled", true);

    // Action-bar hotkey mode — "ability" (WC3 mnemonic) or "positional"
    // (MOBA grid). Player-level preference, not per-map. HUD consults
    // this every frame for both resolve + keyboard dispatch so a flip
    // takes effect immediately without a session restart.
    m_settings.subscribe("input.action_bar_hotkey_mode", [this](const settings::Value& v) {
        const std::string* s = std::get_if<std::string>(&v);
        auto mode = (s && *s == "positional")
                    ? hud::ActionBarHotkeyMode::Positional
                    : hud::ActionBarHotkeyMode::Ability;
        m_hud.action_bar_set_hotkey_mode(mode);
    });
    m_settings.set("input.action_bar_hotkey_mode", std::string("ability"));

    // Hardcoded local player name for now — carried over C_JOIN, shown in
    // lobbies, and surfaced to Lua as GetPlayerName(player). User-configurable
    // later (settings store, options screen).
#if defined(ULDUM_DEV_UI)
    m_network.set_player_name("Dev");
#elif defined(ULDUM_SHELL_UI)
    m_network.set_player_name("Player");
#else
    m_network.set_player_name("Player");
#endif

#ifdef ULDUM_DEV_UI
    // Dev console — engine-dev iteration UI. Replaces the CLI-driven
    // auto-start with an ImGui map picker + session controls.
    m_dev_console = std::make_unique<DevConsole>();
    if (!m_dev_console->init(m_rhi, *m_platform)) {
        log::error(TAG, "Dev console init failed");
        return false;
    }
#endif

#ifdef ULDUM_SHELL_UI
    // Shell UI — game builds only. Loads the main menu and routes its
    // button clicks into the App state machine.
    m_shell = std::make_unique<shell::Shell>();
    if (!m_shell->init(m_rhi, m_platform->width(), m_platform->height())) {
        log::error(TAG, "Shell UI init failed");
        return false;
    }

    // Click dispatch — each RML button's `id=` maps to an app action.
    // Commands (verbs) are handled directly; settings toggles go through
    // the settings store so subsystems react via their subscriptions.
    m_shell->set_click_handler([this](std::string_view id) {
        if (id == "play") {
            if (m_state == AppState::Menu) {
                // Same AppState sequence as uldum_dev: Menu → Lobby → Loading
                // → Playing → Results. Shell has no lobby RML yet, so the
                // main loop auto-advances through Lobby (see the Lobby case
                // in run()). When a game-build lobby screen lands, remove
                // that auto-advance and let the user click Start.
                m_args.map_path = "maps/simple_map.uldmap";
                m_args.net_mode = network::Mode::Offline;
                if (!enter_lobby()) {
                    log::error(TAG, "Shell: enter_lobby failed");
                    return;
                }
                log::info(TAG, "Shell: 'play' -> Lobby");
                m_shell->hide_current_document();
                m_state = AppState::Lobby;
            }
        } else if (id == "quit") {
            log::info(TAG, "Shell: 'quit'");
            m_wants_quit = true;
        } else if (id == "options") {
            m_shell->load_document("shell/options.rml");
            // Refresh the sound toggle's label to match current setting,
            // since the RML ships with "Sound: ON" as its static text.
            bool snd = m_settings.get_bool("audio.master_enabled", true);
            m_shell->set_element_text("sound_toggle", snd ? "Sound: ON" : "Sound: OFF");
        } else if (id == "back") {
            // Back works from both Options and Results. In Results we also
            // need to leave that state — session was already torn down when
            // we entered Results, so just transition to Menu.
            if (m_state == AppState::Results) m_state = AppState::Menu;
            m_shell->load_document("shell/main_menu.rml");
        } else if (id == "sound_toggle") {
            bool cur = m_settings.get_bool("audio.master_enabled", true);
            bool now = !cur;
            m_settings.set("audio.master_enabled", now);  // fires audio listener
            m_shell->set_element_text("sound_toggle", now ? "Sound: ON" : "Sound: OFF");
        }
    });

    m_shell->load_document("shell/main_menu.rml");
#endif

    log::info(TAG, "=== Engine subsystems initialized ===");
    return true;
}

// ── Per-session lifecycle ─────────────────────────────────────────────────

bool App::enter_lobby() {
    log::info(TAG, "=== Entering lobby: {} ===", m_args.map_path);

    // init_host needs a Simulation reference to accept C_ORDER mid-game,
    // so the sim object has to exist already. It's cheap — just creates an
    // empty type registry; map content loads later in start_session().
    if (!m_server.init_simulation(m_asset)) {
        log::error(TAG, "GameServer simulation init failed");
        return false;
    }

    if (!m_map.init()) {
        log::error(TAG, "MapManager init failed");
        return false;
    }

    // Lobby only reads manifest.json — slots, teams, colors, name. No
    // terrain, no preplaced units, no renderer state. That's deferred to
    // start_session() when the host commits the lobby.
    if (!m_map.load_manifest_only(m_args.map_path, m_asset)) {
        log::error(TAG, "Failed to load manifest for '{}'", m_args.map_path);
        return false;
    }

    // Per-mode network + lobby bring-up. Offline and Host populate the lobby
    // from their own manifest (host is authoritative). Client leaves the
    // lobby empty and waits for S_LOBBY_STATE from the host to arrive.
    u32 map_hash = hash_string(m_map.manifest().name + m_map.manifest().version);
    m_network.set_map_hash(map_hash);
    auto claim_first_open_as_me = [&](network::LobbyState& s) {
        for (auto& a : s.slots) {
            if (a.occupant == network::SlotOccupant::Open && !a.locked) {
                a.occupant     = network::SlotOccupant::Human;
                a.peer_id      = network::LOCAL_PEER;
                a.display_name = m_network.player_name();
                return;
            }
        }
    };

    switch (m_args.net_mode) {
    case network::Mode::Offline:
        m_network.init_offline();
        init_lobby_from_manifest(m_network.lobby_state(), m_args.map_path, m_map.manifest());
        claim_first_open_as_me(m_network.lobby_state());
        break;
    case network::Mode::Host:
        init_lobby_from_manifest(m_network.lobby_state(), m_args.map_path, m_map.manifest());
        claim_first_open_as_me(m_network.lobby_state());
        if (!m_network.init_host(m_args.port, 8, m_server.simulation(), m_commands)) {
            log::error(TAG, "Failed to start host listener on port {}", m_args.port);
            return false;
        }
        break;
    case network::Mode::Client:
        // Client: network connection initiates immediately, but the lobby
        // will populate from the host's S_LOBBY_STATE broadcast. A temporary
        // placeholder so the dev UI draws a "Connecting..." screen with the
        // map path the user picked (will be confirmed by the host).
        m_network.lobby_state().map_path = m_args.map_path;
        m_network.lobby_state().map_name = m_map.manifest().name;
        if (!m_network.init_client(m_args.connect_address, m_args.port)) {
            log::error(TAG, "Failed to connect to {}:{}", m_args.connect_address, m_args.port);
            return false;
        }
        break;
    }
    m_args.local_slot = 0;
    m_lobby_active = true;
    return true;
}

void App::leave_lobby() {
    if (!m_lobby_active) return;
    log::info(TAG, "=== Leaving lobby (not started) ===");
    m_renderer.set_simulation(nullptr);
    m_renderer.set_terrain_data(nullptr);
    m_renderer.set_fog_grid(nullptr, 0, 0);
    m_network.shutdown();
    m_map.shutdown();
    m_server.shutdown();
    m_lobby_active = false;
}

bool App::start_session() {
    if (!m_lobby_active) {
        log::error(TAG, "start_session called without a prepared lobby");
        return false;
    }
    log::info(TAG, "=== Loading session: {} (local slot {}) ===",
              m_args.map_path, m_args.local_slot);

    bool is_client = (m_args.net_mode == network::Mode::Client);

    // The heavy lift deferred from enter_lobby — tileset, types, terrain,
    // preplaced objects. Happens here so the Lobby screen is cheap to show
    // and the loading cost is paid behind a Loading screen with a ready
    // handshake.
    if (!m_map.load_content(m_asset, m_server.simulation())) {
        log::error(TAG, "Failed to load map content for '{}'", m_args.map_path);
        return false;
    }

    // Renderer-side setup moved from enter_lobby. Tileset textures, terrain
    // mesh, environment, and the scene camera pose are only meaningful once
    // the content is loaded.
    m_renderer.set_map_root(m_map.map_root());
    m_audio.set_map_root(m_map.map_root());
    {
        std::string effects_path = m_map.map_root() + "/types/effects.json";
        m_renderer.effect_registry().load_from_json(effects_path);
        m_renderer.effect_registry().resolve_textures(m_renderer.particles());
    }
    {
        std::vector<u8> shallow, deep;
        m_map.tileset().get_water_layer_ids(shallow, deep);
        m_map.terrain().set_water_layers(shallow, deep);
    }
    m_renderer.load_tileset_textures(m_map.tileset());
    m_renderer.set_environment(m_map.manifest().environment);
    if (m_map.terrain().is_valid()) {
        m_renderer.set_terrain(m_map.terrain());
        m_renderer.set_terrain_data(&m_map.terrain());
    }
    if (!m_map.scene().cameras.empty()) {
        const auto& cam = m_map.scene().cameras.front();
        m_renderer.camera().set_pose({cam.x, cam.y, cam.z}, cam.pitch, cam.yaw);
    }

    // HUD setup — runs on EVERY flavor (host, offline, client). Client
    // needs hud.json loaded for its own entity-bar config + name-label
    // rendering, and needs the NetworkManager → Hud wire so incoming
    // S_HUD_* messages apply to its local tree.
    m_hud.clear_nodes();
    {
        std::string hud_path = m_map.map_root() + "/hud.json";
        hud::load_from_asset(m_hud, hud_path,
                             m_rhi.extent().width, m_rhi.extent().height);
    }
    m_hud.set_local_player(m_args.local_slot);
    m_network.set_hud(&m_hud);
    // Button click callback. Host / offline fires the server trigger
    // directly; client forwards to host via C_NODE_EVENT.
    m_hud.set_button_event_fn([this](const std::string& node_id) {
        if (m_args.net_mode == network::Mode::Client) {
            m_network.send_node_event(node_id, network::NodeEventKind::ButtonPressed);
        } else {
            m_server.script().fire_node_event("button_pressed",
                                              m_args.local_slot, node_id);
        }
    });

    // Action-bar slot click → same path as pressing the ability's
    // hotkey. HUD pointer dispatch runs just before the preset update
    // each frame, so the queued request is consumed in that same
    // update's trailing flush. Client mode has no input preset today —
    // the callback is a no-op there until ability-cast-over-network
    // lands as its own task.
    m_hud.set_action_bar_cast_fn([this](const std::string& ability_id) {
        if (m_input_preset) m_input_preset->queue_ability(ability_id);
    });

    // Minimap click → jump the camera so its ground-focus point lands
    // at the clicked world coord. Preserves the current pitch/yaw so
    // the player's view angle stays consistent across jumps.
    m_hud.set_minimap_jump_fn([this](f32 wx, f32 wy) {
        auto& cam = m_renderer.camera();
        glm::vec3 pos = cam.position();
        glm::vec3 fwd = cam.forward_dir();
        if (fwd.z >= -0.001f) {
            // Camera looking at or above the horizon — fall back to a
            // direct XY snap without preserving the offset.
            cam.set_pose({ wx, wy, pos.z }, cam.pitch(), cam.yaw());
            return;
        }
        f32 t = -pos.z / fwd.z;
        glm::vec3 focus  = pos + t * fwd;
        glm::vec3 offset = pos - focus;
        cam.set_pose({ wx + offset.x, wy + offset.y, pos.z },
                     cam.pitch(), cam.yaw());
    });

    // Input wiring that the map's Lua may touch from main() — command
    // submission, selection, and the script→input bridge. Must happen
    // before `init_game` below, which runs the map's `main()` and is
    // where scripts call SetControlledUnit / IssueOrder / etc.
    if (!is_client) {
        m_commands.init(&m_server.simulation().world());
    } else {
        m_commands.init(nullptr);  // no local world
        m_commands.set_network_send([this](const input::GameCommand& cmd) {
            m_network.send_order(cmd);
        });
    }
    u32 local_player_id_early = is_client ? UINT32_MAX : m_args.local_slot;
    m_selection.set_player(simulation::Player{local_player_id_early});
    if (!is_client) {
        m_server.script().set_input(&m_selection, &m_commands);
    }

    // Game server phase 2 (offline/host only — client doesn't run the simulation)
    if (!is_client) {
        // Push finalized lobby names into the sim so Lua's GetPlayerName can
        // read them. Client receives names via the lobby snapshot mirror.
        std::vector<std::string> names;
        names.reserve(m_network.lobby_state().slots.size());
        for (const auto& a : m_network.lobby_state().slots) {
            names.push_back(a.display_name);
        }
        m_server.simulation().set_player_names(std::move(names));

        // Connect server Lua → HUD + → NetworkManager (for C_NODE_EVENT
        // handling). Both must be set before init_game runs the map's Lua.
        m_server.script().set_hud(&m_hud);
        m_network.set_script(&m_server.script());

        // Host mode: every local HUD mutation also emits a protocol packet
        // via NetworkManager::host_hud_sync. The network layer routes it
        // to the owning peer (or broadcasts). Offline skips sync entirely.
        if (m_args.net_mode == network::Mode::Host) {
            m_hud.set_sync_fn([this](const std::vector<u8>& pkt, u32 owner) {
                m_network.host_hud_sync(pkt, owner);
            });
        }

        if (!m_server.init_game(m_map,
                                &m_renderer.effect_registry(), &m_renderer.effect_manager(), &m_audio, &m_renderer)) {
            log::error(TAG, "GameServer game init failed");
            return false;
        }
    }

    if (m_args.net_mode == network::Mode::Host) {
        m_network.set_disconnect_timeout(m_map.manifest().disconnect_timeout);
        m_network.set_pause_on_disconnect(m_map.manifest().pause_on_disconnect);
    } else if (is_client) {
        m_network.set_type_registry(&m_server.simulation().types());
        m_network.init_client_fog(m_map.terrain(), m_map, m_server.simulation());
    }

    // Wire callbacks
    if (!is_client) {
        m_server.simulation().world().on_sound = [this](std::string_view path, glm::vec3 pos) {
            m_audio.play_sfx(path, pos);
        };
        m_server.script().set_attach_point_fn([this](u32 entity_id, std::string_view bone) {
            return m_renderer.get_attachment_point(entity_id, bone);
        });
        m_network.on_player_disconnected = [this](u32 player_id) {
            m_server.script().fire_event("global_disconnect", UINT32_MAX, "", player_id);
            m_server.script().fire_event("player_disconnect", UINT32_MAX, "", player_id);
        };
        m_network.on_player_dropped = [this](u32 player_id) {
            m_server.script().fire_event("global_leave", UINT32_MAX, "", player_id);
            m_server.script().fire_event("player_leave", UINT32_MAX, "", player_id);
        };
        m_server.script().set_unit_update_fn([this](u32 entity_id, const std::vector<u8>& pkt) {
            m_network.host_broadcast_update(entity_id, pkt);
        });
        m_server.script().set_end_game_fn([this](u32 winner_id, std::string_view stats) {
            if (m_args.net_mode == network::Mode::Host) {
                m_network.host_end_game(winner_id, stats);
            }
            log::info(TAG, "Game ended — winner: player {}", winner_id);

#ifdef ULDUM_SHELL_UI
            // Parse whatever the Lua script shipped in the stats JSON. The
            // "elapsed" key is the sample_map convention; other maps can
            // produce their own stats schema and Shell UIs their own
            // results.rml to render it.
            f32 elapsed = 0.0f;
            try {
                auto j = nlohmann::json::parse(stats);
                elapsed = j.value("elapsed", 0.0f);
            } catch (...) {
                // Stats wasn't valid JSON; keep elapsed=0 and move on.
            }
            m_last_elapsed_seconds = elapsed;
#endif
            m_state = AppState::Results;
        });
    } else {
        m_network.on_sound = [this](std::string_view path, glm::vec3 pos) {
            m_audio.play_sfx(path, pos);
        };
    }

    // Picking: needs camera + terrain, both ready after map content load.
    m_picker.init(&m_renderer.camera(), &m_map.terrain(),
                  &active_world(),
                  m_platform->width(), m_platform->height());

    // Input preset + bindings — preset is map-defined; bindings are the
    // user-customizable layer on top of preset defaults.
    m_input_preset = input::create_preset(m_map.manifest().input_preset);
    m_bindings.load(m_map.manifest().input_bindings_json);
    m_bindings.apply_defaults(input::rts_default_bindings());

    // Wire fog of war to renderer — local viewer = the slot this user picked
    // in the lobby. Previously hardcoded to 0, which meant Host always saw
    // slot-0's fog regardless of which slot they actually played.
    if (!is_client) {
        m_renderer.set_simulation(&m_server.simulation());
        m_renderer.set_local_player(m_args.local_slot);
    } else {
        // Client: renderer doesn't need simulation ref for fog filtering
        // (server already filters entities by fog)
        m_renderer.set_simulation(nullptr);
    }

    // HUD world-UI context: supplies world / fog / camera / picker / selection
    // / terrain / local player so draw_world_overlays() can iterate entities,
    // project positions, and filter by fog. Built here and held stable for
    // the session; cleared in end_session().
    {
        m_hud_world_ctx = hud::WorldContext{};
        if (is_client) {
            m_hud_world_ctx.world = &m_network.client_world();
            m_hud_world_ctx.fog   = &m_network.client_fog();
        } else {
            m_hud_world_ctx.world = &m_server.simulation().world();
            m_hud_world_ctx.fog   = &m_server.simulation().fog();
        }
        // Type registry: server/host owns one; client also needs it (set via
        // NetworkManager::set_type_registry earlier in start_session). Both
        // paths resolve to the same pointer — the server's simulation types.
        m_hud_world_ctx.types        = &m_server.simulation().types();
        m_hud_world_ctx.abilities    = &m_server.simulation().abilities();
        m_hud_world_ctx.camera       = &m_renderer.camera();
        m_hud_world_ctx.picker       = &m_picker;
        m_hud_world_ctx.selection    = &m_selection;
        m_hud_world_ctx.terrain      = m_map.terrain().is_valid() ? &m_map.terrain() : nullptr;
        m_hud_world_ctx.local_player = simulation::Player{m_args.local_slot};
        m_hud.set_world_context(&m_hud_world_ctx);
    }

    m_session_active = true;
    log::info(TAG, "=== Session started ===");
    return true;
}

void App::end_session() {
    if (!m_session_active) return;
    log::info(TAG, "=== Ending session ===");

    m_input_preset.reset();
    m_bindings = input::InputBindings{};
    m_commands = input::CommandSystem{};
    m_selection = input::SelectionState{};

    m_hud.set_world_context(nullptr);
    m_hud_world_ctx = hud::WorldContext{};
    m_hud.clear_nodes();
    m_network.shutdown();
    m_map.shutdown();
    m_server.shutdown();

    m_renderer.set_simulation(nullptr);
    m_renderer.set_terrain_data(nullptr);
    m_renderer.set_fog_grid(nullptr, 0, 0);
    // Free per-entity animation state so reused entity ids in the next
    // session don't inherit stale bone buffers (visible as detached
    // body parts). Runs after the simulation shuts down so no in-flight
    // render references the instances.
    m_renderer.end_session();

    m_session_active = false;
    m_lobby_active   = false;
    log::info(TAG, "=== Session ended ===");
}

// ── Main loop ─────────────────────────────────────────────────────────────

void App::run() {
    log::info(TAG, "Entering main loop (tick rate: {} Hz)", TICK_RATE);

    auto previous_time = std::chrono::high_resolution_clock::now();
    float accumulator = 0.0f;
    float game_speed = 1.0f;
    float game_time = 0.0f;
    u32 tick_counter = 0;

    m_state = AppState::Menu;
    log::info(TAG, "Entered Menu state");

    // Auto-start path: only taken when the CLI asked for it (`--map` on
    // uldum_dev). Otherwise the menu drives start (dev console / Shell UI).
    bool dev_auto_start = m_args.auto_start;

    while (m_platform->poll_events() && !m_wants_quit) {
        auto current_time = std::chrono::high_resolution_clock::now();
        float frame_dt = std::chrono::duration<float>(current_time - previous_time).count();
        previous_time = current_time;
        if (frame_dt > 0.25f) frame_dt = 0.25f;

        if (m_platform->was_resized()) {
            m_rhi.handle_resize(m_platform->width(), m_platform->height());
            f32 aspect = static_cast<f32>(m_platform->width()) / static_cast<f32>(m_platform->height());
            m_renderer.handle_resize(aspect);
            if (m_session_active)
                m_picker.set_screen_size(m_platform->width(), m_platform->height());
            // Re-anchor HUD composites against the new viewport so bars
            // stay pinned to their corners instead of the old extents.
            m_hud.on_viewport_resized(m_platform->width(), m_platform->height());
#ifdef ULDUM_SHELL_UI
            if (m_shell) m_shell->on_resize(m_platform->width(), m_platform->height());
#endif
        }

#ifdef ULDUM_SHELL_UI
        // Forward mouse state to the Shell UI every frame. We only care
        // about the menu screens here; gameplay input still flows through
        // the existing input_preset path.
        if (m_shell) {
            auto& in = m_platform->input();
            m_shell->on_mouse_move(static_cast<i32>(in.mouse_x), static_cast<i32>(in.mouse_y));
            if (in.mouse_left_pressed)  m_shell->on_mouse_button(0, true);
            if (in.mouse_left_released) m_shell->on_mouse_button(0, false);
            if (in.mouse_right_pressed)  m_shell->on_mouse_button(1, true);
            if (in.mouse_right_released) m_shell->on_mouse_button(1, false);
        }
#endif

#ifdef ULDUM_DEV_UI
        // Run the dev console's ImGui step outside the render pass, then
        // consume any action the user requested this frame.
        if (m_dev_console) {
            m_dev_console->update(frame_dt, m_state, m_network);
            auto action = m_dev_console->poll_action();
            using A = DevConsole::ActionType;
            if (action.type == A::EnterLobbyOffline && m_state == AppState::Menu) {
                m_args.map_path = action.map_path;
                m_args.net_mode = network::Mode::Offline;
                if (enter_lobby()) {
                    m_state = AppState::Lobby;
                    log::info(TAG, "Dev console: EnterLobby Offline '{}'", m_args.map_path);
                } else {
                    log::error(TAG, "enter_lobby failed");
                    leave_lobby();
                }
            } else if (action.type == A::EnterLobbyHost && m_state == AppState::Menu) {
                m_args.map_path = action.map_path;
                m_args.net_mode = network::Mode::Host;
                m_args.port     = action.port;
                if (enter_lobby()) {
                    m_state = AppState::Lobby;
                    log::info(TAG, "Dev console: EnterLobby Host '{}' port {}", m_args.map_path, m_args.port);
                } else {
                    leave_lobby();
                }
            } else if (action.type == A::EnterLobbyClient && m_state == AppState::Menu) {
                // 16b-ii: client still loads the map locally by path. 16b-iii
                // will negotiate the map (and slot table) with the server.
                m_args.map_path        = action.map_path;
                m_args.net_mode        = network::Mode::Client;
                m_args.connect_address = action.connect_address;
                m_args.port            = action.port;
                if (enter_lobby()) {
                    m_state = AppState::Lobby;
                    log::info(TAG, "Dev console: EnterLobby Client {}:{}", m_args.connect_address, m_args.port);
                } else {
                    leave_lobby();
                }
            } else if (action.type == A::ClaimSlot && m_state == AppState::Lobby) {
                m_network.send_claim_slot(action.slot);
            } else if (action.type == A::ReleaseSlot && m_state == AppState::Lobby) {
                m_network.send_release_slot(action.slot);
            } else if (action.type == A::StartGame && m_state == AppState::Lobby) {
                // Host-authority action. Commit lobby → Loading. Host broadcasts
                // S_LOBBY_COMMIT so all clients also enter Loading. Then every
                // peer loads map content in parallel and acks via C_LOAD_DONE.
                u32 my_peer = (m_args.net_mode == network::Mode::Client)
                    ? m_network.client_peer_id() : network::LOCAL_PEER;
                u32 my_slot = network::lobby_slot_for_peer(m_network.lobby_state(), my_peer);
                m_args.local_slot = (my_slot == UINT32_MAX) ? 0 : my_slot;
                if (m_args.net_mode == network::Mode::Host) {
                    m_network.host_commit_start();
                }
                m_state = AppState::Loading;
                log::info(TAG, "Dev console: StartGame (local slot {})", m_args.local_slot);
            } else if (action.type == A::LeaveLobby && m_state == AppState::Lobby) {
                leave_lobby();
                m_state = AppState::Menu;
            } else if (action.type == A::EndSession && m_session_active) {
                end_session();
                m_state = AppState::Menu;
            } else if (action.type == A::Quit) {
                m_wants_quit = true;
            }
        }
#endif

        switch (m_state) {
        case AppState::Menu:
            if (dev_auto_start) {
                dev_auto_start = false;
                // CLI `--map <path>` path: enter the lobby and let the Lobby
                // case auto-advance to Loading because m_args.auto_start is set.
                if (enter_lobby()) {
                    m_state = AppState::Lobby;
                    log::info(TAG, "Auto-start → Lobby");
                } else {
                    log::error(TAG, "Auto-start: enter_lobby failed");
                }
            }
            break;

        case AppState::Lobby:
            // Pump the network so host receives C_JOIN / C_CLAIM_SLOT /
            // C_RELEASE_SLOT and client receives S_LOBBY_STATE.
            m_network.update(frame_dt);
            // Client: host broadcast S_LOBBY_COMMIT → flip to Loading.
            if (m_args.net_mode == network::Mode::Client &&
                m_network.phase() == network::Phase::Loading) {
                u32 my_slot = network::lobby_slot_for_peer(
                    m_network.lobby_state(), m_network.client_peer_id());
                m_args.local_slot = (my_slot == UINT32_MAX) ? 0 : my_slot;
                m_state = AppState::Loading;
                log::info(TAG, "Client: S_LOBBY_COMMIT → Loading (slot {})", m_args.local_slot);
            }
            // Auto-advance Lobby → Loading when:
            //   - uldum_game's Shell UI triggered "play" (no lobby RML yet), or
            //   - uldum_dev was launched with `--map <path>` (bypass menu + lobby).
            // Offline only; networked paths wait for the host's commit.
            {
                bool skip_lobby_ui = false;
#ifdef ULDUM_SHELL_UI
                skip_lobby_ui = true;
#endif
                if (m_args.auto_start) skip_lobby_ui = true;
                if (skip_lobby_ui && m_args.net_mode == network::Mode::Offline) {
                    u32 my_slot = network::lobby_slot_for_peer(
                        m_network.lobby_state(), network::LOCAL_PEER);
                    m_args.local_slot = (my_slot == UINT32_MAX) ? 0 : my_slot;
                    m_state = AppState::Loading;
                    log::info(TAG, "Lobby (auto) → Loading (slot {})", m_args.local_slot);
                }
            }
            break;

        case AppState::Loading: {
            // Do the heavy map-content + renderer-setup load the first time
            // we enter Loading. `m_session_active` is flipped on at the end
            // of start_session() so the work runs exactly once.
            if (!m_session_active) {
                if (!start_session()) {
                    log::error(TAG, "Session failed to start → Menu");
                    end_session();
                    m_state = AppState::Menu;
                    break;
                }
                // Signal "I'm loaded". Host marks self locally; Client sends
                // C_LOAD_DONE. Offline skips the handshake entirely.
                if (m_args.net_mode == network::Mode::Host) {
                    m_network.mark_self_loaded();
                } else if (m_args.net_mode == network::Mode::Client) {
                    m_network.send_load_done();
                }
            }
            m_network.update(frame_dt);

            bool ready_to_play = false;
            if (m_args.net_mode == network::Mode::Offline) {
                ready_to_play = true;
            } else if (m_args.net_mode == network::Mode::Host) {
                if (m_network.all_peers_loaded()) {
                    m_network.host_finish_start();  // sends S_WELCOME + S_SPAWN + S_START
                    ready_to_play = true;
                }
            } else {  // Client
                if (m_network.phase() == network::Phase::Playing) {
                    ready_to_play = true;
                }
            }

            if (ready_to_play) {
                m_state = AppState::Playing;
                accumulator = 0; game_time = 0; tick_counter = 0;
                log::info(TAG, "Loading complete → Playing");
            }
            break;
        }

        case AppState::Playing: {
            bool is_client = (m_args.net_mode == network::Mode::Client);

            m_network.update(frame_dt);

            // Client: server disconnect handling lives in the UI. We stay
            // in Playing (frozen scene) and let the dev/Shell UI render a
            // "Lost connection" dialog. User picks End Session to return
            // to Menu; no auto-transition here. client_on_disconnect has
            // already logged the event.

            if (is_client && m_network.is_connected()) {
                auto lp = m_network.local_player();
                if (lp.is_valid() && lp.id != m_selection.player().id) {
                    m_selection.set_player(lp);
                    log::info(TAG, "Local player set to {}", lp.id);
                }
            }

            if (is_client && m_network.client_game_ended()) {
                m_state = AppState::Results;
                break;
            }

            bool should_tick = !is_client &&
                (m_args.net_mode == network::Mode::Offline || m_network.is_game_started()) &&
                !m_network.is_paused();
            if (should_tick) {
                float game_dt = TICK_DT * game_speed;
                accumulator += frame_dt;
                while (accumulator >= TICK_DT) {
                    auto t0 = std::chrono::steady_clock::now();
                    m_server.tick(game_dt);
                    auto t1 = std::chrono::steady_clock::now();
                    f32 tick_ms = std::chrono::duration<f32, std::milli>(t1 - t0).count();
                    if (tick_ms > 5.0f) {
                        log::warn(TAG, "Slow tick: {:.1f}ms (units: {})",
                                  tick_ms, active_world().transforms.count());
                    }
                    game_time += game_dt;
                    tick_counter++;
                    if (m_args.net_mode == network::Mode::Host)
                        m_network.host_broadcast_tick(tick_counter);
                    accumulator -= TICK_DT;
                }
            }

            {
                // HUD input dispatch runs BEFORE the input preset so:
                //   (1) slot clicks / slot hotkeys are queued as ability
                //       requests in time for the preset's trailing flush,
                //   (2) the preset can query `hud_captured` on the same
                //       frame to suppress pointer-driven selection and
                //       orders when the pointer is over UI.
                const auto& in = m_platform->input();
                m_hud.handle_pointer(in.mouse_x, in.mouse_y, in.mouse_left);
                m_hud.handle_action_bar_keys(in);
                // Push targeting-mode state so the classic_rts render
                // highlights the armed slot. Reads empty when the preset
                // isn't waiting on a target.
                m_hud.action_bar_set_targeting_ability(
                    m_input_preset ? m_input_preset->targeting_ability_id()
                                   : std::string_view{});

                // Same sub-tick interpolation factor the renderer uses.
                // Clients don't run the tick loop locally, so they pin
                // it at 1.0 (mirror snapshots are already at-the-tick).
                bool  is_client_now = (m_args.net_mode == network::Mode::Client);
                f32   preset_alpha  = is_client_now ? 1.0f : (accumulator / TICK_DT);

                input::InputContext ictx{
                    m_platform->input(), m_selection, m_commands, m_picker,
                    m_renderer.camera(), m_bindings, m_server.simulation(),
                    m_platform->width(), m_platform->height(),
                    m_hud.input_captured(), preset_alpha
                };
                m_input_preset->update(ictx, frame_dt);
            }

            {
                auto& cam = m_renderer.camera();
                m_audio.set_listener(cam.position(), cam.forward_dir(), glm::vec3{0, 0, 1});
            }
            m_audio.update();

            if (!is_client) {
                auto& fog = m_server.simulation().fog();
                if (fog.enabled()) {
                    const f32* visual = fog.update_visual(simulation::Player{m_args.local_slot}, frame_dt);
                    m_renderer.set_fog_grid(visual, fog.tiles_x(), fog.tiles_y());
                }
            } else {
                const f32* visual = m_network.update_client_fog(frame_dt);
                if (visual) {
                    m_renderer.set_fog_grid(visual, m_network.client_fog().tiles_x(),
                                            m_network.client_fog().tiles_y());
                }
            }
            break;
        }

        case AppState::Results:
#ifdef ULDUM_SHELL_UI
            // Game build: end the session immediately (tear down sim / audio /
            // network), then show the Results screen and stay put until the
            // user clicks "back". Loading the document is a one-shot — the
            // `m_results_shown` latch keeps us from reloading every frame.
            if (m_session_active) {
                end_session();
                if (m_shell) {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "Time: %.1f s", m_last_elapsed_seconds);
                    m_shell->load_document("shell/results.rml");
                    m_shell->set_element_text("time_label", buf);
                }
            }
#else
            // Engine-dev build (no Shell): auto-return to Menu. uldum_dev's
            // auto-start will kick the next session off on the following frame.
            log::info(TAG, "Session complete → ending session → Menu");
            end_session();
            m_state = AppState::Menu;
#endif
            break;
        }

        bool have_world = (m_state == AppState::Playing && m_session_active);
        if (have_world) {
            bool is_client = (m_args.net_mode == network::Mode::Client);
            auto& world = active_world();
            f32 alpha = is_client ? 1.0f : (accumulator / TICK_DT);
            auto r0 = std::chrono::steady_clock::now();
            VkCommandBuffer cmd = m_rhi.begin_frame();
            if (cmd && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
                m_renderer.upload_fog(cmd);
                m_renderer.draw_shadows(cmd, world, alpha);
                m_rhi.begin_rendering();
                m_renderer.draw(cmd, m_rhi.extent(), world, alpha);
                // Selection circles — ground rings under selected units.
                // Drawn after the 3D scene so they can be occluded by
                // buildings, before HUD so bars / labels stack above.
                const map::TerrainData* sel_terrain =
                    m_map.terrain().is_valid() ? &m_map.terrain() : nullptr;
                m_selection_circles.draw(cmd, m_renderer.camera(), world,
                                         sel_terrain,
                                         m_selection.selected(),
                                         simulation::Player{m_args.local_slot},
                                         alpha);
                // HUD overlay. Pointer is already dispatched earlier in
                // the frame (before input preset update) so its captured
                // state gates gameplay input correctly — here we just
                // build + render the draw list.
                {
                    m_hud.update_text_tags(frame_dt);
                    m_hud.begin_frame(m_rhi.extent().width, m_rhi.extent().height);
                    m_hud.draw_tree();
                    m_hud.draw_world_overlays(alpha);
                    m_hud.render(cmd);
                }
#ifdef ULDUM_SHELL_UI
                if (m_shell) {
                    m_shell->update(frame_dt);
                    m_shell->render(cmd, m_rhi.extent().width, m_rhi.extent().height);
                }
#endif
#ifdef ULDUM_DEV_UI
                if (m_dev_console) m_dev_console->render(cmd);
#endif
                m_rhi.end_frame();
            }
            auto r1 = std::chrono::steady_clock::now();
            f32 render_ms = std::chrono::duration<f32, std::milli>(r1 - r0).count();
            static f32 render_log_timer = 0;
            render_log_timer += frame_dt;
            if (render_log_timer >= 3.0f) {
                render_log_timer = 0;
                log::info(TAG, "Frame: {:.1f}ms, Render: {:.1f}ms, Units: {}",
                          frame_dt * 1000.0f, render_ms, world.transforms.count());
            }
        }
#ifdef ULDUM_SHELL_UI
        else if (m_shell) {
            // Non-Playing states (Menu, Loading, Results): no 3D scene, but
            // the Shell UI still renders. Open the render pass, draw only
            // Shell. Once a main menu wires up start_session, Menu state is
            // reachable before/between sessions.
            VkCommandBuffer cmd = m_rhi.begin_frame();
            if (cmd && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
                m_rhi.begin_rendering();
                m_shell->update(frame_dt);
                m_shell->render(cmd, m_rhi.extent().width, m_rhi.extent().height);
                m_rhi.end_frame();
            }
        }
#endif
#ifdef ULDUM_DEV_UI
        else if (m_dev_console) {
            // Non-Playing states in dev build: render the dev console menu
            // on a cleared background so the user can pick a map, host, etc.
            VkCommandBuffer cmd = m_rhi.begin_frame();
            if (cmd && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
                m_rhi.begin_rendering();
                m_dev_console->render(cmd);
                m_rhi.end_frame();
            }
        }
#endif
    }

    if (m_session_active) end_session();
    log::info(TAG, "Exiting main loop");
}

void App::shutdown() {
    log::info(TAG, "=== Shutting down engine subsystems ===");
    if (m_session_active) end_session();
#ifdef ULDUM_DEV_UI
    if (m_dev_console) { m_dev_console->shutdown(); m_dev_console.reset(); }
#endif
#ifdef ULDUM_SHELL_UI
    if (m_shell) m_shell.reset();
#endif
    m_audio.shutdown();
    m_hud.shutdown();
    m_selection_circles.shutdown();
    m_renderer.shutdown();
    m_asset.shutdown();
    m_rhi.shutdown();
    m_platform->shutdown();
    log::info(TAG, "=== All shut down ===");
}

} // namespace uldum
