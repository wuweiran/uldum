#include "app/engine.h"
#include "core/log.h"
#include "hud/node.h"
#include "hud/hud_loader.h"
#include "hud/hud_network.h"
#include "hud/cast_indicator.h"
#include "render/hud/world.h"
#include "hud/text_tag.h"
#include "hud/action_bar.h"

#ifdef ULDUM_SHELL_UI
#include "shell/shell.h"
#include <nlohmann/json.hpp>
#endif

#include "app/app.h"
// The concrete App class for this binary. CMake sets ULDUM_APP_HEADER
// (string) and ULDUM_APP_CLASS (identifier) per target — DevApp for
// dev builds, NullApp for game builds without their own App, the
// project's own class for converted game projects.
#include ULDUM_APP_HEADER
#ifdef ULDUM_DEV_UI
#include "app/dev_app.h"  // for the DevApp static_cast in the locale subscriber
#endif

#include <glm/trigonometric.hpp>   // glm::radians

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <thread>

namespace uldum {

Engine::Engine()  = default;
Engine::~Engine() = default;

static constexpr const char* TAG = "App";
static constexpr float TICK_RATE = 32.0f;  // real-time ticks per second (always constant)
static constexpr float TICK_DT  = 1.0f / TICK_RATE;  // real-time interval between ticks

simulation::World& Engine::active_world() {
    if (m_args.net_mode == network::Mode::Client)
        return m_network.client_world();
    return m_server.simulation().world();
}

void Engine::set_state(AppState s) {
    if (s == m_state) return;
    AppState prev = m_state;
    m_state = s;
    if (m_app) m_app->on_state_changed(prev, s);
}

void Engine::refresh_safe_insets() {
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

bool Engine::init(const LaunchArgs& args) {
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
    m_camera_controller.attach(&m_renderer.camera());

    // Let effect bursts spawn at the bone the author asked for (e.g.
    // "overhead") instead of always at the unit's feet. The renderer's
    // per-frame update already does this for continuous emitters; this
    // resolver covers the one-shot burst at create / play time. Captures
    // `this` so it re-reads active_world() each call (survives host ↔
    // client switches across sessions).
    m_renderer.effect_manager().set_unit_pos_resolver(
        [this](simulation::Unit u, std::string_view attach) -> glm::vec3 {
            auto& world = active_world();
            auto* t = world.transforms.get(u.id);
            if (!t) return {0, 0, 0};
            glm::vec3 pos = t->position;
            if (!attach.empty()) {
                pos += m_renderer.get_attachment_point(u.id, attach) * t->scale;
            }
            return pos;
        });

    // HUD — custom retained-mode UI for in-game overlays. Initialized once
    // alongside the renderer; lives across sessions. The data side (Hud)
    // holds the node tree + composite configs; HudRenderer owns the
    // Vulkan-side pipelines, ring buffers, and font atlas.
    if (!m_hud.init()) {
        log::error(TAG, "HUD init failed");
        return false;
    }
    if (!m_hud_renderer.init(m_hud, m_rhi)) {
        log::error(TAG, "HudRenderer init failed");
        return false;
    }
    m_hud.set_locale_manager(&m_i18n);
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
    // (see start_session). Engine::init leaves the HUD tree empty.

    // Audio
    if (!m_audio.init()) {
        log::error(TAG, "AudioEngine init failed");
        return false;
    }

    // Settings wiring. Subsystems subscribe to the keys they care about;
    // the Shell UI flips values via click handlers (or, in Tier 2, via
    // Lua / data binding). Defaults are applied by calling set() once
    // so listeners get a consistent initial state.
    //
    // Applicability contract (see docs/engine-model.md "Engine settings"):
    // most keys apply LIVE (the subscriber acts immediately, mid-session is
    // fine). A SESSION-LOCKED key (today only i18n.locale, below) instead
    // checks `m_session_active` and returns early with a [WARN] when a
    // session is running — the value is stored/persisted but applied next
    // session. Follow that same guard pattern for any future setting the
    // running session can't absorb.
    // Per-channel audio volumes (0..1). Each maps to a miniaudio sound
    // group via AudioEngine::set_volume — fully runtime. Defaults are
    // applied below after load(), only for keys the saved file didn't set.
    struct VolBinding { const char* key; audio::Channel ch; f32 def; };
    static constexpr VolBinding kVolumes[] = {
        { "audio.master_volume",  audio::Channel::Master,  1.0f },
        { "audio.sfx_volume",     audio::Channel::SFX,     1.0f },
        { "audio.music_volume",   audio::Channel::Music,   1.0f },
        { "audio.ambient_volume", audio::Channel::Ambient, 1.0f },
        { "audio.voice_volume",   audio::Channel::Voice,   1.0f },
    };
    for (const auto& vb : kVolumes) {
        audio::Channel ch = vb.ch;
        m_settings.subscribe(vb.key, [this, ch](const settings::Value& v) {
            f32 vol = std::get_if<f32>(&v) ? std::get<f32>(v) : 1.0f;
            m_audio.set_volume(ch, std::clamp(vol, 0.0f, 1.0f));
        });
    }

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

    // Graphics: vsync (RHI present mode) + fullscreen (window restyle).
    // Both apply live — vsync rebuilds the swapchain, fullscreen restyles
    // the window (the resulting resize rebuilds the swapchain too).
    m_settings.subscribe("graphics.vsync", [this](const settings::Value& v) {
        m_rhi.set_vsync(std::get_if<bool>(&v) ? std::get<bool>(v) : true);
    });
    m_settings.subscribe("graphics.fullscreen", [this](const settings::Value& v) {
        if (m_platform) m_platform->set_fullscreen(std::get_if<bool>(&v) ? std::get<bool>(v) : false);
    });

    // Load persisted settings (fires the subscribers above), then apply
    // defaults only for keys the file didn't carry — so a fresh install
    // starts sane and an existing settings.json wins where present.
    m_settings_path = m_platform->writable_data_dir() + "/settings.json";
    m_settings.load(m_settings_path);
    for (const auto& vb : kVolumes) {
        if (!m_settings.has(vb.key)) m_settings.set(vb.key, vb.def);
    }
    if (!m_settings.has("input.action_bar_hotkey_mode"))
        m_settings.set("input.action_bar_hotkey_mode", std::string("ability"));
    if (!m_settings.has("graphics.vsync"))      m_settings.set("graphics.vsync", true);
    if (!m_settings.has("graphics.fullscreen")) m_settings.set("graphics.fullscreen", false);

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

#ifdef ULDUM_SHELL_UI
    // Shell UI — game builds only. Built BEFORE the App so the App's
    // on_init can call engine.shell().load_document(...) for its
    // starting screen. Screen-specific bindings + RML loads now all
    // live in the App; engine.cpp no longer hosts a click dispatcher.
    m_shell = std::make_unique<shell::Shell>();
    if (!m_shell->init(m_rhi, m_platform->width(), m_platform->height())) {
        log::error(TAG, "Shell UI init failed");
        return false;
    }
#endif

    // Construct the App and run its on_init. The concrete class is
    // picked at compile time via the ULDUM_APP_CLASS macro CMake set
    // for this target — engine code stays agnostic of which it is.
    m_app = std::make_unique<ULDUM_APP_CLASS>();
    m_app->on_init(*this);

    // I18n: active locale comes from CLI (`--locale`) → settings store →
    // default "en". The engine ships no locale registry; game projects own
    // the list of supported locales (via their own locales.json + a
    // load_locale_registry call). Map pool loads later in start_session
    // once a map root is known.

    // Settings-driven locale switch. Changes outside a session apply
    // immediately and reload the shell pack; changes during a session are
    // logged + ignored (locale is fixed for the session duration).
    m_settings.subscribe("i18n.locale", [this](const settings::Value& v) {
        const std::string* code = std::get_if<std::string>(&v);
        if (!code || code->empty()) return;
        if (m_session_active) {
            log::warn(TAG, "Locale change to '{}' deferred — session in progress", *code);
            return;
        }
        m_i18n.set_active(*code);
        // Re-pick CJK TTC face so they render in the
        // matching script (NotoSansCJK on Android contains JP / KR / SC /
        // TC variants — face 0 is Japanese, which is wrong for zh-CN
        // users).
        m_hud_renderer.set_locale(*code);
    });
    // Apply locale with priority: CLI flag > persisted value > default.
    // load() already put any saved locale in the store, but it ran before
    // this subscriber was registered, so nothing has applied it yet.
    // Resolve the winner and set() it — that fires the subscriber above
    // (actually switching the locale) and leaves the store holding the
    // truth, so quit/relaunch round-trips the player's choice.
    std::string locale = "en";
    {
        settings::Value v = m_settings.get("i18n.locale");
        if (auto* s = std::get_if<std::string>(&v); s && !s->empty()) locale = *s;
    }
    if (!m_args.locale.empty()) locale = m_args.locale;  // CLI overrides saved
    m_settings.set("i18n.locale", locale);
#ifdef ULDUM_DEV_UI
    if (m_app) {
        static_cast<DevApp*>(m_app.get())
            ->set_active_locale(std::string(m_i18n.active()));
    }
#endif
    // Shell pool stays empty in engine builds — game projects load their
    // own shell strings from <project>/strings/<locale>/shell.json when
    // that wiring lands (game build only).

    // Raw fallback: when a map-pool entity-key lookup misses every locale
    // pack, fall through to the corresponding string field in
    // `types/<entity>.json`. Maps that don't localize at all get correct
    // names + tooltips automatically; partial translations fall through
    // per-key. The callback routes by the key's prefix to the right
    // registry — both TypeRegistry (unit / item / destructable / doodad)
    // and AbilityRegistry expose a `raw_string_field` for this.
    m_i18n.set_raw_fallback_fn([this](std::string_view key) -> std::optional<std::string> {
        // Engine-bound conventions translate the lookup-key suffix to the
        // raw JSON field name when they differ (only "name" for units,
        // items, destructables, doodads — all use "display_name" in JSON;
        // abilities use "name" directly).
        auto dot1 = key.find('.');
        if (dot1 == std::string_view::npos) return std::nullopt;
        auto rest = key.substr(dot1 + 1);
        auto dot2 = rest.find('.');
        if (dot2 == std::string_view::npos) return std::nullopt;
        std::string_view prefix    = key.substr(0, dot1);
        std::string_view entity_id = rest.substr(0, dot2);
        std::string_view field     = rest.substr(dot2 + 1);

        auto& types     = m_server.simulation().types();
        auto& abilities = m_server.simulation().abilities();
        using Cat = simulation::TypeRegistry::Category;

        if (prefix == "ability") {
            return abilities.raw_string_field(entity_id, field);
        }
        // Unit / Item: lookup key uses "name" but the raw JSON field
        // is "display_name" for units, and items accept either (loader
        // takes "name" or "display_name"). Try the conventional remap
        // first, fall back to the literal field name. Destructables /
        // doodads have no player-facing name today — the remap costs
        // nothing if the field is absent.
        Cat cat;
        if      (prefix == "unit")         cat = Cat::Unit;
        else if (prefix == "item")         cat = Cat::Item;
        else if (prefix == "destructable") cat = Cat::Destructable;
        else if (prefix == "doodad")       cat = Cat::Doodad;
        else return std::nullopt;

        if (field == "name") {
            if (auto v = types.raw_string_field(cat, entity_id, "display_name")) return v;
            if (auto v = types.raw_string_field(cat, entity_id, "name"))         return v;
            return std::nullopt;
        }
        return types.raw_string_field(cat, entity_id, field);
    });

    log::info(TAG, "=== Engine subsystems initialized ===");
    return true;
}

// ── Per-session lifecycle ─────────────────────────────────────────────────

bool Engine::enter_lobby() {
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
    // Map identity is the SHA-256 of all .lua files in the map; host and
    // joining clients compute the same digest from their local copies.
    m_network.set_map_hash(m_map.compute_script_hash(m_asset));
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
        // Forward the worker-session bearer token (when one is present)
        // so the worker's auth-on-join check passes. Empty token =
        // LAN / dev path; the worker's "no callback installed" default
        // accepts the join.
        m_network.set_auth_token(m_args.auth_token);
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

void Engine::leave_lobby() {
    if (!m_lobby_active) return;
    log::info(TAG, "=== Leaving lobby (not started) ===");
    m_renderer.set_simulation(nullptr);
    m_renderer.set_terrain(nullptr);
    m_renderer.set_fog_grid(nullptr, 0, 0);
    m_network.shutdown();
    m_map.shutdown();
    m_server.shutdown();
    m_lobby_active = false;
}

void Engine::fire_local_ping(const simulation::GameCommand& cmd) {
    if (auto ping = input::derive_target_ping(cmd, m_server.simulation())) {
        m_target_ping.unit     = ping->unit;
        m_target_ping.pos      = ping->pos;
        m_target_ping.kind     = ping->kind;
        m_target_ping.age      = 0.0f;
        m_target_ping.lifespan = 0.45f;
    }
}

bool Engine::start_session() {
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
        m_renderer.set_terrain(&m_map.terrain());
    }
    if (!m_map.scene().cameras.empty()) {
        const auto& cam = m_map.scene().cameras.front();
        m_renderer.camera().set_pose(
            {cam.target_x, cam.target_y, cam.target_z},
            cam.distance,
            glm::radians(cam.pitch_deg),
            glm::radians(cam.yaw_deg));
    }
    if (const auto& b = m_map.scene().camera_bounds) {
        m_renderer.camera().set_bounds({b->min_x, b->min_y}, {b->max_x, b->max_y});
    } else {
        m_renderer.camera().clear_bounds();
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
        apply(TexId::SnapTarget,    s.snap_target_texture);
        apply(TexId::CastCurve,     s.arrow_texture);
        apply(TexId::Reticle,       s.reticle_texture);
        apply(TexId::AoeCircle,     s.area_texture);
        apply(TexId::AoeCone,       s.area_cone_texture);
        apply(TexId::AoeLine,       s.area_line_texture);
    }
    m_hud.set_local_player(m_args.local_slot);
    m_network.set_hud_message_fn([this](std::span<const u8> data) {
        hud::apply_network_message(m_hud, data);
    });
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
            simulation::GameCommand cmd;
            cmd.player = m_selection.player();
            cmd.units  = m_selection.selected();
            simulation::orders::Cast c;
            c.ability_id = ability_id;
            if (target_unit_id != UINT32_MAX) {
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

    // Mobile command-bar drag-commit (Phase 5a). The HUD has already
    // resolved the gesture into a snapped unit (Attack target) or a
    // ground point (Move / AttackMove fallback). We bypass the
    // preset's targeting-mode entry path and submit the matching
    // order directly — same shape as the ability cast-at-target fn
    // above.
    m_hud.set_command_bar_drag_commit_fn(
        [this](const std::string& command_id, u32 target_unit_id,
               f32 target_x, f32 target_y, f32 target_z) {
            simulation::GameCommand cmd;
            cmd.player = m_selection.player();
            cmd.units  = m_selection.selected();
            const glm::vec3 wp{target_x, target_y, target_z};
            if (command_id == "move") {
                simulation::orders::Move m;
                if (target_unit_id != UINT32_MAX) {
                    // Snapped to a unit → Follow that unit (Move with
                    // target_unit; range > 0 so the unit stops at a
                    // comfortable trailing distance instead of clipping
                    // into the leader's collider).
                    const auto& world = m_server.simulation().world();
                    if (auto* hi = world.handle_infos.get(target_unit_id)) {
                        simulation::Unit u;
                        u.id         = target_unit_id;
                        u.generation = hi->generation;
                        m.target_unit = u;
                        m.range       = 96.0f;
                    } else {
                        m.target = wp;   // handle invalid mid-frame — fall back to ground
                    }
                } else {
                    m.target = wp;
                }
                cmd.order = std::move(m);
            } else if (command_id == "attack" || command_id == "attack_move") {
                if (target_unit_id != UINT32_MAX) {
                    // Snapped to a unit → Attack on that unit.
                    const auto& world = m_server.simulation().world();
                    simulation::Unit u;
                    u.id = target_unit_id;
                    if (auto* hi = world.handle_infos.get(target_unit_id))
                        u.generation = hi->generation;
                    cmd.order = simulation::orders::Attack{u};
                } else {
                    // No snap → AttackMove on the ground point.
                    cmd.order = simulation::orders::AttackMove{wp};
                }
            } else {
                return;   // unknown command — drop silently
            }
            m_commands.submit(cmd);
            fire_local_ping(cmd);   // mobile command-bar drag: ping like desktop
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

        simulation::GameCommand cmd;
        cmd.player = m_selection.player();
        cmd.units  = m_selection.selected();
        simulation::orders::Cast c;
        c.ability_id  = ability_id;
        c.source_item = item;
        cmd.order = std::move(c);
        m_commands.submit(cmd);
    });

    // Mobile inventory drag-cast — drag from a slot onto the world,
    // release to commit. Same Cast order as the no-target use path,
    // but with target_pos / target_unit pre-filled from the gesture
    // so the simulation skips the targeting prompt and resolves the
    // ability immediately.
    m_hud.set_inventory_use_at_target_fn(
        [this](u32 item_id, const std::string& ability_id,
               u32 target_unit_id, glm::vec3 world_pos) {
            const auto& world = m_server.simulation().world();
            const auto* hi = world.handle_infos.get(item_id);
            if (!hi) return;
            simulation::Item item;
            item.id         = item_id;
            item.generation = hi->generation;

            simulation::GameCommand cmd;
            cmd.player = m_selection.player();
            cmd.units  = m_selection.selected();
            simulation::orders::Cast c;
            c.ability_id  = ability_id;
            c.source_item = item;
            c.target_pos  = world_pos;
            if (target_unit_id != UINT32_MAX) {
                if (const auto* th = world.handle_infos.get(target_unit_id)) {
                    simulation::Unit tu;
                    tu.id         = target_unit_id;
                    tu.generation = th->generation;
                    c.target_unit = tu;
                }
            }
            cmd.order = std::move(c);
            m_commands.submit(cmd);
        });

    // Inventory drop — held-then-clicked-on-terrain (WC3 style). The
    // sim places the item at the explicit world pos passed in.
    m_hud.set_inventory_drop_fn([this](u32 item_id, i32 /*slot*/, glm::vec3 world_pos) {
        const auto& world = m_server.simulation().world();
        const auto* hi = world.handle_infos.get(item_id);
        if (!hi) return;
        simulation::Item item;
        item.id         = item_id;
        item.generation = hi->generation;

        simulation::GameCommand cmd;
        cmd.player = m_selection.player();
        cmd.units  = m_selection.selected();
        simulation::orders::DropItem d;
        d.item = item;
        d.pos  = world_pos;
        cmd.order = std::move(d);
        m_commands.submit(cmd);
    });

    // Inventory drag-swap — left-press on slot A, drag to B, release.
    // Same Cast / Drop pipeline, just a different order kind.
    m_hud.set_inventory_swap_fn([this](i32 slot_a, i32 slot_b) {
        simulation::GameCommand cmd;
        cmd.player = m_selection.player();
        cmd.units  = m_selection.selected();
        cmd.order  = simulation::orders::SwapInventorySlot{slot_a, slot_b};
        m_commands.submit(cmd);
    });

    // Minimap click → jump the camera so its ground-focus point lands
    // at the clicked world coord. Preserves the current pitch/yaw so
    // the player's view angle stays consistent across jumps.
    m_hud.set_minimap_jump_fn([this](f32 wx, f32 wy) {
        // Target-based camera: minimap-click snaps the look-at point
        // to the clicked ground location. Distance / pitch / yaw stay.
        m_renderer.camera().set_target_xy(wx, wy);
    });

    // Input wiring that the map's Lua may touch from main() — command
    // submission, selection, and the script→input bridge. Must happen
    // before `init_game` below, which runs the map's `main()` and is
    // where scripts call SetControlledUnit / IssueOrder / etc.
    if (!is_client) {
        m_commands.init(&m_server.simulation().world());
    } else {
        m_commands.init(nullptr);  // no local world
        m_commands.set_network_send([this](const simulation::GameCommand& cmd) {
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
        m_server.script().set_locale_manager(&m_i18n);
        m_network.set_script(&m_server.script());

        // Surface the launch mode to Lua before main() runs so scripts
        // can branch on IsSinglePlayer() at scene-init time (e.g.
        // build SP-only HUD nodes, register pause-aware triggers).
        m_server.script().set_singleplayer(m_args.net_mode == network::Mode::Offline);

        // Host mode: every local HUD mutation also emits a protocol packet
        // via NetworkManager::host_hud_sync. The network layer routes it
        // to the owning peer (or broadcasts). Offline skips sync entirely.
        if (m_args.net_mode == network::Mode::Host) {
            m_hud.set_sync_fn([this](const std::vector<u8>& pkt, u32 owner) {
                m_network.host_hud_sync(pkt, owner);
            });
        }

        // SetSunDirection (Lua) updates the host's own renderer here;
        // the network broadcast to peers happens inside the binding
        // via m_broadcast_fn (script.cpp).
        m_server.script().set_sun_direction_fn([this](f32 x, f32 y, f32 z) {
            map::EnvironmentConfig env;
            env.sun_direction = glm::normalize(glm::vec3{x, y, z});
            m_renderer.set_environment(env);
        });
        if (!m_server.init_game(m_map, &m_audio)) {
            log::error(TAG, "GameServer game init failed");
            return false;
        }
    }

    if (m_args.net_mode == network::Mode::Host) {
        m_network.set_disconnect_timeout(m_map.manifest().disconnect_timeout);
        m_network.set_pause_on_disconnect(m_map.manifest().pause_on_disconnect);
    } else if (is_client) {
        m_network.set_type_registry(&m_server.simulation().types());
        m_network.set_ability_registry(&m_server.simulation().abilities());
        m_network.init_client_fog(m_map.terrain(), m_map, m_server.simulation());
        // Route the never-ticked server simulation's world/vision
        // accessors to the network-mirrored state so input presets,
        // HUD, target_filter_passes, and is_allied/is_enemy queries
        // (which all reach in via m_server.simulation()) see the real
        // entities and fog instead of the empty server-side defaults.
        m_server.simulation().set_world_override(&m_network.client_world());
        m_server.simulation().set_vision_override(&m_network.client_vision());
    }

    // Wire callbacks
    if (!is_client) {
        m_server.simulation().world().on_sound = [this](std::string_view path, glm::vec3 pos) {
            m_audio.play_sfx(path, pos);
        };
        // Birth-clip gate: a unit plays its birth animation only when
        // spawned in the local player's sight. Mirrors the network
        // client, which derives the same from the S_SPAWN newly_created
        // flag. create_unit consults this; null on the headless server.
        m_server.simulation().world().spawn_visible_to_viewer =
            [this](f32 x, f32 y) -> bool {
                auto& sim = m_server.simulation();
                const auto* terrain = sim.terrain();
                if (!terrain) return true;   // pre-terrain spawns: don't suppress
                glm::ivec2 t = terrain->world_to_tile(x, y);
                if (t.x < 0 || t.y < 0) return false;
                return sim.vision().is_visible(simulation::Player{m_args.local_slot},
                                               static_cast<u32>(t.x), static_cast<u32>(t.y));
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
        m_server.script().set_broadcast_fn([this](const std::vector<u8>& pkt) {
            m_network.host_broadcast(pkt);
        });
        m_server.script().set_player_count(
            static_cast<u32>(m_map.manifest().players.size()));
        // Fog-aware effect delivery. ScriptEngine::update calls these
        // per (player, effect) once that player's vision covers the
        // effect's position. For the host's local player we drive the
        // renderer directly; for peers we send the appropriate packet.
        // Deliver and destroy go through the same Create/Destroy code
        // path on the client — no burst-vs-persistent split. Particles
        // already in flight fade out on their own; the EffectInstance
        // is what the destroy actually removes.
        m_server.script().set_effect_deliver_fn(
            [this](u32 player_id, u32 server_id, std::string_view name,
                   u32 entity_id, glm::vec3 pos, std::string_view attach_point) {
                if (player_id == m_args.local_slot) {
                    auto& mgr = m_renderer.effect_manager();
                    u32 local_id;
                    if (entity_id == UINT32_MAX) {
                        local_id = mgr.create(std::string(name), pos);
                    } else {
                        simulation::Unit u;
                        u.id = entity_id;
                        auto* info = m_server.simulation().world().handle_infos.get(entity_id);
                        u.generation = info ? info->generation : 0;
                        local_id = mgr.create_on_unit(std::string(name), u, pos,
                                                      std::string(attach_point));
                    }
                    m_effect_id_map[server_id] = local_id;
                } else {
                    auto pkt = network::build_effect_create(server_id, name, entity_id,
                                                            pos, attach_point);
                    m_network.host_send_to_player(player_id, pkt);
                }
            });
        m_server.script().set_effect_destroy_fn(
            [this](u32 player_id, u32 server_id) {
                if (player_id == m_args.local_slot) {
                    auto it = m_effect_id_map.find(server_id);
                    if (it != m_effect_id_map.end()) {
                        m_renderer.effect_manager().destroy(it->second);
                        m_effect_id_map.erase(it);
                    }
                } else {
                    auto pkt = network::build_effect_destroy(server_id);
                    m_network.host_send_to_player(player_id, pkt);
                }
            });
        // Ability lifecycle broadcasts — fire from every add / remove
        // path (Lua, engine aura ticks, natural duration expiry) and
        // ride the same per-entity visibility filter as other
        // S_UPDATE packets.
        m_server.simulation().world().on_ability_added =
            [this](simulation::Unit unit, std::string_view ability_id, u32 level) {
                auto pkt = network::build_update_ability_add(unit.id, ability_id, level);
                m_network.host_broadcast_update(unit.id, pkt);
            };
        m_server.simulation().world().on_ability_removed =
            [this](simulation::Unit unit, std::string_view ability_id) {
                auto pkt = network::build_update_ability_remove(unit.id, ability_id);
                m_network.host_broadcast_update(unit.id, pkt);
            };
        // Renderer-owned hook so the simulation can match projectile
        // death timers to the actual animation clip duration.
        m_server.simulation().world().get_clip_duration =
            [this](std::string_view model_path, std::string_view clip_name) -> f32 {
                return m_renderer.clip_duration(model_path, clip_name);
            };
        register_script_camera_callbacks();
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
            set_state(AppState::Results);
        });
        // Lua-driven scene swap. The actual heavy lift (entity reset,
        // script reload, main() rerun) happens in perform_scene_switch
        // off the main loop — this just defers the request.
        m_server.script().set_scene_switch_fn(
            [this](std::string_view scene) {
                m_pending_scene_switch.assign(scene);
            });
    } else {
        m_network.on_sound = [this](std::string_view path, glm::vec3 pos) {
            m_audio.play_sfx(path, pos);
        };
        // Script-initiated audio mirrors the Lua API surface 1:1.
        m_network.on_sound_2d = [this](std::string_view path) {
            m_audio.play_sfx_2d(path);
        };
        m_network.on_music_play = [this](std::string_view path, f32 fade_in) {
            m_audio.play_music(path, fade_in);
        };
        m_network.on_music_stop = [this](f32 fade_out) {
            m_audio.stop_music(fade_out);
        };
        // Ambient loops are host-handle-keyed. We map the host's
        // handle to the local AudioEngine handle returned by our own
        // play_ambient call, then look up on stop.
        m_network.on_ambient_start = [this](u32 host_handle, std::string_view path, f32 x, f32 y) {
            auto h = m_audio.play_ambient(path, {x, y, 0});
            m_client_ambient_handles[host_handle] = h.id;
        };
        m_network.on_ambient_stop = [this](u32 host_handle, f32 fade_out) {
            auto it = m_client_ambient_handles.find(host_handle);
            if (it == m_client_ambient_handles.end()) return;
            m_audio.stop_ambient({it->second}, fade_out);
            m_client_ambient_handles.erase(it);
        };
        m_network.on_set_sun_direction = [this](f32 x, f32 y, f32 z) {
            map::EnvironmentConfig env;
            env.sun_direction = glm::normalize(glm::vec3{x, y, z});
            m_renderer.set_environment(env);
        };
        // CreateEffect: persistent effect with stable server-assigned
        // id. Track server→local handle so a later destroy can find
        // its EffectManager instance.
        m_network.on_effect_create = [this](u32 server_id, std::string_view name,
                                              u32 entity_id, glm::vec3 pos,
                                              std::string_view attach_point) {
            auto& mgr = m_renderer.effect_manager();
            u32 local_id;
            if (entity_id == UINT32_MAX) {
                local_id = mgr.create(std::string(name), pos);
            } else {
                simulation::Unit u;
                u.id = entity_id;
                auto* info = m_network.client_world().handle_infos.get(entity_id);
                u.generation = info ? info->generation : 0;
                local_id = mgr.create_on_unit(std::string(name), u, pos,
                                              std::string(attach_point));
            }
            m_effect_id_map[server_id] = local_id;
        };
        m_network.on_effect_destroy = [this](u32 server_id) {
            auto it = m_effect_id_map.find(server_id);
            if (it != m_effect_id_map.end()) {
                m_renderer.effect_manager().destroy(it->second);
                m_effect_id_map.erase(it);
            }
        };
        // Host-driven scene swap. NetworkManager fires this when an
        // S_SCENE_SWITCH arrives; the App tears down local scene
        // state inline (terrain swap, sim wipe, HUD/picker reset,
        // camera re-pose). NetworkManager handles the C_LOAD_DONE
        // ack right after the callback returns. The host then bursts
        // S_SPAWN/S_HUD_* messages once every peer has acked, so the
        // client renders the new scene as those deltas arrive.
        m_network.set_scene_switch_recv_fn([this](std::string_view scene) {
            scene_switch_local_teardown(std::string(scene));
        });
        // Scripted-camera apply. The host has already chosen this
        // client as the recipient; just hand off to the controller.
        m_network.set_camera_apply_setup_recv_fn(
            [this](f32 tx, f32 ty, f32 tz, f32 distance,
                   f32 pitch_rad, f32 yaw_rad, f32 duration) {
                m_camera_controller.apply_setup({tx, ty, tz}, distance,
                                                  pitch_rad, yaw_rad, duration);
            });
        m_network.set_camera_set_target_position_recv_fn(
            [this](f32 x, f32 y, f32 z, f32 d) {
                m_camera_controller.set_target_position(x, y, z, d);
            });
        m_network.set_camera_set_source_distance_recv_fn(
            [this](f32 distance, f32 d) {
                m_camera_controller.set_source_distance(distance, d);
            });
        m_network.set_camera_shake_recv_fn([this](f32 i, f32 d) {
            m_camera_controller.shake(i, d);
        });
        m_network.set_camera_set_target_controller_recv_fn([this](u32 entity_id) {
            if (entity_id == UINT32_MAX) {
                m_camera_controller.unlock_unit();
            } else {
                // Resolve to a stable Unit handle (with generation) so
                // the controller's lookup-fn can detect deaths.
                simulation::Unit u; u.id = entity_id;
                if (auto* hi = m_server.simulation().world().handle_infos.get(entity_id)) {
                    u.generation = hi->generation;
                }
                m_camera_controller.lock_unit(u);
            }
        });
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
        m_picker.set_vision(&m_network.client_vision(), simulation::Player{m_args.local_slot});
    } else {
        m_picker.set_vision(&m_server.simulation().vision(), simulation::Player{m_args.local_slot});
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
            m_hud_world_ctx.vision = &m_network.client_vision();
        } else {
            m_hud_world_ctx.world  = &m_server.simulation().world();
            m_hud_world_ctx.vision = &m_server.simulation().vision();
        }
        // Type registry: server/host owns one; client also needs it (set via
        // NetworkManager::set_type_registry earlier in start_session). Both
        // paths resolve to the same pointer — the server's simulation types.
        m_hud_world_ctx.types        = &m_server.simulation().types();
        m_hud_world_ctx.abilities    = &m_server.simulation().abilities();
        m_hud_world_ctx.simulation   = &m_server.simulation();
        m_hud_world_ctx.camera       = &m_renderer.camera();
        m_hud_world_ctx.selection    = &m_selection;
        m_hud_world_ctx.terrain      = m_map.terrain().is_valid() ? &m_map.terrain() : nullptr;
        m_hud_world_ctx.local_player = simulation::Player{m_args.local_slot};
        // Picker callbacks — Hud's data-side queries (cursor_intent,
        // aim_state, inventory-drop-on-terrain) go through these lambdas
        // so `uldum_hud` doesn't pull `input::Picker` symbols at link
        // time. HudRenderer's world overlay reads `pick_target` via
        // the same path.
        m_hud_world_ctx.pick_item       = [this](f32 sx, f32 sy) { return m_picker.pick_item(sx, sy); };
        m_hud_world_ctx.pick_target     = [this](f32 sx, f32 sy) { return m_picker.pick_target(sx, sy); };
        m_hud_world_ctx.pick_unit_local = [this](f32 sx, f32 sy) {
            return m_picker.pick_unit(sx, sy, m_selection.player());
        };
        m_hud_world_ctx.screen_to_world = [this](f32 sx, f32 sy, glm::vec3& wp) {
            return m_picker.screen_to_world(sx, sy, wp);
        };
        m_hud.set_world_context(&m_hud_world_ctx);
    }

    // I18n: load the map's strings pool. AssetManager's mount of the map
    // package gave us `<map>/strings/<locale>/*.json` paths.
    m_i18n.load_map(m_asset, "strings");

    m_session_active = true;
    log::info(TAG, "=== Session started ===");
    return true;
}

void Engine::end_session() {
    if (!m_session_active) return;
    log::info(TAG, "=== Ending session ===");

    // Restore the OS cursor — gameplay sessions hide it (Phase 4b)
    // and draw a HUD cursor instead. After end_session the menu /
    // shell / dev-console want the system pointer back.
    if (m_platform) m_platform->set_cursor_visible(true);

    // Input — drop the preset (RTS / Action) and reset its dependents.
    m_input_preset.reset();
    m_bindings  = input::InputBindings{};
    m_commands  = simulation::CommandSystem{};
    m_selection = simulation::SelectionState{};

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
    m_renderer.set_terrain(nullptr);
    m_renderer.set_fog_grid(nullptr, 0, 0);
    m_renderer.end_session();
    m_hud_renderer.reset_session_images();
    m_world_overlays.reset_session_state();

    // A pending LoadScene request that hadn't fired yet (or a host-side
    // barrier in progress) is meaningless after the session ends.
    m_pending_scene_switch.clear();
    m_pending_scene_switch_finalize.clear();

    // I18n: drop the map's strings pool. Shell pool persists.
    m_i18n.unload_map();

    m_session_active = false;
    m_lobby_active   = false;
    log::info(TAG, "=== Session ended ===");
}

// ── Scripted-camera routing ───────────────────────────────────────────────
//
// Each route function takes a `players_mask` (bitmask of player ids).
// For each bit set: apply locally if it matches the host's own slot,
// otherwise send the matching S_CAMERA_* packet to that peer. Offline
// collapses to "apply locally for whatever bit is set" (typically the
// host's own slot).

void Engine::route_camera_apply_setup(u32 players_mask,
                                    f32 tx, f32 ty, f32 tz, f32 distance,
                                    f32 pitch_rad, f32 yaw_rad, f32 duration) {
    if (players_mask & (1u << m_args.local_slot)) {
        m_camera_controller.apply_setup({tx, ty, tz}, distance, pitch_rad, yaw_rad, duration);
    }
    if (m_args.net_mode != network::Mode::Host) return;
    for (u32 p = 0; p < 32; ++p) {
        if (p == m_args.local_slot) continue;
        if (!(players_mask & (1u << p))) continue;
        m_network.host_send_camera_apply_setup(p, tx, ty, tz, distance,
                                                pitch_rad, yaw_rad, duration);
    }
}

void Engine::route_camera_set_target_position(u32 players_mask,
                                            f32 x, f32 y, f32 z, f32 duration) {
    if (players_mask & (1u << m_args.local_slot)) {
        m_camera_controller.set_target_position(x, y, z, duration);
    }
    if (m_args.net_mode != network::Mode::Host) return;
    for (u32 p = 0; p < 32; ++p) {
        if (p == m_args.local_slot) continue;
        if (!(players_mask & (1u << p))) continue;
        m_network.host_send_camera_set_target_position(p, x, y, z, duration);
    }
}

void Engine::route_camera_set_source_distance(u32 players_mask,
                                            f32 distance, f32 duration) {
    if (players_mask & (1u << m_args.local_slot)) {
        m_camera_controller.set_source_distance(distance, duration);
    }
    if (m_args.net_mode != network::Mode::Host) return;
    for (u32 p = 0; p < 32; ++p) {
        if (p == m_args.local_slot) continue;
        if (!(players_mask & (1u << p))) continue;
        m_network.host_send_camera_set_source_distance(p, distance, duration);
    }
}

void Engine::route_camera_shake(u32 players_mask, f32 intensity, f32 duration) {
    if (players_mask & (1u << m_args.local_slot)) {
        m_camera_controller.shake(intensity, duration);
    }
    if (m_args.net_mode != network::Mode::Host) return;
    for (u32 p = 0; p < 32; ++p) {
        if (p == m_args.local_slot) continue;
        if (!(players_mask & (1u << p))) continue;
        m_network.host_send_camera_shake(p, intensity, duration);
    }
}

void Engine::route_camera_set_target_controller(u32 players_mask, simulation::Unit unit) {
    if (players_mask & (1u << m_args.local_slot)) {
        if (unit.id == UINT32_MAX) m_camera_controller.unlock_unit();
        else                       m_camera_controller.lock_unit(unit);
    }
    if (m_args.net_mode != network::Mode::Host) return;
    for (u32 p = 0; p < 32; ++p) {
        if (p == m_args.local_slot) continue;
        if (!(players_mask & (1u << p))) continue;
        m_network.host_send_camera_set_target_controller(p, unit.id);
    }
}

void Engine::register_script_camera_callbacks() {
    auto& script = m_server.script();
    script.set_camera_apply_setup_fn([this](u32 mask, f32 tx, f32 ty, f32 tz,
                                            f32 d, f32 pr, f32 yr, f32 dur) {
        route_camera_apply_setup(mask, tx, ty, tz, d, pr, yr, dur);
    });
    script.set_camera_set_target_position_fn([this](u32 mask, f32 x, f32 y, f32 z, f32 dur) {
        route_camera_set_target_position(mask, x, y, z, dur);
    });
    script.set_camera_set_source_distance_fn([this](u32 mask, f32 dist, f32 dur) {
        route_camera_set_source_distance(mask, dist, dur);
    });
    script.set_camera_shake_fn([this](u32 mask, f32 i, f32 dur) {
        route_camera_shake(mask, i, dur);
    });
    script.set_camera_set_target_controller_fn([this](u32 mask, simulation::Unit u) {
        route_camera_set_target_controller(mask, u);
    });
}

// Local teardown — runs on host AND clients when entering a new scene.
// Wipes sim entities, swaps terrain, resets the HUD's per-scene tree,
// re-poses the camera, and clears selection / picking handles. The
// host's MP path defers placement instantiation + Lua reset until
// after the client-load barrier (clients haven't reset yet at that
// point), so this helper does NOT load placements or run main(). For
// the host's offline + post-barrier path, scene_switch_run_main()
// finishes the job.
//
// Resets: sim entities & regions, terrain (mesh + pathfinder + spatial
// grid), camera, selection, HUD per-scene state (text tags, drag-cast,
// focus, slot input — but NOT hud.json composites or the image cache),
// world overlays, picker, and the Lua VM. SaveData/LoadData is the
// cross-scene data channel for maps that persist values across scenes.
// Persists: map manifest, type registry, ability registry, tileset,
// fog-of-war state (assumes scenes share terrain dimensions), audio,
// renderer-cached models / effects / textures, network connection,
// local player slot, lobby, hud.json composites.
void Engine::scene_switch_local_teardown(const std::string& scene_name) {
    log::info(TAG, "Scene switch teardown → '{}'", scene_name);

    auto& sim = m_server.simulation();

    // Terrain swap (and entity wipe). On host's offline + post-barrier
    // paths we'd then call load_scene_placements; on host's pre-barrier
    // and on clients we leave the world empty — the host re-spawns
    // entities through S_SPAWN once the barrier clears.
    if (!m_map.switch_scene_terrain_only(scene_name, m_asset, sim)) {
        log::error(TAG, "scene switch teardown failed for '{}'", scene_name);
        return;
    }

    if (m_map.terrain().is_valid()) {
        sim.set_terrain(&m_map.terrain());
    }
    sim.sync_pathing_blockers();
    sim.spatial_grid().update(sim.world());

    if (m_map.terrain().is_valid()) {
        m_renderer.set_terrain(&m_map.terrain());
    }

    // Re-pose camera from the new scene's authored start camera.
    if (!m_map.scene().cameras.empty()) {
        const auto& cam = m_map.scene().cameras.front();
        m_renderer.camera().set_pose(
            {cam.target_x, cam.target_y, cam.target_z},
            cam.distance,
            glm::radians(cam.pitch_deg),
            glm::radians(cam.yaw_deg));
    }
    if (const auto& b = m_map.scene().camera_bounds) {
        m_renderer.camera().set_bounds({b->min_x, b->min_y}, {b->max_x, b->max_y});
    } else {
        m_renderer.camera().clear_bounds();
    }
    // Drop any in-flight pan / shake / lock from the previous scene.
    // Lock targets (entity ids) belong to the old world and won't
    // resolve in the new one anyway; clearing keeps the camera under
    // player input control until the new scene's main() decides to
    // grab it.
    m_camera_controller.reset();

    // Drop the previous scene's persistent VFX. Without this,
    // CreateEffect emitters (e.g. portal-rim glow) keep spawning
    // particles into the new scene's world.
    m_renderer.effect_manager().clear();

    // Stop any music / ambient loops / lingering SFX from the previous
    // scene. The map's audio resource cache is preserved so common
    // sounds don't have to re-register; only the active playback
    // graph is wiped.
    m_audio.reset_scene_state();

    // Local UI / picking state — handles all reference dead unit ids.
    // World overlays' decal textures (SelectionRing, AoE shapes, etc.)
    // are map-level — set once from hud.json's cast_indicator config —
    // so we leave them alone here. Per-frame ring / path commands are
    // already cleared each frame by the renderer; nothing to reset.
    m_selection = simulation::SelectionState{};
    m_selection.set_player(simulation::Player{m_args.local_slot});
    m_hud.reset_scene_state();
    m_picker.init(&m_renderer.camera(), &m_map.terrain(), &active_world(),
                  m_platform->width(), m_platform->height());

    // Client: rebuild the fog mirror against the new terrain
    // dimensions. Host's authoritative fog lives on its own
    // simulation and is rebuilt by sim.set_terrain above.
    if (m_args.net_mode == network::Mode::Client && m_map.terrain().is_valid()) {
        m_network.init_client_fog(m_map.terrain(), m_map, m_server.simulation());
        m_picker.set_vision(&m_network.client_vision(), simulation::Player{m_args.local_slot});
    }
}

// Host-only second half — instantiate the new scene's placements,
// reset the Lua VM, re-wire callbacks, and run main(). Per the design
// contract, Lua state does not survive a scene swap; maps carry data
// across scenes via SaveData / LoadData.
void Engine::scene_switch_run_main(const std::string& scene_name) {
    auto& sim    = m_server.simulation();
    auto& script = m_server.script();

    // Spawn placement entities for the new scene. host_send_spawn_burst
    // (called later by host_finish_scene_switch in MP) iterates the
    // current world, so the entities have to exist before the burst
    // goes out.
    if (!m_map.load_scene_placements(scene_name, m_asset, sim)) {
        log::warn(TAG, "Scene '{}': no placements loaded", scene_name);
    }
    sim.sync_pathing_blockers();
    sim.spatial_grid().update(sim.world());

    // Lua VM full reset.
    script.shutdown();
    if (!script.init(sim, m_map, &m_audio)) {
        log::error(TAG, "ScriptEngine re-init failed for scene '{}'", scene_name);
        return;
    }

    // App-owned wiring (mirrors start_session). Pre-init bindings
    // (input + hud) are set first so the script's main() can use
    // them at scene init time.
    script.set_input(&m_selection, &m_commands);
    script.set_hud(&m_hud);
    m_network.set_script(&script);
    script.set_attach_point_fn([this](u32 entity_id, std::string_view bone) {
        return m_renderer.get_attachment_point(entity_id, bone);
    });
    script.set_unit_update_fn([this](u32 entity_id, const std::vector<u8>& pkt) {
        m_network.host_broadcast_update(entity_id, pkt);
    });
    script.set_sun_direction_fn([this](f32 x, f32 y, f32 z) {
        map::EnvironmentConfig env;
        env.sun_direction = glm::normalize(glm::vec3{x, y, z});
        m_renderer.set_environment(env);
    });
    register_script_camera_callbacks();
    script.set_singleplayer(m_args.net_mode == network::Mode::Offline);
    script.set_end_game_fn([this](u32 winner_id, std::string_view stats) {
        log::info(TAG, "Game ended — winner: player {}", winner_id);
#ifdef ULDUM_SHELL_UI
        f32 elapsed = 0.0f;
        try {
            auto j = nlohmann::json::parse(stats);
            elapsed = j.value("elapsed", 0.0f);
        } catch (...) {}
        m_last_elapsed_seconds = elapsed;
#else
        (void)stats;
#endif
        set_state(AppState::Results);
    });
    script.set_scene_switch_fn([this](std::string_view scene) {
        m_pending_scene_switch.assign(scene);
    });

    // Save path + script paths + bootstrap scripts. Same shape as
    // GameServer::init_game; per-scene data transfer goes through
    // the save channel since the VM itself is fresh.
    {
        std::string map_id = m_map.manifest().id;
        if (map_id.empty()) map_id = m_map.manifest().name;
        std::string save_dir;
#ifdef _WIN32
        char* appdata = nullptr;
        size_t appdata_len = 0;
        if (_dupenv_s(&appdata, &appdata_len, "APPDATA") == 0 && appdata) {
            save_dir = std::string(appdata) + "/saves/" + map_id;
            free(appdata);
        } else {
            save_dir = "saves/" + map_id;
        }
#else
        save_dir = "saves/" + map_id;
#endif
        script.set_save_path(save_dir);
    }
    std::string scene_scripts  = m_map.map_root() + "/scenes/" + scene_name + "/scripts";
    std::string shared_scripts = m_map.map_root() + "/scripts";
    std::string engine_scripts = "engine/scripts";
    script.set_script_paths(scene_scripts, shared_scripts, engine_scripts);
    script.load_script("engine/scripts/constants.lua");

    std::string main_script = scene_scripts + "/main.lua";
    bool loaded = script.load_script(main_script);
    if (!loaded) {
        std::string fallback = m_map.map_root() + "/scripts/main.lua";
        loaded = script.load_script(fallback);
    }
    if (!loaded) {
        log::error(TAG, "Scene '{}' has no main.lua", scene_name);
        return;
    }
    script.call_function("main");
}

// Orchestrator. Offline: teardown + run_main back-to-back. Host MP:
// broadcast the swap and run teardown locally, then defer run_main +
// the entity-spawn burst until every client has acked C_LOAD_DONE.
// Clients never call this directly — they react to S_SCENE_SWITCH
// via the recv_fn registered in start_session.
void Engine::perform_scene_switch(const std::string& scene_name) {
    if (m_args.net_mode == network::Mode::Client) return;

    log::info(TAG, "Scene switch → '{}'", scene_name);

    if (m_args.net_mode == network::Mode::Offline) {
        scene_switch_local_teardown(scene_name);
        scene_switch_run_main(scene_name);
        return;
    }

    // Host MP path. Tell clients first (reliable-ordered ENet
    // guarantees they process this before any later S_SPAWN /
    // S_HUD_* delta), then do the host's own teardown, then mark
    // self-loaded so the barrier closes once peers ack. Phase-2
    // (run_main + spawn burst) is fired from finalize_scene_switch
    // when m_network.all_peers_loaded() goes true.
    m_network.host_broadcast_scene_switch(scene_name);
    scene_switch_local_teardown(scene_name);
    m_pending_scene_switch_finalize = scene_name;
    m_network.mark_self_loaded();
}

// Host MP only. Called from the main loop once every peer has acked
// C_LOAD_DONE for the in-flight scene swap.
void Engine::finalize_scene_switch() {
    std::string scene = std::move(m_pending_scene_switch_finalize);
    m_pending_scene_switch_finalize.clear();

    scene_switch_run_main(scene);

    // Burst spawns to each peer for the new scene's entities + flip
    // network phase back to Playing so ticks resume.
    m_network.host_finish_scene_switch();
    log::info(TAG, "Scene switch '{}' complete — sim resuming", scene);
}

// ── Main loop ─────────────────────────────────────────────────────────────

void Engine::run() {
    log::info(TAG, "Entering main loop (tick rate: {} Hz)", TICK_RATE);

    auto previous_time = std::chrono::high_resolution_clock::now();
    float accumulator = 0.0f;
    u32 tick_counter = 0;

    set_state(AppState::Menu);
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

        // App's per-frame hook. DevApp uses it for ImGui frame setup
        // + dev-console action translation; NullApp / Shell-UI Apps
        // typically leave it empty.
        m_app->on_update(frame_dt);

        switch (m_state) {
        case AppState::Menu:
            if (dev_auto_start) {
                dev_auto_start = false;
                // CLI `--map <path>` path: enter the lobby and let the Lobby
                // case auto-advance to Loading because m_args.auto_start is set.
                if (enter_lobby()) {
                    set_state(AppState::Lobby);
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
                set_state(AppState::Loading);
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
                    set_state(AppState::Loading);
                    log::info(TAG, "Lobby (auto) → Loading (slot {})", m_args.local_slot);
                }
            }
            break;

        case AppState::Loading: {
            // The heavy start_session() is synchronous and freezes the loop.
            // We don't run it here — we request it (m_load_pending) and let
            // the post-present block (end of the loop) run it, so the loading
            // screen the UI drew this frame is what's on screen during the
            // freeze. Request once, on the first Loading frame before the
            // session exists; then just wait for the load + ready handshake.
            if (!m_session_active) {
                m_load_pending = true;
                break;  // nothing else to do until the load actually runs
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
                set_state(AppState::Playing);
                accumulator = 0; tick_counter = 0;
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
                set_state(AppState::Results);
                break;
            }

            // Pending scene switch from Lua (LoadScene). Done before
            // ticking so the new scene's main() runs first, and the
            // tick that follows operates on the new scene's entities.
            if (!is_client && !m_pending_scene_switch.empty()) {
                std::string scene = std::move(m_pending_scene_switch);
                m_pending_scene_switch.clear();
                perform_scene_switch(scene);
            }

            // Host MP: close the scene-switch barrier once every peer
            // has acked C_LOAD_DONE. Runs the new scene's main() and
            // bursts spawns to clients before resuming ticks.
            if (!is_client &&
                m_network.is_scene_switching() &&
                !m_pending_scene_switch_finalize.empty() &&
                m_network.all_peers_loaded()) {
                finalize_scene_switch();
            }

            bool should_tick = !is_client &&
                (m_args.net_mode == network::Mode::Offline || m_network.is_game_started()) &&
                !m_network.is_paused() &&
                !m_network.is_scene_switching() &&
                !m_server.script().is_paused();
            if (should_tick) {
                float game_dt = TICK_DT * m_server.script().game_speed();
                accumulator += frame_dt;
                while (accumulator >= TICK_DT) {
                    auto t0 = std::chrono::steady_clock::now();
                    m_server.tick(game_dt);
                    auto t1 = std::chrono::steady_clock::now();
                    f32 tick_ms = std::chrono::duration<f32, std::milli>(t1 - t0).count();
                    // Warn when a tick consumes half its budget — at
                    // that point we're at the inflection where the
                    // loop is one busy frame away from backlogging.
                    // Scales automatically if TICK_RATE changes.
                    constexpr f32 SLOW_TICK_MS = TICK_DT * 0.5f * 1000.0f;
                    if (tick_ms > SLOW_TICK_MS) {
                        log::warn(TAG, "Slow tick: {:.1f}ms", tick_ms);
                    }
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
                // the corpse is a live unit. Use active_world() so the
                // pass reads from the client's mirror in MP mode —
                // m_server.simulation().world() is empty on the client
                // and would mark every selected unit dead.
                {
                    const auto& world = active_world();
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
                // Route the HUD's primary pointer around the joystick.
                // The platform layer only fires mouse_left_pressed for
                // the very first DOWN (slot 0); secondary fingers come
                // in as POINTER_DOWN and don't trip mouse_left_pressed.
                // So when one finger has the joystick captured, we hand
                // handle_pointer the FIRST other live touch — that
                // makes "joystick + ability button" work simultaneously
                // (otherwise the ability tap is silently dropped). When
                // no second finger is down, the pointer reads as
                // released so the HUD's release-edge detection fires
                // cleanly.
                f32  hud_px    = in.mouse_x;
                f32  hud_py    = in.mouse_y;
                bool hud_pdown = in.mouse_left;
                i32 stick_slot = m_hud.joystick_captured_slot();
                if (stick_slot >= 0) {
                    bool found_other = false;
                    for (u32 i = 0; i < in.touch_count
                                  && i < platform::InputState::MAX_TOUCHES; ++i) {
                        if (static_cast<i32>(i) == stick_slot) continue;
                        hud_px = in.touch_x[i];
                        hud_py = in.touch_y[i];
                        hud_pdown = true;
                        found_other = true;
                        break;
                    }
                    if (!found_other) hud_pdown = false;
                }
                m_hud.handle_pointer(hud_px, hud_py, hud_pdown);
                // Right-click pulse — drives the WC3-style item lift
                // (right-click slot to grab; right-click again to
                // cancel). Fires before the input preset so when the
                // HUD claims the right-click (lift / cancel), the
                // preset's smart-order branch is suppressed on the
                // same frame via `hud_captured`.
                //
                // Mutual exclusion with the input preset's targeting
                // modes (cast / move / attack-move): the HUD's held
                // item and the preset's targeting state are the
                // engine's two "next-click pending" signals. They
                // must never be active simultaneously, so:
                //   • lifting an item cancels any preset targeting
                //   • entering a preset targeting mode (this frame
                //     vs. last) cancels any held item
                // The first edge runs here; the second edge is
                // detected after the preset update below.
                if (in.mouse_right_pressed) {
                    bool was_holding = m_hud.is_holding_item();
                    if (m_hud.handle_right_click(in.mouse_x, in.mouse_y)
                        && !was_holding && m_input_preset) {
                        m_input_preset->cancel_targeting();
                    }
                }
                // ESC cancels both sides — symmetric with the preset's
                // ESC handling so the player has one "bail out" key.
                if (in.key_escape) m_hud.cancel_held_item();
                // Refresh the per-frame camera yaw the HUD's drag-cast
                // uses to align finger displacement with screen axes.
                m_hud_world_ctx.camera_yaw_rad = m_renderer.camera().yaw_rad();
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

                // OS cursor visibility — hide only when the HUD is
                // actually drawing its own. Both cursor textures
                // default to empty (no engine-shipped assets), so
                // the OS cursor stays in every state unless the map
                // opts in via `targeting.cursors.{default,target}`
                // in hud.json. Non-cursor platforms (Android) no-op.
                const auto& cs = m_hud.cast_indicator_style();
                bool targeting = m_hud.aim_state().active;
                bool hud_cursor_active =
                    (targeting && !cs.cursor_target_path.empty()) ||
                    (!targeting && !cs.cursor_default_path.empty());
                m_platform->set_cursor_visible(!hud_cursor_active);

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
                    m_hud.input_captured(),
                    m_hud.is_minimap_dragging(),
                    m_hud.joystick_active(),
                    preset_alpha,
                    jx, jy,
                    &m_hud,
                    [this](simulation::Unit unit, glm::vec3 pos,
                           input::InputContext::TargetPingKind kind) {
                        m_target_ping.unit     = unit;
                        m_target_ping.pos      = pos;
                        m_target_ping.kind     = kind;
                        m_target_ping.age      = 0.0f;
                        m_target_ping.lifespan = 0.45f;
                    },
                };
                bool was_targeting = m_input_preset && m_input_preset->is_targeting();
                m_input_preset->update(ictx, frame_dt);
                // Rising edge of preset targeting → cancel any held
                // item (mutual exclusion, see comment above the
                // right-click block).
                if (m_input_preset && m_input_preset->is_targeting() && !was_targeting) {
                    m_hud.cancel_held_item();
                }
            }

            // Scripted-camera overlay. Runs after the input preset so
            // a script's lock / pan / shake silently overrides player
            // input the same frame. Lookup function returns the unit's
            // current XY for lock-tracking; NaN on stale handle so the
            // controller drops the lock.
            {
                auto& world = active_world();
                m_camera_controller.update(frame_dt,
                    [&world](simulation::Unit unit) -> glm::vec2 {
                        const auto* hi = world.handle_infos.get(unit.id);
                        if (!hi || hi->generation != unit.generation) {
                            f32 nan = std::numeric_limits<f32>::quiet_NaN();
                            return { nan, nan };
                        }
                        const auto* t = world.transforms.get(unit.id);
                        if (!t) {
                            f32 nan = std::numeric_limits<f32>::quiet_NaN();
                            return { nan, nan };
                        }
                        return { t->position.x, t->position.y };
                    });
            }

            {
                auto& cam = m_renderer.camera();
                m_audio.set_listener(cam.position(), cam.forward_dir(), glm::vec3{0, 0, 1});
            }

            if (!is_client) {
                auto& vision = m_server.simulation().vision();
                if (vision.enabled()) {
                    const f32* visual = vision.update_visual(simulation::Player{m_args.local_slot}, frame_dt);
                    m_renderer.set_fog_grid(visual, vision.tiles_x(), vision.tiles_y());
                }
            } else {
                const f32* visual = m_network.update_client_fog(frame_dt);
                if (visual) {
                    m_renderer.set_fog_grid(visual, m_network.client_vision().tiles_x(),
                                            m_network.client_vision().tiles_y());
                }
            }
            break;
        }

        case AppState::Results:
#ifdef ULDUM_SHELL_UI
            // Game build: end the session immediately (tear down sim /
            // audio / network) on first entry. The App's
            // on_state_changed has already loaded results.rml and
            // populated the elapsed-time label.
            if (m_session_active) {
                end_session();
            }
#else
            // Engine-dev build (no Shell): auto-return to Menu. uldum_dev's
            // auto-start will kick the next session off on the following frame.
            log::info(TAG, "Session complete → ending session → Menu");
            end_session();
            set_state(AppState::Menu);
#endif
            break;
        }

        // Audio fades run every frame regardless of AppState so music
        // transitions don't freeze on pause / menu / scene-load. Pass
        // real frame_dt so fade rates are vsync-independent (used to be
        // a hard-coded 1/60 inside the Playing case — wrong by ~2-3x
        // on 144 Hz desktop and 30 Hz mobile, and stuck mid-fade when
        // the player paused).
        m_audio.update(frame_dt);

        // Shell document / button bindings / results data are all the
        // App's responsibility now. Engine just fires
        // App::on_state_changed via set_state and stays out of the way.

        bool have_world = (m_state == AppState::Playing && m_session_active);
        if (have_world) {
            bool is_client = (m_args.net_mode == network::Mode::Client);
            auto& world = active_world();
            f32 alpha = is_client ? 1.0f : (accumulator / TICK_DT);
            auto r0 = std::chrono::steady_clock::now();
            rhi::CommandList cmd = m_rhi.begin_frame();
            if (cmd.is_valid() && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
                m_renderer.upload_fog(cmd);
                m_renderer.draw_shadows(cmd, world, alpha);
                m_rhi.begin_rendering();

                // World overlays — selection rings, ability indicators,
                // future build-placement ghosts. We BUILD the overlay
                // batch up front, then hand its draw call to
                // renderer.draw via the on_after_terrain callback so it
                // composites at the right depth-stencil point (after
                // terrain + water, before unit meshes). That ordering
                // makes alpha-blended units (Wind Walk fade) blend
                // *over* the ring rather than depth-occluding it.
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
                            bool is_local = owner && owner->id == m_args.local_slot;
                            m_world_overlays.add_path(samples, kSelectionStroke,
                                                     is_local ? kColorLocal : kColorOther,
                                                     TexId::SelectionRing);
                            ++emitted;
                        }
                    }

                    // ── Focus target reticle ─────────────────────────
                    // Draws under the hero's current focus target (Action
                    // preset auto/manual lock — see Hud::update_focus).
                    // Sits OUTSIDE the unit's selection radius so it
                    // doesn't fight the selection ring visually.
                    if (terrain) {
                        auto focus = m_hud.focus_target();
                        if (focus.is_valid() && world.validate(focus)) {
                            const auto* tf  = world.transforms.get(focus.id);
                            const auto* sel = world.selectables.get(focus.id);
                            if (tf) {
                                glm::vec3 ip = tf->interp_position(alpha);
                                f32 base_r = (sel && sel->selection_radius > 0.0f)
                                                ? sel->selection_radius : 48.0f;
                                f32 ring_r = base_r * 1.20f;
                                constexpr u32 kSamples = 48;
                                constexpr f32 kStroke  = 5.0f;
                                // Manual lock = solid orange; auto = lighter
                                // amber so the player sees which mode is on.
                                glm::vec4 color = m_hud.focus_is_manual()
                                    ? glm::vec4{1.00f, 0.55f, 0.10f, 0.95f}
                                    : glm::vec4{1.00f, 0.78f, 0.30f, 0.70f};
                                std::vector<glm::vec3> samples;
                                samples.reserve(kSamples + 1);
                                for (u32 i = 0; i <= kSamples; ++i) {
                                    f32 a  = (static_cast<f32>(i % kSamples) / kSamples) * 6.28318530718f;
                                    f32 sx = ip.x + ring_r * std::cos(a);
                                    f32 sy = ip.y + ring_r * std::sin(a);
                                    f32 sz = map::sample_height(*terrain, sx, sy);
                                    samples.push_back({sx, sy, sz});
                                }
                                m_world_overlays.add_path(samples, kStroke, color,
                                                         TexId::SelectionRing);
                            }
                        }
                    }

                    // ── Target ping (WC3-style) ──────────────────────
                    // Brief flashing ring at the target of a right-click
                    // attack / pickup. Scales from 1.4× to 0.9× of the
                    // target's selection radius and fades to zero alpha
                    // over `lifespan`. Color: red for hostile, green for
                    // friendly / pickup. Follows the unit if the handle
                    // is still valid (so a moving target reads cleanly);
                    // falls back to the captured world position if not.
                    if (terrain && m_target_ping.age < m_target_ping.lifespan) {
                        m_target_ping.age += frame_dt;
                        if (m_target_ping.age < m_target_ping.lifespan) {
                            f32 t = m_target_ping.age / m_target_ping.lifespan;
                            // Keep the ring's geometry identical to the
                            // selection circle (same radius, same per-
                            // sample terrain Z) so on a ramp the ribbon
                            // hugs the slope the same way. Bigger rings
                            // span more terrain inclination and the
                            // XY-aligned strip starts visibly clipping
                            // the slope — ditching the scale animation
                            // fixes that. The "ping" feel now comes
                            // from a stroke-width pulse + alpha fade,
                            // both of which leave the centerline
                            // exactly where the selection circle is.
                            f32 a_fade   = 1.0f - t;          // 1.0 → 0.0
                            f32 stroke_w = 8.0f - 4.0f * t;   // 8.0 → 4.0 (matches selection at end)

                            glm::vec3 anchor = m_target_ping.pos;
                            f32 base_r = 48.0f;
                            if (world.validate(m_target_ping.unit)) {
                                if (auto* tf = world.transforms.get(m_target_ping.unit.id)) {
                                    anchor = tf->interp_position(alpha);
                                }
                                if (auto* sl = world.selectables.get(m_target_ping.unit.id)) {
                                    if (sl->selection_radius > 0.0f) base_r = sl->selection_radius;
                                }
                            }
                            // Inset the centerline by half the stroke
                            // (same trick the selection circle uses) so
                            // the outer edge lands at base_r.
                            f32 ring_r = base_r - stroke_w * 0.5f;
                            if (ring_r < stroke_w * 0.5f) ring_r = stroke_w * 0.5f;
                            std::vector<glm::vec3> p_samples;
                            p_samples.reserve(48 + 1);
                            for (u32 i = 0; i <= 48; ++i) {
                                f32 ang = (static_cast<f32>(i % 48) / 48.0f) * 6.28318530718f;
                                f32 sx = anchor.x + ring_r * std::cos(ang);
                                f32 sy = anchor.y + ring_r * std::sin(ang);
                                f32 sz = map::sample_height(*terrain, sx, sy);
                                p_samples.push_back({sx, sy, sz});
                            }
                            // Tint from the intent palette (Phase 4a).
                            // Per-call kind selects which entry; alpha
                            // comes from the lifespan fade. Authors
                            // restyle by editing `targeting.intents`
                            // in hud.json — every intent-tinted visual
                            // updates uniformly.
                            using PingKind = input::InputContext::TargetPingKind;
                            const auto& intents = m_hud.cast_indicator_style().intents;
                            hud::Color base = intents.ally;
                            switch (m_target_ping.kind) {
                                case PingKind::Enemy: base = intents.enemy; break;
                                case PingKind::Ally:  base = intents.ally;  break;
                                case PingKind::Item:  base = intents.item;  break;
                            }
                            auto unpack = [](hud::Color c) -> glm::vec4 {
                                return { ((c.rgba >>  0) & 0xFFu) / 255.0f,
                                         ((c.rgba >>  8) & 0xFFu) / 255.0f,
                                         ((c.rgba >> 16) & 0xFFu) / 255.0f,
                                         ((c.rgba >> 24) & 0xFFu) / 255.0f };
                            };
                            glm::vec4 color = unpack(base);
                            color.a *= a_fade;
                            m_world_overlays.add_path(p_samples, stroke_w,
                                                     color, TexId::SelectionRing);
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
                                case Phase::OutOfRange: return unpack(s.phase_out_of_range);
                                case Phase::Cancelling: return unpack(s.phase_cancel);
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
                                                                  phase_color(s.phase_normal),
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
                                                                  phase_color(s.phase_normal),
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
                                                                  phase_color(s.phase_normal),
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

                        // 4) Ground reticle removed — replaced by the
                        //    HUD-drawn cursor (engine/textures/cursors/
                        //    target.ktx2). The cursor follows the mouse
                        //    and reads as a 2D affordance, which doesn't
                        //    fight terrain inclination on ramps. Only
                        //    AoE preview (handled above) and the snap
                        //    indicator (below) are still 3D ground decals.

                        // 5) Snap-target indicator (mobile only). On
                        //    desktop the cursor's hover position is
                        //    enough — no extra visual. On mobile
                        //    drag-cast we drop a vertical light
                        //    column over the snapped target so the
                        //    player can see the lock at a glance even
                        //    when their finger covers the unit. The
                        //    column is tinted purely by PHASE (normal /
                        //    out-of-range / cancelling) via phase_color,
                        //    the same palette the AoE indicator uses — it
                        //    answers "will this gesture succeed?", not
                        //    "is the target friend or foe" (ability target
                        //    filters already gate what the snap lands on).
                        if (aim.is_unit_target
                            && aim.snapped_id != UINT32_MAX
                            && aim.is_drag_cast) {
                            glm::vec3 base{ aim.snapped_x, aim.snapped_y,
                                            aim.snapped_z + s.snap_target_base_offset };
                            glm::vec3 cam_pos = m_renderer.camera().position();
                            m_world_overlays.add_pillar(base,
                                                       s.snap_target_height,
                                                       s.snap_target_width,
                                                       cam_pos,
                                                       phase_color(s.phase_normal),
                                                       TexId::SnapTarget);
                        }
                    }
                }
                m_renderer.draw(cmd, m_rhi.extent(), world, alpha, [&]() {
                    m_world_overlays.draw(cmd, m_renderer.camera().view_projection());
                });
                // HUD overlay. Pointer is already dispatched earlier in
                // the frame (before input preset update) so its captured
                // state gates gameplay input correctly — here we just
                // build + render the draw list.
                {
                    m_hud.update_text_tags(frame_dt);
                    m_hud.update_display_messages(frame_dt);
                    m_hud.update_focus(frame_dt);
                    m_hud_renderer.begin_frame(m_rhi.extent().width, m_rhi.extent().height);
                    // World-anchored HUD layer first — entity HP bars,
                    // name labels, floating damage numbers. They live
                    // in screen space but conceptually belong to the
                    // 3D world, so they render BENEATH the UI tree
                    // and composites: a unit's HP bar drifting near
                    // the action bar gets occluded by the bar, not
                    // overlaid on top of it.
                    m_hud_renderer.draw_world_overlays(alpha);
                    m_hud_renderer.draw_tree();
                    // Box-select marquee (RTS preset's drag-rectangle).
                    // The preset records mouse coords in physical
                    // pixels (same space the Picker takes for world
                    // hits); HUD draw calls take dp. Convert once here
                    // so the rectangle tracks the cursor at any ui_scale.
                    if (m_input_preset) {
                        auto bs = m_input_preset->box_selection();
                        if (bs.active) {
                            f32 inv = 1.0f / m_hud.ui_scale();
                            m_hud_renderer.draw_marquee(bs.x0 * inv, bs.y0 * inv,
                                                        bs.x1 * inv, bs.y1 * inv);
                        }
                    }
                    m_hud_renderer.render(cmd);
                }
#ifdef ULDUM_SHELL_UI
                if (m_shell) {
                    m_shell->update(frame_dt);
                    m_shell->render(cmd, m_rhi.extent().width, m_rhi.extent().height);
                }
#endif
                m_app->on_render(cmd);
                m_rhi.end_frame();
            }
        }
#ifdef ULDUM_SHELL_UI
        else if (m_shell) {
            // Non-Playing states (Menu, Loading, Results): no 3D scene, but
            // the Shell UI still renders. Open the render pass, draw only
            // Shell. Once a main menu wires up start_session, Menu state is
            // reachable before/between sessions.
            rhi::CommandList cmd = m_rhi.begin_frame();
            if (cmd.is_valid() && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
                m_rhi.begin_rendering();
                m_shell->update(frame_dt);
                m_shell->render(cmd, m_rhi.extent().width, m_rhi.extent().height);
                m_rhi.end_frame();
            }
        }
#endif
        else {
            // Non-Playing states with no Shell UI: open the render pass
            // on a cleared background and let the App draw whatever it
            // wants. Dev builds (DevApp) draw the dev console menu here;
            // NullApp builds end up with just the clear. Either way the
            // begin/end_frame pair stays balanced.
            rhi::CommandList cmd = m_rhi.begin_frame();
            if (cmd.is_valid() && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
                m_rhi.begin_rendering();
                m_app->on_render(cmd);
                m_rhi.end_frame();
            }
        }

        // Deferred session load. Runs AFTER this frame was presented, so the
        // last thing on screen is the loading screen the UI drew (dev console
        // or game shell) — the synchronous load then freezes with "Loading…"
        // visible instead of a stale prior frame. The Loading case below only
        // *requests* the load (sets m_load_pending); it never blocks.
        if (m_load_pending) {
            m_load_pending = false;
            if (!start_session()) {
                log::error(TAG, "Session failed to start → Menu");
                end_session();
                set_state(AppState::Menu);
            } else {
                // Signal "I'm loaded". Host marks self locally; Client sends
                // C_LOAD_DONE. Offline skips the handshake entirely.
                if (m_args.net_mode == network::Mode::Host) {
                    m_network.mark_self_loaded();
                } else if (m_args.net_mode == network::Mode::Client) {
                    m_network.send_load_done();
                }
            }
        }
    }

    if (m_session_active) end_session();
    log::info(TAG, "Exiting main loop");
}

void Engine::shutdown() {
    log::info(TAG, "=== Shutting down engine subsystems ===");
    if (m_session_active) end_session();
    // Persist settings so menu changes survive restart.
    if (!m_settings_path.empty()) m_settings.save(m_settings_path);
    // App's destructor handles whatever subsystems it owns (DevApp
    // tears down its DevConsole here, etc.).
    m_app.reset();
#ifdef ULDUM_SHELL_UI
    if (m_shell) m_shell.reset();
#endif
    m_audio.shutdown();
    m_hud_renderer.shutdown();
    m_hud.shutdown();
    m_world_overlays.shutdown();
    m_renderer.shutdown();
    m_asset.shutdown();
    m_rhi.shutdown();
    m_platform->shutdown();
    log::info(TAG, "=== All shut down ===");
}

} // namespace uldum
