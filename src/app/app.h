#pragma once

// App — the seam between Engine (engine main loop / subsystems) and the
// specific application built on top. Every Uldum-based product is
// `Engine + one App implementation`:
//
//   • uldum_dev    =  Engine + DevApp          (map picker + dev console)
//   • a real game  =  Engine + SampleGameApp   (real menu / lobby / login UI)
//
// Engine holds `std::unique_ptr<App> m_app` regardless of which build,
// dispatches the four lifecycle entry points below at the right moments,
// and otherwise stays out of the App's way. Engine's code has zero
// conditionals distinguishing "this is dev" from "this is game" — the
// dispatch through this interface is the architectural unification.
//
// Per-frame hooks (on_update / on_render) exist because the dev console
// uses immediate-mode ImGui, which needs frame-by-frame draw submission.
// Shell-UI-based Apps typically leave them empty — Shell UI is rendered
// by the engine itself, not the App.

#include "core/types.h"

namespace uldum::rhi { class CommandList; }

namespace uldum {

class Engine;
class SessionResult;
enum class AppState;

class App {
public:
    virtual ~App() = default;

    // Engine has booted all subsystems and the asset packs are mounted.
    // App sets up its UI, binds input, reads settings to initialize
    // state. Called once near the end of Engine::init.
    virtual void on_init(Engine&) = 0;

    // AppState just transitioned. Engine fires this once per actual
    // change (no-op transitions are filtered out). Default: empty.
    // Shell-UI Apps use this to load the screen RML for the new state.
    virtual void on_state_changed(AppState prev, AppState next) {
        (void)prev; (void)next;
    }

    // Per-frame logic. Runs on the main thread, outside the render
    // pass. Default: empty. Used for ImGui frame setup, polling user
    // input that doesn't go through Shell UI, etc.
    virtual void on_update(f32 dt) { (void)dt; }

    // Per-frame render. Runs inside the main render pass after the
    // world has been drawn. Default: empty. ImGui-based Apps submit
    // draw data here; Shell-UI-based Apps typically don't override
    // (the engine renders Shell UI separately).
    virtual void on_render(rhi::CommandList& cmd) { (void)cmd; }

    // A session just ended. The result carries whatever the engine
    // captured at session end. Default: empty.
    virtual void on_session_ended(const SessionResult&) {}
};

// Forward declaration only — the actual struct lives wherever the
// session result is materialized. Plumbed in as the App surface is
// built out; for the dev refactor the on_session_ended hook is not
// yet wired to anything that produces a populated result.
class SessionResult {};

// Trivial App that does nothing. Used as the default for game builds
// that don't yet provide their own App implementation (today's
// `uldum_game` Shell-UI build — Shell rendering is driven by the
// engine, not the App, so NullApp is functionally fine until the
// project supplies its own concrete App). Deletable once every
// game project on Uldum ships a real App.
class NullApp final : public App {
public:
    void on_init(Engine&) override {}
};

// The concrete App class is selected at build time via the
// `ULDUM_APP_HEADER` and `ULDUM_APP_CLASS` macros (set by CMake in
// `src/app/CMakeLists.txt`). Engine.cpp uses them like this:
//
//   #include ULDUM_APP_HEADER
//   m_app = std::make_unique<ULDUM_APP_CLASS>();
//
// Each binary picks one App at configure time — dev maps to DevApp,
// game-without-project-App maps to NullApp, and a future game project
// overrides both macros via its `game.cmake`.

} // namespace uldum
