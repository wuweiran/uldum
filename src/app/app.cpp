#include "app/app.h"
#include "core/log.h"
#include "hud/node.h"
#include "hud/hud_loader.h"
#include "hud/cast_indicator.h"
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
#include <thread>

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

#ifdef ULDUM_SHELL_UI
void App::update_shell_for_state() {
    if (!m_shell) return;
    // First call: snapshot current state without firing a transition,
    // because App::init has already loaded main_menu.rml manually.
    if (!m_shell_state_initialized) {
        m_shell_state = m_state;
        m_shell_state_initialized = true;
        return;
    }
    if (m_state == m_shell_state) return;
    AppState prev = m_shell_state;
    m_shell_state = m_state;

    // Options is a sub-screen of Menu — it's loaded by the Options
    // button click handler, not by AppState. So when re-entering Menu
    // (e.g. from Results → back) we always reload main_menu.rml,
    // regardless of whether Options was the previously-shown doc.
    switch (m_state) {
        case AppState::Menu:
            m_shell->load_document("shell/main_menu.rml");
            break;
        case AppState::Lobby:
            m_shell->load_document("shell/lobby.rml");
            break;
        case AppState::Loading:
            m_shell->load_document("shell/loading.rml");
            break;
        case AppState::Playing:
            // 3D scene + HUD own the screen during play.
            m_shell->hide_current_document();
            break;
        case AppState::Results:
            m_shell->load_document("shell/results.rml");
            break;
    }
    log::info(TAG, "Shell screen: {} -> {}",
              static_cast<int>(prev), static_cast<int>(m_state));
}
#endif

