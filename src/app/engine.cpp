#include "app/engine.h"
#include "simulation/world_view.h"
#include "simulation/placement.h"
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
#include <cmath>
#include <functional>
#include <limits>
#include <thread>

namespace uldum {

Engine::Engine()  = default;
Engine::~Engine() = default;

static constexpr const char* TAG = "App";
static constexpr float TICK_RATE = network::SIM_TICK_RATE;
static constexpr float TICK_DT  = network::SIM_TICK_DT;

simulation::IWorldView& Engine::active_world() {
    // What render / pick / HUD read: always the fog-projected LocalView, never a
    // raw World. The authoritative / mirror world is reached via active_sim().world().
    return m_network.active_view();
}

simulation::Simulation& Engine::active_sim() {
    // Client → the replica sim (GameClient); host/offline → the authoritative one
    // (GameServer). Replaces the old world()/vision() override indirection.
    return is_client() ? m_client.simulation() : m_server.simulation();
}
const simulation::Simulation& Engine::active_sim() const {
    return is_client() ? m_client.simulation() : m_server.simulation();
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
    // Window title: the game's name (compile-def from game.json), "Uldum" for dev.
#ifdef ULDUM_APP_NAME
    platform_config.title  = ULDUM_APP_NAME;
#else
    platform_config.title  = "Uldum";
#endif
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
            if (!world.contains(u.id)) return {0, 0, 0};
            auto* t = world.transform(u.id);
            if (!t) return {0, 0, 0};
            glm::vec3 pos = t->position;
            pos.z += simulation::unit_fly_height(world, u.id);   // fly_height is render-only in the sim; match the visual lift
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
    // The HUD emits cast-reject sounds through this — it can't reach audio
    // directly. play_sfx_2d silently no-ops on an empty / missing path.
    m_hud.set_play_sound_fn([this](std::string_view path) {
        m_audio.play_sfx_2d(path);
    });
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

    // Action-bar hotkey mode — "ability" (mnemonic) or "positional"
    // (grid). Player-level preference, not per-map. HUD consults
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
#ifdef ULDUM_APP_ID
    m_settings_path = m_platform->user_data_dir(ULDUM_APP_ID) + "/settings.json";
#else
    m_settings_path = m_platform->user_data_dir("Uldum") + "/settings.json";
#endif
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

        auto& types     = active_sim().types();
        auto& abilities = active_sim().abilities();
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

    // The sim must exist before init_host/init_client (map content loads later in
    // start_session). Client and host each init their own; the other stays untouched.
    if (is_client()) {
        if (!m_client.init_simulation(m_asset)) {
            log::error(TAG, "GameClient simulation init failed");
            return false;
        }
        // Point the mirror at the client sim's world before init_client connects,
        // so an early S_WELCOME (reads mirror_world() for the placement-count
        // check) can't null-deref. The address is stable for the session.
        m_network.set_mirror(&m_client.simulation().world());
    } else {
        if (!m_server.init_simulation(m_asset)) {
            log::error(TAG, "GameServer simulation init failed");
            return false;
        }
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
    // Only the sim that enter_lobby actually inited (client → m_client, else
    // m_server) gets torn down — the other was never touched, so shutting it
    // down is wasted work + a spurious log.
    if (is_client()) m_client.shutdown();
    else             m_server.shutdown();
    m_lobby_active = false;
}

void Engine::fire_local_ping(const simulation::GameCommand& cmd) {
    if (auto ping = input::derive_target_ping(cmd, active_sim())) {
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

    bool is_client = this->is_client();

    // The mirror was already set in enter_lobby (client only). We build preplaced
    // into active_sim()'s world — on the client that IS the mirror, so it
    // materializes the host's preplaced set at deterministic ids [0, N).
    auto& sim = active_sim();
    sim.world().clear_entities();
    if (!m_map.load_content(m_asset)) {
        log::error(TAG, "Failed to load map content for '{}'", m_args.map_path);
        return false;
    }
    if (!simulation::register_map_types(sim, m_asset, m_map.map_root())) {
        log::error(TAG, "Failed to load map types for '{}'", m_args.map_path);
        return false;
    }
    sim.set_terrain(&m_map.terrain());
    simulation::apply_scene_data(sim, m_map.mutable_scene());

    // Preplaced/dynamic id boundary: after load_content (before Lua main()), next_id
    // == N marks where dynamic ids begin. Host ships N to clients in S_WELCOME.
    m_network.set_placement_count(active_sim().world().entities.next_id());

    // Renderer-side setup moved from enter_lobby. Tileset textures, terrain
    // mesh, environment, and the scene camera pose are only meaningful once
    // the content is loaded.
    m_renderer.set_map_root(m_map.map_root());
    {
        std::string effects_path = m_map.map_root() + "/types/effects.json";
        m_renderer.effect_registry().load_from_json(effects_path);
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
        apply(TexId::SnapTarget,    s.snap_target_texture);
        apply(TexId::CastCurve,     s.arrow_texture);
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
        if (this->is_client()) {
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

    // Build sub-panel: picking a structure slot arms build placement on
    // the preset. Same zero-latency queue path as ability casts; the
    // next world click commits an orders::Build at the snapped point.
    m_hud.set_build_panel_fn([this](const std::string& structure_type_id) {
        if (m_input_preset) m_input_preset->queue_build(structure_type_id);
    });

    // Mobile build drag-cast: a press-drag-release on a structure slot lifts
    // on the map → place there directly. Snap + validity via the shared
    // evaluate_building_placement (same rule as desktop confirm); on an
    // invalid spot, error instead of submitting.
    m_hud.set_build_at_target_fn([this](const std::string& type, f32 x, f32 y) {
        auto place = simulation::evaluate_building_placement(
            active_sim(), type, x, y, /*ignore_id*/ 0, /*owner_id*/ m_args.local_slot);
        if (!place.valid) {
            m_hud.emit_order_error("build", "blocked");
            return;
        }
        simulation::GameCommand cmd;
        cmd.player = m_selection.player();
        cmd.units  = m_selection.selected_units(active_sim().world());
        cmd.order  = simulation::orders::Build{type, place.snapped};
        m_commands.submit(cmd);
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
            cmd.units  = m_selection.selected_units(active_sim().world());
            simulation::orders::Cast c;
            c.ability_id = ability_id;
            if (target_unit_id != UINT32_MAX) {
                const auto& world = active_sim().world();
                if (world.handle_infos.has(target_unit_id)) {
                    c.target_unit = simulation::Unit{target_unit_id};
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
            cmd.units  = m_selection.selected_units(active_sim().world());
            const glm::vec3 wp{target_x, target_y, target_z};
            if (command_id == "move") {
                simulation::orders::Move m;
                if (target_unit_id != UINT32_MAX) {
                    // Snapped to a unit → Follow that unit (Move with
                    // target_unit; range > 0 so the unit stops at a
                    // comfortable trailing distance instead of clipping
                    // into the leader's collider).
                    const auto& world = active_sim().world();
                    if (world.handle_infos.has(target_unit_id)) {
                        m.target_widget = simulation::Unit{target_unit_id};
                        m.target        = wp;   // last-seen seed for the fog/removed seek
                        m.range         = 96.0f;
                    } else {
                        m.target = wp;   // handle invalid mid-frame — fall back to ground
                    }
                } else {
                    m.target = wp;
                }
                cmd.order = std::move(m);
            } else if (command_id == "attack") {
                if (target_unit_id != UINT32_MAX) {
                    // Snapped to a widget → Attack it (point filled from live pos by sim).
                    const auto& world = active_sim().world();
                    if (!world.handle_infos.has(target_unit_id)) return;
                    cmd.order = simulation::orders::Attack{wp, simulation::Unit{target_unit_id}};
                } else {
                    // No snap → A-move to the ground point.
                    cmd.order = simulation::orders::Attack{wp};
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
        const auto& world = active_sim().world();
        if (!world.handle_infos.has(item_id)) return;
        simulation::Item item{item_id};

        // An item is held by exactly one unit — order only that unit to
        // cast, NOT the whole selection. Fanning out to the selection made
        // every co-selected unit take a fresh Cast order (clearing its
        // queue → stopping its move) even though only the holder has the
        // ability, which then fizzled.
        simulation::Unit holder = simulation::get_item_owner(world, item);
        if (!world.contains(holder)) return;

        simulation::GameCommand cmd;
        cmd.player = m_selection.player();
        cmd.units  = { holder };
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
            const auto& world = active_sim().world();
            if (!world.handle_infos.has(item_id)) return;
            simulation::Item item{item_id};

            // Item cast targets its holder only, never the full selection
            // (see set_inventory_use_fn).
            simulation::Unit holder = simulation::get_item_owner(world, item);
            if (!world.contains(holder)) return;

            simulation::GameCommand cmd;
            cmd.player = m_selection.player();
            cmd.units  = { holder };
            simulation::orders::Cast c;
            c.ability_id  = ability_id;
            c.source_item = item;
            c.target_pos  = world_pos;
            if (target_unit_id != UINT32_MAX) {
                if (world.handle_infos.has(target_unit_id)) {
                    c.target_unit = simulation::Unit{target_unit_id};
                }
            }
            cmd.order = std::move(c);
            m_commands.submit(cmd);
        });

    // Inventory drop — held-then-clicked-on-terrain. The
    // sim places the item at the explicit world pos passed in.
    m_hud.set_inventory_drop_fn([this](u32 item_id, i32 /*slot*/, glm::vec3 world_pos) {
        const auto& world = active_sim().world();
        if (!world.handle_infos.has(item_id)) return;
        simulation::Item item{item_id};

        simulation::GameCommand cmd;
        cmd.player = m_selection.player();
        cmd.units  = m_selection.selected_units(active_sim().world());
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
        cmd.units  = m_selection.selected_units(active_sim().world());
        cmd.order  = simulation::orders::SwapInventorySlot{slot_a, slot_b};
        m_commands.submit(cmd);
    });

    m_hud.set_pickup_fn([this](simulation::Unit unit, simulation::Item item) {
        auto& world = active_world();
        if (!world.contains(unit.id) || !world.contains(item.id)) return;

        simulation::GameCommand cmd;
        cmd.player = m_selection.player();
        cmd.units = {unit};
        cmd.order = simulation::orders::PickupItem{item};
        m_commands.submit(cmd);
        fire_local_ping(cmd);
    });

    // Minimap click → jump the camera so its ground-focus point lands
    // at the clicked world coord. Preserves the current pitch/yaw so
    // the player's view angle stays consistent across jumps.
    m_hud.set_minimap_jump_fn([this](f32 wx, f32 wy) {
        // Target-based camera: minimap-click snaps the look-at point
        // to the clicked ground location. Distance / pitch / yaw stay.
        // Suppressed while the preset is in a targeting mode (attack-move,
        // move, ability): there the minimap click is an ORDER at that point,
        // not a camera pan — issuing it should not also yank the view.
        if (m_input_preset && m_input_preset->is_targeting()) return;
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
        m_network.set_hud_replay_source(&m_hud);  // join-replay of persistent HUD

        // HUD → client sync, wired at set_hud time so nodes/tags the map
        // creates in main() (during init_game below) are already syncing. Host
        // mode only; offline host_hud_sync no-ops (no peers). NOT part of
        // wire_to_network (which runs after init_game — too late for the
        // main()-time node/tag creates).
        if (is_host()) {
            m_hud.set_sync_fn([this](const std::vector<u8>& pkt, u32 owner) {
                m_network.host_hud_sync(pkt, owner);
            });
        }

        // Surface the launch mode to Lua before main() runs so scripts
        // can branch on IsSinglePlayer() at scene-init time (e.g.
        // build SP-only HUD nodes, register pause-aware triggers).
        m_server.script().set_singleplayer(is_offline());

        // SetSunDirection (Lua) updates the host's own renderer here;
        // the network broadcast to peers happens inside the binding
        // via m_broadcast_fn (script.cpp).
        m_server.script().set_sun_direction_fn([this](f32 x, f32 y, f32 z) {
            map::EnvironmentConfig env;
            env.sun_direction = glm::vec3{x, y, z};  // normalized + guarded in set_environment
            m_renderer.set_environment(env);
        });
        // pre_main runs the host's script-facing wiring AFTER the Lua VM is
        // inited (so item-sync can chain onto the script's freshly-installed
        // triggers) but BEFORE main() runs — the worker does the same via
        // wire_server. Without this, any callback main() invokes at scene start
        // (notably SetControlledUnit, scripted camera) fired into an empty slot
        // and was silently dropped on the host — the reason a host-client
        // client never got its controlled hero. (set_input/set_hud/set_script/
        // sun_direction/singleplayer are already installed above, before here.)
        auto pre_main = [this](script::ScriptEngine& script) {
            script.set_attach_point_fn([this](u32 entity_id, std::string_view bone) {
                return m_renderer.get_attachment_point(entity_id, bone);
            });
            wire_host_broadcasts();
            script.set_scene_switch_fn([this](std::string_view scene) {
                m_pending_scene_switch.assign(scene);
            });
        };
        if (!m_server.init_game(m_map, &m_audio, pre_main)) {
            log::error(TAG, "GameServer game init failed");
            return false;
        }
    }

    if (is_host()) {
        m_network.set_disconnect_timeout(m_map.manifest().disconnect_timeout);
        m_network.set_pause_on_disconnect(m_map.manifest().pause_on_disconnect);
    } else if (is_client) {
        // Client post-load setup on its own replica sim: alliances + terrain + fog
        // (the non-scripting subset of GameServer::init_game).
        m_client.init_game(m_map);
    }

    // Wire callbacks
    if (!is_client) {
        m_server.simulation().world().on_sound = [this](std::string_view path, glm::vec3 pos) {
            m_audio.play_sfx(path, pos);
        };
        // Build failed (worker walked to the site but couldn't build) → surface
        // the reason on the owning player's HUD error line, same channel the
        // click-time "can't build there" uses. Chains onto any prior handler
        // (script's Lua fan-out) so both fire. Local-player only for now — a
        // remote client's failure would need a network HUD route (deferred MP).
        {
            auto& w = m_server.simulation().world();
            auto prior = std::move(w.on_construction_failed);
            w.on_construction_failed =
                [this, prior = std::move(prior)](simulation::Unit builder,
                                                 std::string_view reason) {
                    if (prior) prior(builder, reason);
                    const auto& world = m_server.simulation().world();
                    const auto* o = world.owners.get(builder.id);
                    if (o && o->id == m_args.local_slot) {
                        m_hud.emit_order_error("build", reason);
                    }
                };
        }
        // Birth-clip gate: a unit plays its birth animation only when
        // spawned in the local player's sight. Mirrors the client's
        // S_SPAWN/S_SHOW distinction. Null on the headless server.
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
        // Renderer-owned hook so the simulation can match projectile
        // death timers to the actual animation clip duration. Not script-facing
        // (sim reads it during ticks), so it stays here — the script-facing
        // wiring (attach_point / wire_host_broadcasts / scene_switch_fn) moved
        // into init_game's pre_main hook so it exists before main() runs.
        m_server.simulation().world().get_clip_duration =
            [this](std::string_view model_path, std::string_view clip_name) -> f32 {
                return m_renderer.clip_duration(model_path, clip_name);
            };
    } else {
        // Client: install the receive callbacks that apply replicated events to
        // App-owned systems — the twin of wire_host_broadcasts.
        wire_client_callbacks();
    }

    // Picking: needs camera + terrain, both ready after map content load.
    m_picker.init(&m_renderer.camera(), &m_map.terrain(),
                  &active_world(),
                  m_platform->width(), m_platform->height());
    // Fog filter — entities in unscouted tiles drop out of pick_*. active_sim()
    // .vision() is the local player's fog (client replica or authoritative).
    m_picker.set_vision(&active_sim().vision(), simulation::Player{m_args.local_slot});

    // Input preset + bindings — preset is map-defined; bindings are the
    // user-customizable layer on top of preset defaults.
    m_input_preset = input::create_preset(m_map.manifest().input_preset);
    m_bindings.load(m_map.manifest().input_bindings_json);
    m_bindings.apply_defaults(input::rts_default_bindings());

    // Renderer reads active_sim() to fog-cull entities (is_fog_hidden /
    // is_in_fog_memory) — the client's replica sim or the authoritative one.
    m_renderer.set_simulation(&active_sim());
    m_renderer.set_local_player(m_args.local_slot);
    m_renderer.set_sound_fn([this](std::string_view path, glm::vec3 position) {
        m_audio.play_sfx(path, position);
    });

    // HUD world-UI context: supplies world / fog / camera / picker / selection
    // / terrain / local player so draw_world_overlays() can iterate entities,
    // project positions, and filter by fog. Built here and held stable for
    // the session; cleared in end_session().
    {
        m_hud_world_ctx = hud::WorldContext{};
        m_hud_world_ctx.world = &active_world();
        m_hud_world_ctx.vision = &active_sim().vision();
        m_hud_world_ctx.types        = &active_sim().types();
        m_hud_world_ctx.abilities    = &active_sim().abilities();
        m_hud_world_ctx.simulation   = &active_sim();
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
            return m_picker.pick_widget(sx, sy, m_selection.player());
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
    // Tear down only the sim that was inited for this session's mode (see
    // leave_lobby) — client → m_client, host/offline → m_server.
    if (is_client()) m_client.shutdown();
    else             m_server.shutdown();

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
    m_target_ping = TargetPing{};   // drop any in-flight right-click ping (starts expired)

    // A pending LoadScene request that hadn't fired yet is meaningless after
    // the session ends. The host-side finalize barrier state lives in
    // GameServer now and is cleared by its shutdown() above.
    m_pending_scene_switch.clear();

    // I18n: drop the map's strings pool. Shell pool persists.
    m_i18n.unload_map();

    m_session_active = false;
    m_lobby_active   = false;
    set_state(AppState::Menu);
    m_app->on_session_ended();
    log::info(TAG, "=== Session ended ===");
}

// ── Scripted-camera routing ───────────────────────────────────────────────
//
// Each route function takes a `players_mask` (bitmask of player ids).
// For each bit set: apply locally if it matches the host's own slot,
// otherwise send the matching S_CAMERA_* packet to that peer. Offline
// collapses to "apply locally for whatever bit is set" (typically the
// host's own slot).

void Engine::wire_host_broadcasts() {
    // Host wiring = the shared server→client sends + the host's OWN local-player
    // apply chained on top. wire_to_network installs the sends (effects, item
    // pickup/drop, ability/cooldown/charge updates, EndGame S_END, unit updates,
    // player-leave); the worker installs exactly the same. Here the host, which
    // is also a player, wraps the mixed ones (effect deliver/destroy, EndGame)
    // to additionally drive its renderer and camera. Called from both
    // start_session and scene_switch_run_main (a scene re-init reinstalls the
    // script handlers, so this must re-run to re-chain onto them).
    m_server.wire_to_network(m_network);
    {
        auto send_event = std::move(m_server.script().anim_event_fn());
        m_server.script().set_anim_event_fn(
            [this, send_event = std::move(send_event)](
                u32 entity_id, const std::vector<u8>& packet) {
                if (send_event) send_event(entity_id, packet);
                auto event = network::parse_anim_event(packet);
                if (!event || event->kind == network::AnimEventKind::AttackStart) return;
                switch (event->kind) {
                case network::AnimEventKind::SetQueue:
                    m_renderer.enqueue_animation(
                        event->entity_id, event->clip, event->looping);
                    break;
                case network::AnimEventKind::QueueClip:
                    m_renderer.enqueue_animation(
                        event->entity_id, event->clip, false, true);
                    break;
                case network::AnimEventKind::Reset:
                    m_renderer.reset_animation(event->entity_id);
                    break;
                case network::AnimEventKind::AttackStart:
                    break;
                }
            });
    }

    // Effect deliver/destroy: wire_to_network already SENT to every player (a
    // no-op for the host's own slot); add the host renderer apply for us.
    {
        auto send_deliver = std::move(m_server.script().effect_deliver_fn());
        m_server.script().set_effect_deliver_fn(
            [this, send_deliver = std::move(send_deliver)](
                u32 player_id, u32 server_id, std::string_view name,
                simulation::Unit entity, glm::vec3 pos, std::string_view attach_point) {
                if (send_deliver) send_deliver(player_id, server_id, name, entity, pos, attach_point);
                if (player_id != m_args.local_slot) return;
                auto& mgr = m_renderer.effect_manager();
                u32 local_id = simulation::is_non_null_handle(entity)
                    ? mgr.create_on_unit(std::string(name), entity, pos, std::string(attach_point))
                    : mgr.create(std::string(name), pos);
                m_effect_id_map[server_id] = local_id;
            });
    }
    {
        auto send_destroy = std::move(m_server.script().effect_destroy_fn());
        m_server.script().set_effect_destroy_fn(
            [this, send_destroy = std::move(send_destroy)](u32 player_id, u32 server_id) {
                if (send_destroy) send_destroy(player_id, server_id);
                if (player_id != m_args.local_slot) return;
                auto it = m_effect_id_map.find(server_id);
                if (it != m_effect_id_map.end()) {
                    m_renderer.effect_manager().destroy(it->second);
                    m_effect_id_map.erase(it);
                }
            });
    }
    // Scripted-camera: wire_to_network installed the send-half (per-player
    // host_send_camera_*, a no-op for the host's own slot). Chain the host's
    // OWN camera-controller apply for the local slot on top — exactly the
    // effect deliver/destroy pattern above. This replaces the old
    // register_script_camera_callbacks/route_camera_* path so host + worker
    // share one send half and there's no per-path wiring-order hazard.
    {
        auto send = std::move(m_server.script().camera_apply_setup_fn());
        m_server.script().set_camera_apply_setup_fn(
            [this, send = std::move(send)](u32 mask, f32 tx, f32 ty, f32 tz,
                                           f32 dist, f32 pr, f32 yr, f32 dur) {
                if (send) send(mask, tx, ty, tz, dist, pr, yr, dur);
                if (mask & (1u << m_args.local_slot))
                    m_camera_controller.apply_setup({tx, ty, tz}, dist, pr, yr, dur);
            });
    }
    {
        auto send = std::move(m_server.script().camera_set_target_position_fn());
        m_server.script().set_camera_set_target_position_fn(
            [this, send = std::move(send)](u32 mask, f32 x, f32 y, f32 z, f32 dur) {
                if (send) send(mask, x, y, z, dur);
                if (mask & (1u << m_args.local_slot))
                    m_camera_controller.set_target_position(x, y, z, dur);
            });
    }
    {
        auto send = std::move(m_server.script().camera_set_source_distance_fn());
        m_server.script().set_camera_set_source_distance_fn(
            [this, send = std::move(send)](u32 mask, f32 dist, f32 dur) {
                if (send) send(mask, dist, dur);
                if (mask & (1u << m_args.local_slot))
                    m_camera_controller.set_source_distance(dist, dur);
            });
    }
    {
        auto send = std::move(m_server.script().camera_shake_fn());
        m_server.script().set_camera_shake_fn(
            [this, send = std::move(send)](u32 mask, f32 i, f32 dur) {
                if (send) send(mask, i, dur);
                if (mask & (1u << m_args.local_slot))
                    m_camera_controller.shake(i, dur);
            });
    }
    {
        auto send = std::move(m_server.script().camera_set_target_controller_fn());
        m_server.script().set_camera_set_target_controller_fn(
            [this, send = std::move(send)](u32 mask, simulation::Unit unit) {
                if (send) send(mask, unit);
                if (mask & (1u << m_args.local_slot)) {
                    if (unit.id == UINT32_MAX) m_camera_controller.unlock_unit();
                    else                       m_camera_controller.lock_unit(unit);
                }
            });
    }
    // SetControlledUnit: wire_to_network stored+sent per owner; chain the host's
    // OWN App-selection apply when the host owns the slot. Mask is 1<<owner, so
    // this fires only when the host IS that owner — no extra ownership check.
    {
        auto send = std::move(m_server.script().set_controlled_unit_fn());
        m_server.script().set_set_controlled_unit_fn(
            [this, send = std::move(send)](u32 mask, simulation::Unit unit) {
                if (send) send(mask, unit);
                if (mask & (1u << m_args.local_slot)) {
                    if (unit.id == UINT32_MAX) m_selection.clear();
                    else                       m_selection.select(unit);
                }
            });
    }
}

// Client-side server→client wiring — the twin of wire_host_broadcasts (see the
// header for why it lives in the App, not GameClient). Called once from
// start_session's client branch; the recv callbacks persist for the connection.
void Engine::wire_client_callbacks() {
    m_network.on_anim_event = [this](const network::AnimEventData& event) {
        switch (event.kind) {
        case network::AnimEventKind::SetQueue:
            m_renderer.enqueue_animation(
                event.entity_id, event.clip, event.looping);
            break;
        case network::AnimEventKind::QueueClip:
            m_renderer.enqueue_animation(
                event.entity_id, event.clip, false, true);
            break;
        case network::AnimEventKind::Reset:
            m_renderer.reset_animation(event.entity_id);
            break;
        case network::AnimEventKind::AttackStart:
            break;
        }
    };
    m_network.on_projectile_hit_animation = [this](u32 entity_id) {
        m_renderer.enqueue_hit(entity_id);
    };
    // Client sizes a dying projectile's death_timer to the real "death" clip (see
    // client_handle_projectile_dying), tearing it down when the clip finishes
    // instead of waiting for an S_DESTROY the host no longer sends for projectiles.
    m_client.simulation().world().get_clip_duration =
        [this](std::string_view model_path, std::string_view clip_name) -> f32 {
            return m_renderer.clip_duration(model_path, clip_name);
        };
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
    // Ambient loops are host-handle-keyed. We map the host's handle to the local
    // AudioEngine handle returned by our own play_ambient call, then look up on stop.
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
        env.sun_direction = glm::vec3{x, y, z};  // normalized + guarded in set_environment
        m_renderer.set_environment(env);
    };
    // CreateEffect: persistent effect with stable server-assigned id. Track
    // server→local handle so a later destroy can find its EffectManager instance.
    m_network.on_effect_create = [this](u32 server_id, std::string_view name,
                                          simulation::Unit entity, glm::vec3 pos,
                                          std::string_view attach_point) {
        auto& mgr = m_renderer.effect_manager();
        u32 local_id = simulation::is_non_null_handle(entity)
            ? mgr.create_on_unit(std::string(name), entity, pos,
                                 std::string(attach_point))
            : mgr.create(std::string(name), pos);
        m_effect_id_map[server_id] = local_id;
    };
    m_network.on_effect_destroy = [this](u32 server_id) {
        auto it = m_effect_id_map.find(server_id);
        if (it != m_effect_id_map.end()) {
            m_renderer.effect_manager().destroy(it->second);
            m_effect_id_map.erase(it);
        }
    };
    // Host-driven scene swap. NetworkManager fires this when an S_SCENE_SWITCH
    // arrives; the App tears down local scene state inline (terrain swap, sim
    // wipe, HUD/picker reset, camera re-pose). NetworkManager handles the
    // C_LOAD_DONE ack right after the callback returns. The host then bursts
    // S_SPAWN/S_HUD_* messages once every peer has acked, so the client renders
    // the new scene as those deltas arrive.
    m_network.set_scene_switch_recv_fn([this](std::string_view scene) {
        scene_switch_local_teardown(std::string(scene));
    });
    // Scripted-camera apply. The host has already chosen this client as the
    // recipient; just hand off to the controller.
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
            m_camera_controller.lock_unit(simulation::Unit{entity_id});
        }
    });
    // Action-preset hero lock from the server's SetControlledUnit. The Action
    // preset reads m_selection (no separate controlled field), so a plain select
    // is the whole apply; camera-follow keys off selection too.
    m_network.set_set_controlled_unit_recv_fn([this](u32 entity_id) {
        if (entity_id == UINT32_MAX) {
            m_selection.clear();
        } else {
            m_selection.select(simulation::Unit{entity_id});
        }
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

    // The active sim: client replica on a client, authoritative on host/offline.
    // On a client this is where the new scene's preplaced entities rebuild.
    auto& sim = active_sim();

    // Entity wipe (allocator reset to 0) + terrain-data swap. On host's
    // offline + post-barrier paths we then apply placements; on host's
    // pre-barrier we leave the world empty. On CLIENTS we build the new
    // scene's preplaced entities locally here — same as the initial load —
    // so ids [0, N) match the host and only dynamic entities cross the wire.
    sim.world().clear_entities();
    if (!m_map.switch_scene_terrain_only(scene_name, m_asset)) {
        log::error(TAG, "scene switch teardown failed for '{}'", scene_name);
        return;
    }

    // Wipe the local player's view-world in lockstep with the authoritative
    // entity wipe above (H4) — its ids/discovered must not leak across scenes.
    m_network.reset_local_view();
    if (m_map.terrain().is_valid()) {
        sim.set_terrain(&m_map.terrain());
    }
    if (is_session_active() && is_client()) {
        simulation::apply_scene_data(sim, m_map.mutable_scene());
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

    // Drop the previous scene's per-entity animation instances.
    m_renderer.clear_animations();

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

    // Client: rebuild alliances + terrain + fog against the new scene on the
    // client's own replica sim (its vision was wiped with the entities). Host's
    // authoritative fog was rebuilt by sim.set_terrain above.
    if (is_client() && m_map.terrain().is_valid()) {
        m_client.reinit_after_scene_switch(m_map);
        m_picker.set_vision(&m_client.simulation().vision(),
                            simulation::Player{m_args.local_slot});
    }
}

network::GameServer::PreMainHook Engine::scene_pre_main() {
    return [this](script::ScriptEngine& script) {
        // App-owned wiring (mirrors start_session). Pre-init bindings (input +
        // hud) first so the script's main() can use them at scene init time.
        script.set_input(&m_selection, &m_commands);
        script.set_hud(&m_hud);
        m_network.set_script(&script);
        m_network.set_hud_replay_source(&m_hud);
        script.set_attach_point_fn([this](u32 entity_id, std::string_view bone) {
            return m_renderer.get_attachment_point(entity_id, bone);
        });
        script.set_sun_direction_fn([this](f32 x, f32 y, f32 z) {
            map::EnvironmentConfig env;
            env.sun_direction = glm::normalize(glm::vec3{x, y, z});
            m_renderer.set_environment(env);
        });
        script.set_singleplayer(is_offline());
        // Server→client sends + host local-apply (effects, item sync, ability /
        // cooldown / charge, EndGame, unit updates, player-leave). Re-chains onto
        // the freshly-inited script handlers — WITHOUT this, effects/items broke
        // after a LoadScene even on the host.
        wire_host_broadcasts();
        script.set_scene_switch_fn([this](std::string_view scene) {
            m_pending_scene_switch.assign(scene);
        });
    };
}

// Host second half — instantiate the new scene's placements, reset the Lua VM,
// re-wire callbacks, and run main(). The server-authoritative work (terrain +
// placements + VM reset + run main) now lives in GameServer::switch_scene,
// shared with the headless worker; this wrapper supplies the host's App/render
// re-wiring via the pre_main hook (the VM reset clears every callback, so it
// must be re-installed before main() runs).
void Engine::scene_switch_run_main(const std::string& scene_name) {
    u32 boundary = m_server.switch_scene(m_map, m_asset, scene_name, scene_pre_main());
    if (boundary == UINT32_MAX) {
        log::error(TAG, "scene_switch_run_main: switch_scene failed for '{}'", scene_name);
        return;
    }
    // New scene's preplaced/dynamic id boundary (allocator was reset to 0 by the
    // scene wipe). Same role as the initial-load capture in start_session.
    m_network.set_placement_count(boundary);
}

// Orchestrator. Offline: teardown + run_main back-to-back. Host MP:
// GameServer::begin_scene_switch broadcasts the swap + marks self loaded, with
// the host's local teardown injected; phase 2 (run_main + spawn burst) is
// deferred to GameServer::try_finish_scene_switch when all peers ack C_LOAD_DONE.
// Clients never call this directly — they react to S_SCENE_SWITCH
// via the recv_fn registered in start_session.
void Engine::perform_scene_switch(const std::string& scene_name) {
    if (is_client()) return;

    log::info(TAG, "Scene switch → '{}'", scene_name);

    if (is_offline()) {
        scene_switch_local_teardown(scene_name);
        scene_switch_run_main(scene_name);
        return;
    }

    m_server.begin_scene_switch(m_network, scene_name,
        [this](const std::string& scene) { scene_switch_local_teardown(scene); });
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
            // The new surface is fresh state: re-push everything bound to it
            // that the RHI doesn't itself restore.
            m_rhi.set_vsync(m_settings.get_bool("graphics.vsync", true));
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
            if (is_client() &&
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
                if (skip_lobby_ui && is_offline()) {
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
            if (is_offline()) {
                ready_to_play = true;
            } else if (is_host()) {
                if (m_network.try_host_finish_start()) {  // sends S_WELCOME + S_SPAWN + S_START
                    ready_to_play = true;
                }
            } else {  // Client
                if (m_network.phase() == network::Phase::Playing) {
                    ready_to_play = true;
                }
            }

            if (ready_to_play) {
                // Compute initial vision + build the local view ONCE
                // before the first Playing frame, so the fog grid and the
                // LocalView projection are already populated when frame 1's
                // selection prune (and the renderer/HUD) read them. Without
                // this, project_local_view first runs inside the tick loop
                // (host/offline) or per-frame after fog (client) — i.e. AFTER
                // the frame-1 prune — so the view is empty for one frame and an
                // owned unit main() selected gets pruned out. Priming here
                // covers both paths: host/offline stamps auth vision, the
                // client recomputes its mirror fog; both then project. Both read
                // active_sim() — client replica or authoritative — uniformly.
                auto& sim = active_sim();
                sim.vision().update(sim.world(), sim);
                m_network.project_local_view(sim,
                                             simulation::Player{m_args.local_slot},
                                             m_network.placement_count());

                set_state(AppState::Playing);
                accumulator = 0; tick_counter = 0;
                log::info(TAG, "Loading complete → Playing");
            }
            break;
        }

        case AppState::Playing: {
            bool is_client = this->is_client();

            m_network.update(frame_dt);

            if (is_client && !m_network.is_connected() &&
                m_network.local_player().is_valid()) {
                set_state(AppState::Results);
                break;
            }

            if (is_client && m_network.is_connected()) {
                auto lp = m_network.local_player();
                if (lp.is_valid() && lp.id != m_selection.player().id) {
                    m_selection.set_player(lp);
                    log::info(TAG, "Local player set to {}", lp.id);
                }
            }

            if (m_network.is_game_ended()) {
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
            // bursts spawns to clients before resuming ticks. Self-guards
            // on the barrier state inside GameServer.
            if (!is_client && m_server.scene_switch_pending()) {
                m_server.try_finish_scene_switch(m_network, m_map, m_asset,
                                                 scene_pre_main());
            }

            bool should_tick = !is_client &&
                (is_offline() || m_network.is_game_started()) &&
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
                    if (is_host())
                        m_network.host_broadcast_tick(tick_counter);
                    // Project the authoritative world into the local player's
                    // view-world (host + offline) — the in-process sibling of
                    // the client's network-fed mirror. Once per tick (H1) so
                    // render interpolation gets one clean tick delta. Built in
                    // shadow this stage; nothing reads it until Stage 2.
                    m_network.project_local_view(m_server.simulation(),
                                                 simulation::Player{m_args.local_slot},
                                                 m_network.placement_count());
                    accumulator -= TICK_DT;
                }
            }

            // Client twin of the host tick above: derive local-only timers + fog,
            // then project the LocalView — the same pairing the host does per tick,
            // but per FRAME (client_tick is linear/idempotent, so it needs no fixed
            // step, and per-frame membership keeps the fog fade + view flicker-free).
            // After m_network.update()'s interpolation (above), so projection reads
            // this frame's positions.
            if (is_client && m_network.is_game_started() &&
                !m_network.is_paused() && !m_network.is_scene_switching()) {
                m_client.tick(frame_dt);
                m_network.project_local_view(m_client.simulation(),
                                             simulation::Player{m_args.local_slot},
                                             m_network.placement_count());
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
                //
                // Also drop anything that left live vision: you can view a
                // foreign unit/widget while it's in sight, but the moment it
                // slips into fog the selection clears. Own units are
                // never fogged from their owner, so they stay selected.
                {
                    const auto& world = active_world();
                    const simulation::Player me = m_selection.player();
                    const auto& cur = m_selection.selected();
                    // Selection can't mix players. The FIRST selected widget sets
                    // the lead owner; any widget whose owner differs is dropped.
                    // This is what reacts to SetUnitOwner: a selected unit that
                    // changes hands no longer matches the lead → leaves the
                    // selection (and the HUD action bar refreshes for free).
                    u32 lead_owner = UINT32_MAX;
                    if (!cur.empty()) {
                        if (const auto* lo = world.owner(cur.front().id)) lead_owner = lo->id;
                    }
                    auto keep = [&](simulation::Widget u) {
                        // View-side "alive": present in the view-world with
                        // positive projected health. (is_alive() is the
                        // auth-world predicate; here we read active_world().)
                        if (!world.contains(u.id)) return false;
                        const auto* h = world.health(u.id);
                        if (!h || h->current <= 0) return false;
                        // Owner-homogeneity: drop if this widget's owner differs
                        // from the first selected widget's (owner changed mid-select).
                        const auto* own = world.owner(u.id);
                        u32 own_id = own ? own->id : UINT32_MAX;
                        if (own_id != lead_owner) return false;
                        if (own && own->id == me.id) return true;   // own troops: never fogged
                        // Foreign widget: keep selected only while LIVE-visible.
                        // A snapshot (fog memory) or hidden unit drops from the
                        // selection the moment it slips into fog. Pure view
                        // membership — no vision query (that's is_unit_visible_to
                        // on the AUTH World, a different, gameplay question).
                        return world.fog_mode(u.id) == simulation::FogVis::Live;
                    };
                    // Prune dropped entities, preserving shape: first survivor
                    // decides own-units group vs. lone view widget.
                    std::vector<simulation::Widget> live;
                    live.reserve(cur.size());
                    for (auto w : cur) if (keep(w)) live.push_back(w);
                    if (live.size() != cur.size()) {
                        if (live.empty()) {
                            m_selection.clear();
                        } else if (const auto* lo = world.owner(live.front().id);
                                   lo && lo->id == me.id) {
                            std::vector<simulation::Unit> units;
                            units.reserve(live.size());
                            for (auto w : live) units.push_back(simulation::Unit{w.id});
                            m_selection.select_multiple(active_sim().world(), std::move(units));
                        } else {
                            m_selection.select(live.front());   // lone view widget
                        }
                    }
                }

                m_hud.pickup_bar_update();

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
                // Right-click pulse — drives the item lift
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
                // Cursor feed for the world overlay's hovered-unit lookup
                // (VisibilityPolicy::Hovered). Physical px — same space the
                // picker was initialised in.
                m_hud_world_ctx.cursor_x = in.mouse_x;
                m_hud_world_ctx.cursor_y = in.mouse_y;
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
                bool  is_client_now = this->is_client();
                f32   preset_alpha  = is_client_now ? 1.0f : (accumulator / TICK_DT);

                f32 jx = 0.0f, jy = 0.0f;
                m_hud.joystick_vector(jx, jy);

                // Minimap-as-world-proxy: push the minimap panel rect (dp →
                // physical px) to the picker so a click there maps to a ground
                // point, and flag whether the pointer is over it this frame so
                // the preset lets ground orders / ground-target abilities
                // through minimap capture. Desktop mouse only.
                hud::Rect mm_dp = m_hud.minimap_screen_rect();
                f32 ui = m_hud.ui_scale();
                m_picker.set_minimap_rect(mm_dp.x * ui, mm_dp.y * ui,
                                          mm_dp.w * ui, mm_dp.h * ui);
                bool minimap_hovered =
                    m_picker.over_minimap(in.mouse_x, in.mouse_y);

                input::InputContext ictx{
                    m_platform->input(), m_selection, m_commands, m_picker,
                    m_renderer.camera(), m_bindings, active_sim(), active_world(),
                    m_platform->width(), m_platform->height(),
                    m_hud.input_captured(),
                    minimap_hovered,
                    m_hud.is_minimap_dragging(),
                    m_hud.joystick_active(),
                    preset_alpha,
                    jx, jy,
                    &m_hud,
                    [this](simulation::Widget unit, glm::vec3 pos,
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

            // Mobile drag-cast edge-pan. The drag target (aim.drag_*) is
            // anchored to caster + finger-displacement, not a screen raycast,
            // so it can sit far off-screen while the thumb stays by the ability
            // button. Pan the camera to keep it visible while aiming. Stateless:
            // on release this simply stops — RTS leaves the camera where it
            // ended (free camera); the Action preset's hero-follow reclaims
            // it the next frame (its handle_camera_follow suspends while a
            // drag-cast is aiming, so it doesn't fight this pan). A script
            // camera lock always wins, so skip while locked.
            {
                auto aim = m_hud.aim_state();
                if (aim.active && aim.is_drag_cast && !m_camera_controller.is_locked()) {
                    auto& cam = m_renderer.camera();
                    // Project the drag point to NDC; pan only once it leaves the
                    // inner safe box (|ndc| > SAFE). Exponential ease toward the
                    // point self-limits: as the target re-enters the safe box the
                    // pan stops, settling it near the edge rather than yanking it
                    // to center.
                    glm::vec3 drag{aim.drag_x, aim.drag_y, aim.drag_z};
                    glm::vec4 clip = cam.view_projection() * glm::vec4(drag, 1.0f);
                    if (clip.w > 0.0f) {
                        f32 ndc_x = clip.x / clip.w;
                        f32 ndc_y = clip.y / clip.w;
                        constexpr f32 SAFE = 0.70f;   // inner 70% is "on screen"
                        if (std::abs(ndc_x) > SAFE || std::abs(ndc_y) > SAFE) {
                            f32 gain = 1.0f - std::exp(-8.0f * frame_dt);  // ~0.12s constant
                            glm::vec3 t = cam.target();
                            cam.set_target_xy(t.x + (drag.x - t.x) * gain,
                                              t.y + (drag.y - t.y) * gain);
                        }
                    }
                }
            }

            // Scripted-camera overlay. Runs after the input preset so
            // a script's lock / pan / shake silently overrides player
            // input the same frame. Lookup function returns the unit's
            // current XY for lock-tracking; NaN on stale handle so the
            // controller drops the lock.
            {
                // AUTHORITATIVE world here (H5), not active_world(): a scripted
                // camera lock onto a unit currently in fog must keep tracking —
                // the view-world wouldn't contain it, so the lookup would return
                // NaN and the controller would drop the lock. Camera targeting
                // is a sim-authoring concern, not a fog-of-war display concern.
                auto& world = active_sim().world();
                m_camera_controller.update(frame_dt,
                    [&world](simulation::Unit unit) -> glm::vec2 {
                        if (!world.contains(unit)) {
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
                // Fog fade grid the renderer draws — the visual half of fog, per
                // frame like the host's. (Membership + projection ran in the client
                // tick block above; this is only the smooth fade.)
                auto& vision = m_client.simulation().vision();
                if (vision.enabled()) {
                    const f32* visual = vision.update_visual(
                        simulation::Player{m_args.local_slot}, frame_dt);
                    m_renderer.set_fog_grid(visual, vision.tiles_x(), vision.tiles_y());
                }
            }
            break;
        }

        case AppState::Results:
            end_session();
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
            bool is_client = this->is_client();
            auto& world = active_world();
            f32 alpha = is_client ? 1.0f : (accumulator / TICK_DT);
            auto r0 = std::chrono::steady_clock::now();
            rhi::CommandList cmd = m_rhi.begin_frame();
            if (cmd.is_valid() && m_rhi.extent().width > 0 && m_rhi.extent().height > 0) {
                m_renderer.upload_fog(cmd);
                m_renderer.draw_shadows(cmd, world, alpha);
                m_rhi.begin_rendering();

                // Build-placement ghost params, filled by the footprint block
                // below and consumed by draw_ghost_model AFTER m_renderer.draw
                // (so the translucent ghost composites over opaque units).
                std::string ghost_model;
                glm::vec3   ghost_pos{0.0f};
                f32         ghost_scale = 1.0f;
                bool        ghost_valid = false;

                // World overlays — selection rings, ability indicators,
                // future build-placement ghosts. We BUILD the overlay
                // batch up front, then hand its draw call to
                // renderer.draw via the on_after_entities callback so it
                // composites AFTER the unit meshes and depth-tests against
                // them. That gives correct 3D occlusion: a unit in front of
                // an air unit's ring hides it, one behind does not. (Trade:
                // a Wind-Walk-faded body no longer shows the ring through
                // its silhouette — acceptable vs. the prior glitch where a
                // nearer ground unit painted over a flyer's ring.)
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
                        constexpr glm::vec4 kColorNeutral{ 0.95f, 0.90f, 0.35f, 0.8f };  // ownerless: crate / tree / item
                        constexpr u32 kMaxSelectionRings = 48;

                        u32 emitted = 0;
                        std::vector<glm::vec3> samples;
                        samples.reserve(kSelectionSamples + 1);
                        for (auto unit : m_selection.selected()) {
                            if (emitted >= kMaxSelectionRings) break;
                            const auto* tf  = world.transform(unit.id);
                            const auto* sel = world.selectable(unit.id);
                            if (!tf || !sel) continue;

                            glm::vec3 ip = tf->interp_position(alpha);
                            // Air units: lift the ring to the visual hull height
                            // (same fly_height the mesh renderer adds) so the
                            // ring sits under the model, not on the ground.
                            f32 fly = simulation::unit_fly_height(world, unit.id);
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
                                // Ground ring follows terrain; air ring is a flat
                                // disc at hull height (fly>0 → ignore terrain bumps).
                                f32 sz = (fly > 0.0f) ? ip.z + fly
                                                      : map::sample_height(*terrain, sx, sy);
                                samples.push_back({sx, sy, sz});
                            }
                            const auto* owner = world.owner(unit.id);
                            // 3-way: own (green) / other-player (red) / ownerless
                            // crate·tree·item (neutral yellow). Ownerless must NOT
                            // fall to "other" — a neutral crate isn't an enemy.
                            const glm::vec4& ring_color =
                                !owner                              ? kColorNeutral
                                : owner->id == m_args.local_slot    ? kColorLocal
                                                                    : kColorOther;
                            m_world_overlays.add_path(samples, kSelectionStroke,
                                                     ring_color,
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
                        if (simulation::is_non_null_handle(focus) && world.contains(focus.id)) {
                            const auto* tf  = world.transform(focus.id);
                            const auto* sel = world.selectable(focus.id);
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

                    // ── Target ping ──────────────────────────────────
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
                            f32 ping_fly = 0.0f;
                            if (world.contains(m_target_ping.unit.id)) {
                                if (auto* tf = world.transform(m_target_ping.unit.id)) {
                                    anchor = tf->interp_position(alpha);
                                }
                                if (auto* sl = world.selectable(m_target_ping.unit.id)) {
                                    if (sl->selection_radius > 0.0f) base_r = sl->selection_radius;
                                }
                                // Air target: lift the ping to hull height so
                                // it sits under the flying model, not on the
                                // ground below it (matches the selection ring).
                                ping_fly = simulation::unit_fly_height(world, m_target_ping.unit.id);
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
                                // Air ping is a flat disc at hull height; ground
                                // ping follows terrain (fly==0 → sample height).
                                f32 sz = (ping_fly > 0.0f) ? anchor.z + ping_fly
                                                           : map::sample_height(*terrain, sx, sy);
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
                            hud::Color base = intents.neutral;
                            switch (m_target_ping.kind) {
                                case PingKind::Enemy:   base = intents.enemy;   break;
                                case PingKind::Ally:    base = intents.ally;    break;
                                case PingKind::Neutral: base = intents.neutral; break;
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

                    // ── Build placement footprint ─────────────────────
                    // Per-tile footprint tint under the armed/dragged build
                    // point (green = legal, red = blocked). The ghost model is
                    // a separate deferred draw.
                    if (terrain) {
                        // Two sources feed the footprint/ghost:
                        //  • mobile build drag — a structure slot dragged onto
                        //    the map; type + world point come from the HUD drag.
                        //  • desktop armed placement — TargetingMode::Build; type
                        //    from the preset, point from the cursor.
                        std::string btype;
                        glm::vec3 wp{0.0f};
                        bool have = false;
                        f32 dwx = 0.0f, dwy = 0.0f;
                        if (m_hud.active_build_drag(btype, dwx, dwy)) {
                            wp = { dwx, dwy, 0.0f };
                            have = true;
                        } else if (m_input_preset) {
                            std::string_view bt = m_input_preset->build_type_id();
                            if (!bt.empty()) {
                                const auto& in = m_platform->input();
                                if (m_picker.screen_to_world(in.mouse_x, in.mouse_y, wp)) {
                                    btype.assign(bt);
                                    have = true;
                                }
                            }
                        }
                        if (have) {
                            auto place = simulation::evaluate_building_placement(
                                active_sim(), btype, wp.x, wp.y,
                                /*ignore_id*/ 0, /*owner_id*/ m_args.local_slot);
                            const glm::vec4 ok_color { 0.30f, 0.90f, 0.40f, 0.45f };
                            const glm::vec4 bad_color{ 0.92f, 0.28f, 0.28f, 0.45f };
                            f32 ts = terrain->tile_size;
                            if (place.fw > 0 && place.fh > 0) {
                                f32 x0 = place.snapped.x - 0.5f * static_cast<f32>(place.fw) * ts;
                                f32 y0 = place.snapped.y - 0.5f * static_cast<f32>(place.fh) * ts;
                                // Each corner drapes to its own terrain height so the
                                // flush edge-to-edge quad fits the slope.
                                auto corner = [&](f32 x, f32 y) {
                                    return glm::vec3{ x, y, map::sample_height(*terrain, x, y) };
                                };
                                for (u32 j = 0; j < place.fh; ++j) {
                                    for (u32 i = 0; i < place.fw; ++i) {
                                        f32 xl = x0 + static_cast<f32>(i) * ts;
                                        f32 xr = x0 + static_cast<f32>(i + 1) * ts;
                                        f32 yb = y0 + static_cast<f32>(j) * ts;
                                        f32 yt = y0 + static_cast<f32>(j + 1) * ts;
                                        usize idx = static_cast<usize>(j) * place.fw + i;
                                        bool tok = idx < place.tile_ok.size() && place.tile_ok[idx];
                                        m_world_overlays.add_quad_corners(
                                            corner(xl, yb), corner(xr, yb),
                                            corner(xr, yt), corner(xl, yt),
                                            tok ? ok_color : bad_color, TexId::Placement);
                                    }
                                }
                            } else {
                                m_world_overlays.add_quad(place.snapped, ts * 0.5f,
                                                          place.valid ? ok_color : bad_color,
                                                          TexId::Placement);
                            }
                            // Stash the mesh ghost for the post-draw pass.
                            if (const auto* def = active_sim().types().get_unit_type(btype);
                                def && !def->model_path.empty()) {
                                ghost_model = def->model_path;
                                ghost_pos   = place.snapped;
                                ghost_scale = def->model_scale;
                                ghost_valid = place.valid;
                            }
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
                                    // Strip from caster in the caster→drag
                                    // direction, width = aim.area_width. Reach =
                                    // aim.area_length (the effect's travel/length,
                                    // decoupled from cast range); falls back to
                                    // aim.range when unset. Two-sample path is
                                    // enough since the line is straight in XY.
                                    f32 reach = aim.area_length > 0 ? aim.area_length : aim.range;
                                    f32 dx = drag.x - caster.x;
                                    f32 dy = drag.y - caster.y;
                                    f32 d  = std::sqrt(dx*dx + dy*dy);
                                    if (d > 1e-3f && reach > 0) {
                                        f32 inv = 1.0f / d;
                                        glm::vec3 end{
                                            caster.x + dx * inv * reach,
                                            caster.y + dy * inv * reach,
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
                                    // Wedge from caster toward drag, half-angle
                                    // from area_angle (deg→rad). Radius = reach =
                                    // aim.area_length (else aim.range).
                                    f32 reach = aim.area_length > 0 ? aim.area_length : aim.range;
                                    f32 dx = drag.x - caster.x;
                                    f32 dy = drag.y - caster.y;
                                    f32 d  = std::sqrt(dx*dx + dy*dy);
                                    if (d > 1e-3f && reach > 0 && aim.area_angle > 0) {
                                        glm::vec3 dir{ dx / d, dy / d, 0.0f };
                                        f32 half_angle_rad = aim.area_angle * 0.5f
                                                             * 3.14159265358979323846f / 180.0f;
                                        m_world_overlays.add_cone(caster, dir, half_angle_rad,
                                                                  reach,
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
                            // Start the arrow at the caster's feet: ground for
                            // land units, hull height for air (caster_z is
                            // ground Z; lift by fly_height). Only the curve
                            // lifts — the range ring + AoE decals above stay
                            // on the ground where the spell resolves.
                            glm::vec3 curve_start = caster;
                            curve_start.z += aim.caster_fly_height;
                            constexpr u32 kCurveSamples = 24;
                            std::vector<glm::vec3> curve;
                            curve.reserve(kCurveSamples + 1);
                            for (u32 i = 0; i <= kCurveSamples; ++i) {
                                f32 t = static_cast<f32>(i) / static_cast<f32>(kCurveSamples);
                                glm::vec3 p = curve_start * (1.0f - t) + drag * t;
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
                // Translucent building ghost over the placed footprint — after
                // draw() so it composites atop opaque units. Green=valid tint,
                // red=blocked. Render-only (no sim entity), so it's MP-safe.
                if (!ghost_model.empty()) {
                    glm::vec4 tint = ghost_valid ? glm::vec4{0.55f, 1.0f, 0.6f, 1.0f}
                                                 : glm::vec4{1.0f, 0.5f, 0.5f, 1.0f};
                    m_renderer.draw_ghost_model(cmd, ghost_model,
                                                m_renderer.camera().view_projection(),
                                                ghost_pos, 0.0f, ghost_scale, tint, 0.5f);
                }
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
                if (is_host()) {
                    m_network.mark_self_loaded();
                } else if (is_client()) {
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
