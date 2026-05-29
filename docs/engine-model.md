# Uldum Engine — Game Integration Model

How games are built on Uldum. The relationship between the engine, the
game's contributions, the maps the game ships, and the game's backend.

## The model in one paragraph

The engine is a **library of services** wrapped in a main loop, exposed
as `class Engine`. A product built on Uldum is an `App`: a concrete
class implementing the `App` abstract base, which `Engine` instantiates
once at startup and drives via a small set of lifecycle callbacks. Each
binary statically links one `App` implementation — `uldum_dev` links
`DevApp`, `sample_game` links `SampleGameApp`. The game project
contributes a directory (`game.json`, `shell/`, `maps/`, `branding/`,
optional `src/`, optional `android/`) the engine's build absorbs into
the binary. `Engine` runs the main loop, manages subsystems, drives
session lifecycle; the `App` decides which RML screens to show, what
buttons do, when to start a session. The two share the surface
`Engine` exposes — public verbs and accessors, nothing more.

## The roles

| Role | Type | Lives in | What it does |
|---|---|---|---|
| Engine runtime | `class Engine` | `src/app/engine.{h,cpp}` | Subsystems, main loop, session lifecycle, AppState bookkeeping, exposes public surface |
| App abstract base | `class App` | `src/app/app.h` | Defines the lifecycle callbacks any product implements |
| Dev app | `class DevApp` | `src/app/dev_app.{h,cpp}` | Map picker + dev console; what `uldum_dev` runs |
| Sample app | `class SampleGameApp` | `sample_game/src/` | Menu / options / lobby / loading / results flow; what `UldumSample` runs |
| Null app | `class NullApp` | `src/app/app.h` | No-op; the default if a game project has no `game.cmake` |

`Engine` holds `std::unique_ptr<App> m_app` unconditionally and
dispatches through it with zero build-flavor conditionals — that
single dispatch path is where the architectural unification lives.

## AppState as shared vocabulary

A game-shaped product moves through familiar conceptual phases:
**Menu → Lobby → Loading → Playing → Results**. The dev console walks
them, a real game walks them. The engine encodes them as
`enum class AppState` in `engine.h` and exposes them through
`engine.state()` / `engine.set_state(AppState)` so both halves reason
about the same vocabulary.

Engine auto-drives the transitions it can infer from facts it owns:

- App calls `engine.set_state(AppState::Loading)` to commit out of
  Lobby → engine kicks off `start_session()` internally.
- Simulation's first tick completes → engine sets `AppState::Playing`.
- Session ends → engine sets `AppState::Results`.

App drives the transitions only it knows about — `Menu → Lobby` (user
clicked Play), `Results → Menu` (user clicked Back). It does so by
calling `engine.set_state(...)`. Every transition fires
`App::on_state_changed(prev, next)` exactly once on the App.

`AppState` is intentionally **coarser** than the visible Shell UI
screen. A game might show `login.rml`, `signup.rml`, `main_menu.rml`,
and `settings.rml` all while `engine.state() == AppState::Menu`. The
App's *internal* navigation (which screen is up, which form is open)
is the App's private business — `AppState` is for cross-app
vocabulary, HUD library use, dev-console display, telemetry.

## The cooperation surface

### `class App` — the abstract base

```cpp
namespace uldum {

class Engine;
class SessionResult;
enum class AppState;

class App {
public:
    virtual ~App() = default;

    // Engine is up. App registers UI / handlers.
    virtual void on_init(Engine&) = 0;

    // AppState just transitioned (no-op transitions filtered out).
    virtual void on_state_changed(AppState prev, AppState next) {}

    // Per-frame logic. ImGui-based apps use this.
    virtual void on_update(f32 dt) {}

    // Per-frame render. ImGui-based apps submit draw data here.
    virtual void on_render(rhi::CommandList& cmd) {}

    // Session ended. App reacts (load results screen, post stats).
    virtual void on_session_ended(const SessionResult&) {}
};

} // namespace uldum
```

One pure method, four with empty defaults. Engine dispatches these —
nothing more. Anything the App needs to react to between calls flows
through Shell UI event handlers the App registered in `on_init` /
`on_state_changed`.

