#pragma once

// SampleGameApp — the App implementation for the `sample_game` reference
// project. Owns the menu / lobby / loading / results screen flow:
// loads the right RML on each AppState transition and binds the per-
// screen button handlers. Everything in this file used to live in
// engine.cpp as a hardcoded Shell-UI dispatcher.

#include "app/app.h"

namespace uldum {

class Engine;

class SampleGameApp final : public App {
public:
    void on_init(Engine&) override;
    void on_state_changed(AppState prev, AppState next) override;

private:
    // Each `show_*` loads the screen's RML and binds its buttons.
    // Options is a sub-screen of Menu (no AppState change for it), so
    // it's invoked from the main menu's "options" button handler
    // rather than from on_state_changed.
    void show_main_menu();
    void show_options();
    void show_lobby();
    void show_loading();
    void show_results();

    Engine* m_engine = nullptr;
};

} // namespace uldum