void App::refresh_safe_insets() {
    if (!m_platform) return;
    auto cur = m_platform->safe_insets();
    if (cur.left   == m_last_pushed_insets.left  &&
        cur.top    == m_last_pushed_insets.top   &&
        cur.right  == m_last_pushed_insets.right &&
        cur.bottom == m_last_pushed_insets.bottom) {
        return;
    }
    m_last_pushed_insets = cur;
    m_hud.set_safe_insets(hud::Hud::SafeInsets{
        cur.left, cur.top, cur.right, cur.bottom
    });
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
    // Authored HUD units are dp (1 dp = 1/160 inch). Platform reports
    // physical-pixels-per-dp — on Windows derived from the monitor
    // DPI, on Android from AConfiguration density. Setting once at
    // init is fine: a window resize doesn't change dp, only the total
    // px count that covers a given dp extent.
    if (m_platform) {
        m_hud.set_ui_scale(m_platform->ui_scale());
        m_hud.set_is_mobile(m_platform->is_mobile());
        // Safe-area insets for anchoring composites away from system bars
        // / notch. Desktop returns zeros so this is a no-op there; Android
        // reads GameActivity's SYSTEM_BARS union. May return zeros on
        // first call if the activity hasn't run its first layout pass
        // yet; the APP_CMD_WINDOW_INSETS_CHANGED event sets the platform's
        // resize flag once the real values land, which fires the main
        // loop's resize branch (refresh_safe_insets + on_viewport_resized).
        refresh_safe_insets();
    }

    // World overlays — unified ground-decal renderer for selection
    // rings, ability targeting indicators, future build-placement
    // ghosts and debug gizmos. One pipeline, one VBO, per-draw texture.
    if (!m_world_overlays.init(m_rhi)) {
        log::error(TAG, "World overlays init failed");
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
                // Menu → Lobby → Loading → Playing → Results. Lobby
                // currently auto-advances offline (no lobby UI here);
                // update_shell_for_state() drives RML loads on each
                // AppState transition.
                m_args.map_path = "maps/simple_map.uldmap";
                m_args.net_mode = network::Mode::Offline;
                if (!enter_lobby()) {
                    log::error(TAG, "Shell: enter_lobby failed");
                    return;
                }
                log::info(TAG, "Shell: 'play' -> Lobby");
                m_state = AppState::Lobby;
            }
        } else if (id == "quit") {
            log::info(TAG, "Shell: 'quit'");
            m_wants_quit = true;
        } else if (id == "options") {
            // Options is a sub-screen of Menu (no AppState change), so
            // it loads the document directly. update_shell_for_state
            // doesn't fire because m_state stays Menu.
            m_shell->load_document("shell/options.rml");
            bool snd = m_settings.get_bool("audio.master_enabled", true);
            m_shell->set_element_text("sound_toggle", snd ? "Sound: ON" : "Sound: OFF");
        } else if (id == "back") {
            // Back from Options (still in Menu state) or Results (state
            // change). For Results we transition to Menu; the next
            // update_shell_for_state will load main_menu.rml. For
            // Options we load it explicitly because state didn't change.
            if (m_state == AppState::Results) {
                m_state = AppState::Menu;
            } else {
                m_shell->load_document("shell/main_menu.rml");
            }
        } else if (id == "sound_toggle") {
            bool cur = m_settings.get_bool("audio.master_enabled", true);
            bool now = !cur;
            m_settings.set("audio.master_enabled", now);  // fires audio listener
            m_shell->set_element_text("sound_toggle", now ? "Sound: ON" : "Sound: OFF");
        }
    });

    // Initial menu shown explicitly — update_shell_for_state's first
    // call snapshots m_state without firing a transition, so without
    // this load nothing would render until the first state change.
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
    // Apply per-slot world-overlay texture overrides declared in
    // hud.json. Empty strings keep the engine's procedural defaults;
    // non-empty paths replace the slot's image with a map-supplied
    // KTX2. WorldOverlays caches one VkImage per slot, so this
    // happens once per session.
    {
        const auto& s = m_hud.cast_indicator_style();
        using TexId = render::WorldOverlays::TextureId;
        auto apply = [&](TexId id, const std::string& path) {
            if (!path.empty()) m_world_overlays.set_texture(id, path);
        };
        apply(TexId::SelectionRing, s.selection_texture);
        apply(TexId::RangeRing,     s.range_texture);
        apply(TexId::TargetUnit,    s.target_unit_texture);
        apply(TexId::CastCurve,     s.arrow_texture);
        apply(TexId::Reticle,       s.reticle_texture);
        apply(TexId::AoeCircle,     s.area_texture);
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

    // Mobile drag-cast commit. HUD has already collected the target
    // (snapped unit or ground point), so we bypass the preset's
    // targeting-mode entry path and submit a Cast command directly.
    // Same plumbing CommandSystem uses for everything else — local in
    // offline / host, network forward in client mode.
    m_hud.set_action_bar_cast_at_target_fn(
        [this](const std::string& ability_id, u32 target_unit_id,
               f32 target_x, f32 target_y, f32 target_z) {
            input::GameCommand cmd;
            cmd.player = m_selection.player();
            cmd.units  = m_selection.selected();
            simulation::orders::Cast c;
            c.ability_id = ability_id;
            if (target_unit_id != UINT32_MAX) {
                // Look up generation so the order references a stable
                // handle that survives ID reuse.
                const auto& world = m_server.simulation().world();
                if (auto* hi = world.handle_infos.get(target_unit_id)) {
                    c.target_unit.id = target_unit_id;
                    c.target_unit.generation = hi->generation;
                }
            }
            c.target_pos = glm::vec3{target_x, target_y, target_z};
            cmd.order = std::move(c);
            m_commands.submit(cmd);
        });

    // Command-bar slot tap → dispatches an engine-built-in command
    // ("stop", "move", etc.). Same plumbing as the ability callback
    // above; the preset handles the actual work.
    m_hud.set_command_bar_fn([this](const std::string& command_id) {
        if (m_input_preset) m_input_preset->queue_command(command_id);
    });

    // Inventory slot tap → cast the slot's first ability with the item
    // handle attached as `source_item`, so triggers reading
    // GetTriggerItem() inside on_cast_finished resolve to this item.
    // Build the GameCommand directly (bypassing queue_ability) because
    // the preset's path doesn't carry source_item — the HUD-already-
    // gated castable check ran in the press-release handler.
    m_hud.set_inventory_use_fn([this](u32 item_id, const std::string& ability_id) {
        const auto& world = m_server.simulation().world();
        const auto* hi = world.handle_infos.get(item_id);
        if (!hi) return;
        simulation::Item item;
        item.id         = item_id;
        item.generation = hi->generation;

        input::GameCommand cmd;
        cmd.player = m_selection.player();
        cmd.units  = m_selection.selected();
        simulation::orders::Cast c;
        c.ability_id  = ability_id;
        c.source_item = item;
        cmd.order = std::move(c);
        m_commands.submit(cmd);
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
    // Fog filter — entities in unscouted tiles drop out of pick_unit /
    // pick_target / pick_item, so smart-orders treat a click in the
    // fog as a ground click. Client mirrors fog from the server; host
    // / offline reads its own simulation fog directly.
    if (is_client) {
        m_picker.set_fog(&m_network.client_fog(), simulation::Player{m_args.local_slot});
    } else {
        m_picker.set_fog(&m_server.simulation().fog(), simulation::Player{m_args.local_slot});
    }

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
        m_hud_world_ctx.simulation   = &m_server.simulation();
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

    // Input — drop the preset (RTS / Action) and reset its dependents.
    m_input_preset.reset();
    m_bindings  = input::InputBindings{};
    m_commands  = input::CommandSystem{};
    m_selection = input::SelectionState{};

    // HUD — full session reset: widget tree, text tags, composite
    // configs + slot interaction, drag-cast, hidden-hotkey edges,
    // pointer state, callbacks. Detach from the world context first
    // so any hud-side update mid-tear-down can't read freed sim data.
    m_hud.set_world_context(nullptr);
    m_hud_world_ctx = hud::WorldContext{};
    m_hud.reset_session_state();

    // Audio — stop every active sound and drop the per-session sound
    // cache + miniaudio resource-manager registrations.
    m_audio.reset_session_state();

    // Networking + simulation + map. m_map.shutdown() also unmounts
    // the map's package from the AssetManager so mounts don't pile up
    // across sessions.
    m_network.shutdown();
    m_map.shutdown();
    m_server.shutdown();

    // Renderer — drop session-scoped resources. Animations must clear
    // *after* the simulation tears down so no in-flight render
    // references the instances. Also reset world-overlay slot
    // textures so a future map's slot defaults aren't shadowed by
    // the previous map's overrides.
    m_renderer.set_simulation(nullptr);
    m_renderer.set_terrain_data(nullptr);
    m_renderer.set_fog_grid(nullptr, 0, 0);
    m_renderer.end_session();
    m_world_overlays.reset_session_state();

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

        // Android surface lifecycle. Going to background fires
        // APP_CMD_TERM_WINDOW → platform clears the native window
        // pointer; coming back fires APP_CMD_INIT_WINDOW with a *new*
        // ANativeWindow. We need to skip rendering entirely while no
        // window exists, and rebuild the VkSurfaceKHR + swapchain the
        // moment a new one arrives — the old surface is bound to a
        // destroyed ANativeWindow and acquire on it would fail
        // catastrophically (this is the "black screen after resume"
        // bug). On desktop the handle is stable, so these are no-ops.
        void* native_win = m_platform->native_window_handle();
        if (!native_win) {
            // Paused — sleep briefly so we don't spin-poll the OS.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        if (native_win != m_rhi.native_window_handle()) {
            m_rhi.recreate_surface(*m_platform);
            // Re-push HUD viewport + insets — dims / ui_scale / insets
            // may have shifted across the background span.
            m_hud.set_ui_scale(m_platform->ui_scale());
            refresh_safe_insets();
            m_hud.on_viewport_resized(m_platform->width(), m_platform->height());
        }

        if (m_platform->was_resized()) {
            m_rhi.handle_resize(m_platform->width(), m_platform->height());
            f32 aspect = static_cast<f32>(m_platform->width()) / static_cast<f32>(m_platform->height());
            m_renderer.handle_resize(aspect);
            if (m_session_active)
                m_picker.set_screen_size(m_platform->width(), m_platform->height());
            // Re-query the platform's px-per-dp BEFORE re-anchoring HUD
            // composites. A window drag between monitors of different
            // DPI triggers WM_DPICHANGED → WM_SIZE on Windows; the new
            // scale must be in place before on_viewport_resized's
            // physical→dp conversion, otherwise composites anchor with
            // the old scale and jump next frame.
            m_hud.set_ui_scale(m_platform->ui_scale());
            // Android can change insets on rotation or system-bar
            // show/hide — refresh alongside the ui_scale query so the
            // on_viewport_resized below re-anchors composites correctly.
            refresh_safe_insets();
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
                // Prune dead / destroyed units from selection before the
                // preset runs. A unit the player had selected stays in
                // `m_selected` until something clears it — without this
                // step, rings + action_bar + commands keep pretending
                // the corpse is a live unit.
                {
                    const auto& world = m_server.simulation().world();
                    const auto& cur = m_selection.selected();
                    bool any_dead = false;
                    for (auto& u : cur) {
                        if (!simulation::is_alive(world, u)) { any_dead = true; break; }
                    }
                    if (any_dead) {
                        std::vector<simulation::Unit> live;
                        live.reserve(cur.size());
                        for (auto& u : cur) {
                            if (simulation::is_alive(world, u)) live.push_back(u);
                        }
                        m_selection.select_multiple(std::move(live));
                    }
                }

                const auto& in = m_platform->input();
                m_hud.handle_pointer(in.mouse_x, in.mouse_y, in.mouse_left);
                m_hud.handle_hotkeys(in);
                m_hud.joystick_update(in);
                m_hud.action_bar_drag_update(in);
                // Push targeting-mode state so the classic_rts render
                // highlights the armed slot. Reads empty when the preset
                // isn't waiting on a target.
                m_hud.action_bar_set_targeting_ability(
                    m_input_preset ? m_input_preset->targeting_ability_id()
                                   : std::string_view{});
                m_hud.command_bar_set_armed_command(
                    m_input_preset ? m_input_preset->active_command_id()
                                   : std::string_view{});

                // Same sub-tick interpolation factor the renderer uses.
                // Clients don't run the tick loop locally, so they pin
                // it at 1.0 (mirror snapshots are already at-the-tick).
                bool  is_client_now = (m_args.net_mode == network::Mode::Client);
                f32   preset_alpha  = is_client_now ? 1.0f : (accumulator / TICK_DT);

                f32 jx = 0.0f, jy = 0.0f;
                m_hud.joystick_vector(jx, jy);

                input::InputContext ictx{
                    m_platform->input(), m_selection, m_commands, m_picker,
                    m_renderer.camera(), m_bindings, m_server.simulation(),
                    m_platform->width(), m_platform->height(),
                    m_hud.input_captured(), preset_alpha,
                    jx, jy,
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
            // Game build: end the session immediately (tear down sim /
            // audio / network) on first entry. update_shell_for_state()
            // (called below) loads results.rml on the state transition;
            // we just patch in the elapsed-time label after the load.
            if (m_session_active) {
                end_session();
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

#ifdef ULDUM_SHELL_UI
        // Sync Shell document with the AppState. Loads / hides on
        // transitions only, no-op on steady state. Patch dynamic
        // results data after the document loads.
        bool was_results = (m_shell_state == AppState::Results);
        update_shell_for_state();
        if (m_shell && m_state == AppState::Results && !was_results) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Time: %.1f s", m_last_elapsed_seconds);
            m_shell->set_element_text("time_label", buf);
        }
#endif

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

                // World overlays — selection rings, ability indicators,
                // future build-placement ghosts. Drawn after the 3D
                // scene so they can be occluded by buildings, before
                // HUD so bars / labels stack above.
                {
                    m_world_overlays.begin_frame();
                    using TexId = render::WorldOverlays::TextureId;
                    const map::TerrainData* terrain =
                        m_map.terrain().is_valid() ? &m_map.terrain() : nullptr;

                    // ── Selection rings ──────────────────────────────
                    // Gated on the preset: action-style presets suppress
                    // them since the camera already tracks the
                    // controlled hero.
                    if (terrain && (!m_input_preset || m_input_preset->show_selection_circles())) {
                        constexpr u32  kSelectionSamples = 48;
                        constexpr f32  kSelectionStroke  = 4.0f;
                        constexpr glm::vec4 kColorLocal{ 0.24f, 1.00f, 0.36f, 0.8f };
                        constexpr glm::vec4 kColorOther{ 1.00f, 0.28f, 0.24f, 0.8f };
                        constexpr u32 kMaxSelectionRings = 48;

                        u32 emitted = 0;
                        std::vector<glm::vec3> samples;
                        samples.reserve(kSelectionSamples + 1);
                        for (auto unit : m_selection.selected()) {
                            if (emitted >= kMaxSelectionRings) break;
                            const auto* tf  = world.transforms.get(unit.id);
                            const auto* sel = world.selectables.get(unit.id);
                            if (!tf || !sel) continue;

                            glm::vec3 ip = tf->interp_position(alpha);
                            f32 base_r = (sel->selection_radius > 0.0f) ? sel->selection_radius : 48.0f;
                            // Center the stroke just inside the selection radius
                            // so the outer edge matches `base_r` (matches the
                            // previous SelectionCircles visual).
                            f32 ring_r = base_r - kSelectionStroke * 0.5f;
                            if (ring_r < kSelectionStroke * 0.5f) ring_r = kSelectionStroke * 0.5f;

                            samples.clear();
                            for (u32 i = 0; i <= kSelectionSamples; ++i) {
                                f32 a  = (static_cast<f32>(i % kSelectionSamples) / kSelectionSamples) * 6.28318530718f;
                                f32 sx = ip.x + ring_r * std::cos(a);
                                f32 sy = ip.y + ring_r * std::sin(a);
                                f32 sz = map::sample_height(*terrain, sx, sy);
                                samples.push_back({sx, sy, sz});
                            }
                            const auto* owner = world.owners.get(unit.id);
                            bool is_local = owner && owner->player.id == m_args.local_slot;
                            m_world_overlays.add_path(samples, kSelectionStroke,
                                                     is_local ? kColorLocal : kColorOther,
                                                     TexId::SelectionRing);
                            ++emitted;
                        }
                    }

                    // ── Ability targeting indicators ──────────────────
                    // Drawn after selection rings so a snapped target's
                    // ring sits on top of its selection ring.
                    auto aim = m_hud.aim_state();
                    if (aim.active) {
                        using Phase = hud::Hud::AimPhase;
                        const auto& s = m_hud.cast_indicator_style();
                        auto unpack = [](hud::Color c) -> glm::vec4 {
                            return { ((c.rgba >>  0) & 0xFFu) / 255.0f,
                                     ((c.rgba >>  8) & 0xFFu) / 255.0f,
                                     ((c.rgba >> 16) & 0xFFu) / 255.0f,
                                     ((c.rgba >> 24) & 0xFFu) / 255.0f };
                        };
                        // Phase tint behavior: Normal uses base RGB+A;
                        // OutOfRange / Cancelling fully replace with the
                        // configured tint color (cool blue / warm red).
                        auto phase_color = [&](hud::Color base) -> glm::vec4 {
                            switch (aim.phase) {
                                case Phase::Normal:     return unpack(base);
                                case Phase::OutOfRange: return unpack(s.out_of_range_tint);
                                case Phase::Cancelling: return unpack(s.cancel_tint);
                            }
                            return unpack(base);
                        };

                        glm::vec3 caster{aim.caster_x, aim.caster_y, aim.caster_z};
                        glm::vec3 drag  {aim.drag_x,   aim.drag_y,   aim.drag_z};

                        // 1) Range ring at caster — always neutral
                        //    (the ring is the reachability map,
                        //    independent of where the player is aiming).
                        m_world_overlays.add_ring(caster, aim.range,
                                                  s.range_thickness,
                                                  unpack(s.range_color),
                                                  TexId::RangeRing);

                        // 2) AoE indicator. Shape comes from the
                        //    ability's `shape` field; target_unit
                        //    abilities with area_radius > 0 still
                        //    draw a circle around the snapped unit.
                        //    Line and Cone always anchor at the
                        //    caster and orient toward the drag point.
                        if (aim.has_area) {
                            using Shape = hud::Hud::AimAreaShape;
                            switch (aim.area_shape) {
                                case Shape::Circle: {
                                    glm::vec3 area_at = drag;
                                    if (aim.is_unit_target && aim.snapped_id != UINT32_MAX) {
                                        area_at = glm::vec3{aim.snapped_x, aim.snapped_y, aim.snapped_z};
                                    }
                                    if (!aim.is_unit_target || aim.snapped_id != UINT32_MAX) {
                                        m_world_overlays.add_quad(area_at, aim.area_radius,
                                                                  phase_color(s.area_color),
                                                                  TexId::AoeCircle);
                                    }
                                    break;
                                }
                                case Shape::Line: {
                                    // Strip from caster, in caster→drag
                                    // direction, length = aim.range,
                                    // width = aim.area_width. Two-sample
                                    // path is enough since the line is
                                    // straight in XY (terrain z is
                                    // sampled per endpoint).
                                    f32 dx = drag.x - caster.x;
                                    f32 dy = drag.y - caster.y;
                                    f32 d  = std::sqrt(dx*dx + dy*dy);
                                    if (d > 1e-3f && aim.range > 0) {
                                        f32 inv = 1.0f / d;
                                        glm::vec3 end{
                                            caster.x + dx * inv * aim.range,
                                            caster.y + dy * inv * aim.range,
                                            drag.z   // approximate; flat path is fine for v1
                                        };
                                        std::vector<glm::vec3> samples = { caster, end };
                                        m_world_overlays.add_path(samples, aim.area_width,
                                                                  phase_color(s.area_color),
                                                                  TexId::AoeLine);
                                    }
                                    break;
                                }
                                case Shape::Cone: {
                                    // Wedge from caster, oriented toward
                                    // drag, half-angle from area_angle
                                    // (degrees → radians), radius = range.
                                    f32 dx = drag.x - caster.x;
                                    f32 dy = drag.y - caster.y;
                                    f32 d  = std::sqrt(dx*dx + dy*dy);
                                    if (d > 1e-3f && aim.range > 0 && aim.area_angle > 0) {
                                        glm::vec3 dir{ dx / d, dy / d, 0.0f };
                                        f32 half_angle_rad = aim.area_angle * 0.5f
                                                             * 3.14159265358979323846f / 180.0f;
                                        m_world_overlays.add_cone(caster, dir, half_angle_rad,
                                                                  aim.range,
                                                                  phase_color(s.area_color),
                                                                  TexId::AoeCone);
                                    }
                                    break;
                                }
                                case Shape::None: break;
                            }
                        }

                        // 3) Curved 3D arrow from caster ground to drag.
                        //    Only emitted in mobile drag-cast mode —
                        //    on desktop the player clicks to fire, so
                        //    there's no "drag from caster" semantics
                        //    and the arrow is just visual noise.
                        if (aim.is_drag_cast) {
                            constexpr u32 kCurveSamples = 24;
                            std::vector<glm::vec3> curve;
                            curve.reserve(kCurveSamples + 1);
                            for (u32 i = 0; i <= kCurveSamples; ++i) {
                                f32 t = static_cast<f32>(i) / static_cast<f32>(kCurveSamples);
                                glm::vec3 p = caster * (1.0f - t) + drag * t;
                                p.z += 4.0f * t * (1.0f - t) * s.arc_height;
                                curve.push_back(p);
                            }
                            m_world_overlays.add_path(curve, s.arrow_thickness,
                                                     phase_color(s.arrow_color),
                                                     TexId::CastCurve);
                        }

                        // 4) Reticle — shown for ground-target without
                        //    AoE, or for unit-target while not snapped.
                        bool show_reticle = !aim.is_unit_target ||
                                            (aim.snapped_id == UINT32_MAX);
                        if (aim.has_area && !aim.is_unit_target) show_reticle = false;
                        if (show_reticle) {
                            m_world_overlays.add_quad(drag, s.reticle_radius,
                                                      phase_color(s.reticle_color),
                                                      TexId::Reticle);
                        }

                        // 5) Snapped-unit ring.
                        if (aim.is_unit_target && aim.snapped_id != UINT32_MAX) {
                            glm::vec3 sp{aim.snapped_x, aim.snapped_y, aim.snapped_z};
                            m_world_overlays.add_ring(sp, aim.snapped_radius,
                                                      s.target_unit_thickness,
                                                      phase_color(s.target_unit_color),
                                                      TexId::TargetUnit);
                        }
                    }
                    m_world_overlays.draw(cmd, m_renderer.camera().view_projection());
                }
                // HUD overlay. Pointer is already dispatched earlier in
                // the frame (before input preset update) so its captured
                // state gates gameplay input correctly — here we just
                // build + render the draw list.
                {
                    m_hud.update_text_tags(frame_dt);
                    m_hud.begin_frame(m_rhi.extent().width, m_rhi.extent().height);
                    m_hud.draw_tree();
                    m_hud.draw_world_overlays(alpha);
                    // Box-select marquee (RTS preset's drag-rectangle).
                    // The preset records mouse coords in physical
                    // pixels (same space the Picker takes for world
                    // hits); HUD draw calls take dp. Convert once here
                    // so the rectangle tracks the cursor at any ui_scale.
                    if (m_input_preset) {
                        auto bs = m_input_preset->box_selection();
                        if (bs.active) {
                            f32 inv = 1.0f / m_hud.ui_scale();
                            m_hud.draw_marquee(bs.x0 * inv, bs.y0 * inv,
                                               bs.x1 * inv, bs.y1 * inv);
                        }
                    }
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
    m_world_overlays.shutdown();
    m_renderer.shutdown();
    m_asset.shutdown();
    m_rhi.shutdown();
    m_platform->shutdown();
    log::info(TAG, "=== All shut down ===");
}

} // namespace uldum