### `class Engine` — the public surface

The App holds an `Engine&` reference (passed into `on_init`) and calls
through these methods:

```cpp
class Engine {
public:
    // Subsystems
    rhi::Rhi&                rhi();
    platform::Platform&      platform();
    settings::Store&         settings();
    network::NetworkManager& network();
    shell::Shell&            shell();   // game builds only

    // Launch args (App mutates fields before enter_lobby())
    LaunchArgs&              launch_args();

    // AppState read / write — set_state fires App::on_state_changed
    AppState                 state() const;
    void                     set_state(AppState);

    // Session verbs
    bool                     enter_lobby();
    void                     leave_lobby();
    void                     end_session();
    bool                     is_session_active() const;

    // Per-session data the Results screen needs (game builds only)
    f32                      last_session_elapsed_seconds() const;

    // Process lifecycle
    void                     request_quit();
};
```

With static linking there's no header firewall — the App *can* reach
deeper into engine internals than this if it has to. The above is the
intended surface and the one both `DevApp` and `SampleGameApp` use.

### Shell facade

`engine.shell()` returns a `shell::Shell&` (game builds only). The App
uses it to load RML documents and bind per-id click handlers:

```cpp
class Shell {
public:
    Rml::ElementDocument* load_document(std::string_view rml_path);
    void                  hide_current_document();
    void                  set_element_text(std::string_view id, std::string_view text);

    using ClickClosure = std::function<void()>;
    void                  bind(std::string_view id, ClickClosure on_click);
};
```

`load_document` clears the binding table — each screen owns its own
bindings without worrying about stale closures from prior screens.
`bind` registers a closure invoked when an element with the given id
fires a click event.

## Concrete example — sound toggle

`sample_game/shell/options.rml` has an element `<div id="sound_toggle">`.
The logic that flips the setting and updates the label lives in
[`sample_game/src/sample_game_app.cpp`](../sample_game/src/sample_game_app.cpp):

```cpp
void SampleGameApp::show_options() {
    auto& s = m_engine->shell();
    s.load_document("shell/options.rml");
    auto refresh = [this] {
        bool on = m_engine->settings().get_bool("audio.master_enabled", true);
        m_engine->shell().set_element_text("sound_toggle",
                                            on ? "Sound: ON" : "Sound: OFF");
    };
    refresh();
    s.bind("sound_toggle", [this, refresh] {
        bool on = m_engine->settings().get_bool("audio.master_enabled", true);
        m_engine->settings().set("audio.master_enabled", !on);
        refresh();
    });
    s.bind("back", [this] { show_main_menu(); });
}
```

The engine has no `if (id == "sound_toggle")` anywhere — it just
dispatches the click event to the closure the App bound. The audio
subsystem (which subscribed to `audio.master_enabled` at startup) gets
notified by the settings system regardless of who flipped the value.

## Runtime execution

### Boot — desktop

1. OS launches `uldum_dev.exe` or `UldumSample.exe`.
2. `main()` (in engine source at `src/app/dev_main.cpp` or
   `game_main.cpp`) constructs `uldum::Engine engine`, parses CLI args,
   calls `engine.init(args)`.
3. Inside `Engine::init`:
   - Brings up subsystems (platform, RHI, renderer, audio, asset, …).
   - Mounts asset packs.
   - Constructs Shell (game builds only) so the App can use it.
   - Constructs the App: `m_app = std::make_unique<ULDUM_APP_CLASS>()`.
     The macro is set by CMake per target — `DevApp` for dev builds,
     `NullApp` for game builds with no project sources, the project's
     own class when it ships a `game.cmake`.
   - Calls `m_app->on_init(*this)`.
4. App's `on_init` runs synchronously on the main thread:
   - Stores `m_engine = &engine`.
   - Sets up its UI. SampleGameApp loads `shell/main_menu.rml` and
     binds its buttons; DevApp initializes the ImGui dev console.
5. `main()` calls `engine.run()`. Main loop begins.

### Main loop — per frame, main thread

```
pump platform events           (input, resize, OS messages)
update Shell UI                (RmlUi processes input → fires bound closures)
app->on_update(dt)             (ImGui frame setup, action translation)
tick simulation if active      (only when is_session_active())
render                         (shadow → world → HUD → Shell UI → swap)
app->on_render(cmd)            (App-specific render — ImGui submission, etc.)
```

App code runs in three places per frame: Shell UI event handlers
(during "update Shell UI"), `on_update`, and `on_render`. Between
user input, the App is idle.

### Session lifecycle

1. App user-action (button click) calls `engine.enter_lobby()` then
   `engine.set_state(AppState::Lobby)`.
2. App lobby-action calls `engine.set_state(AppState::Loading)`.
   Engine internally calls `start_session()` — loads the map
   asynchronously, brings up network, initializes simulation.
3. Once simulation's first tick is ready, engine sets
   `AppState::Playing` and `on_state_changed` fires.
4. Subsequent main-loop iterations tick the simulation.
5. Session ends (server S_END, win condition, App called
   `engine.end_session()`). Engine sets `AppState::Results` and calls
   `app->on_session_ended(result)` on the main thread.
6. App loads its results screen via `on_state_changed`.

### Boot — Android

Same flow as desktop except for the entry point. Engine ships
`src/app/android_main.cpp` containing `android_main(android_app*)`,
which constructs `uldum::Engine engine` and calls `engine.init({})`.
From there identical to desktop. The game project writes no `main()`,
no `android_main`, no Activity.

## How the game builds

### Desktop

`src/app/CMakeLists.txt` branches on `ULDUM_GAME_PROJECT_DIR`. When
set, it reads `<project>/game.json` for the binary name and includes
`<project>/game.cmake` if present. The game's `game.cmake` sets
four variables the engine picks up:

```cmake
# sample_game/game.cmake
set(ULDUM_GAME_SOURCES      ${CMAKE_CURRENT_LIST_DIR}/src/sample_game_app.cpp)
set(ULDUM_GAME_INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/src)
set(ULDUM_APP_HEADER        "sample_game_app.h")
set(ULDUM_APP_CLASS         SampleGameApp)
```

`ULDUM_APP_HEADER` and `ULDUM_APP_CLASS` become preprocessor
definitions the engine reads:

```cpp
// engine.cpp
#include ULDUM_APP_HEADER
m_app = std::make_unique<ULDUM_APP_CLASS>();
```

The same macros default to `DevApp` (in `src/app/dev_app.h`) for
`uldum_dev` and to `NullApp` (in `src/app/app.h`) for a game build
without a `game.cmake`. CMake picks exactly one per target.

Invocation:

```sh
# engine-dev build
scripts\build.ps1
./build/bin/uldum_dev

# game build
scripts\build_game.ps1
./dist/UldumSample-debug/UldumSample-debug.exe
```

### Android

The engine's AGP project at `platforms/android/` is parameterized by a
Gradle property `ULDUM_GAME_PROJECT_DIR`. When set, AGP reads
`game.json` for package id and app name, merges
`<game>/android/manifest_fragment.xml` if present, adds
`<game>/android/java/` to the Java source set, and passes the property
through to `externalNativeBuild` so the engine's CMake pulls in the
game's `.cpp` files. Result: a single `.so` containing engine + game
C++.

`ULDUM_DEV_UI` and `ULDUM_SHELL_UI` remain as compile-time flags that
gate the *contents* of each binary (ImGui dev console vs RmlUi shell)
— they're per-build feature toggles, not the architectural seam.

## Maps stay Lua

Maps load at session start. Each map ships with `scripts/*.lua`
running in a session-scope Lua VM. Map Lua never sees the App
instance, never sees auth tokens, never sees backend URLs.

## Server-side

The headless `uldum_worker` is engine-level — one binary that handles
any game's session. It writes its end-of-session result as JSON on
stdout, which the orchestrator (`uldum_server`) reads and POSTs to the
game backend's configured webhook. Worker source lives at
`src/server/worker_main.cpp` and does not currently have an App-style
abstract base.

## Engine targets without a game project

- `uldum_editor` — terrain editor; no App.
- `uldum_pack` — packing tool.
- `uldum_worker` — session worker, game-agnostic.
- `uldum_server` — multiplayer orchestrator.

These never link a game project.
