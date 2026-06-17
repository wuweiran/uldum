#include "sample_game_app.h"

#include "app/engine.h"
#include "core/log.h"
#include "core/settings.h"
#include "network/network.h"
#include "shell/shell.h"

#include <cstdio>

namespace uldum {

static constexpr const char* TAG = "SampleGameApp";

void SampleGameApp::on_init(Engine& engine) {
    m_engine = &engine;
    // AppState starts at Menu and on_state_changed only fires on
    // transitions, so the initial screen has to be shown explicitly.
    show_main_menu();
}

void SampleGameApp::on_state_changed(AppState /*prev*/, AppState next) {
    switch (next) {
        case AppState::Menu:    show_main_menu();                       break;
        case AppState::Lobby:   show_lobby();                           break;
        case AppState::Loading: show_loading();                         break;
        case AppState::Playing: m_engine->shell().hide_current_document(); break;
        case AppState::Results: show_results();                         break;
    }
}

void SampleGameApp::show_main_menu() {
    auto& s = m_engine->shell();
    s.load_document("shell/main_menu.rml");
    s.bind("play", [this] {
        if (m_engine->state() != AppState::Menu) return;
        auto& args = m_engine->launch_args();
        args.map_path = "maps/simple_map.uldmap";
        args.net_mode = network::Mode::Offline;
        if (!m_engine->enter_lobby()) {
            log::error(TAG, "enter_lobby failed");
            return;
        }
        m_engine->set_state(AppState::Lobby);
    });
    s.bind("options", [this] { show_options(); });
    s.bind("quit",    [this] { m_engine->request_quit(); });
}

void SampleGameApp::show_options() {
    auto& s = m_engine->shell();
    s.load_document("shell/options.rml");
    auto refresh = [this] {
        bool on = m_engine->settings().get_f32("audio.master_volume", 1.0f) > 0.0f;
        m_engine->shell().set_element_text("sound_toggle",
                                            on ? "Sound: ON" : "Sound: OFF");
    };
    refresh();
    s.bind("sound_toggle", [this, refresh] {
        bool on = m_engine->settings().get_f32("audio.master_volume", 1.0f) > 0.0f;
        m_engine->settings().set("audio.master_volume", on ? 0.0f : 1.0f);
        refresh();
    });
    s.bind("back", [this] { show_main_menu(); });
}

void SampleGameApp::show_lobby() {
    // No bindings yet — offline lobby auto-advances to Loading via the
    // engine's existing lobby handling. A real lobby UI lands later.
    m_engine->shell().load_document("shell/lobby.rml");
}

void SampleGameApp::show_loading() {
    m_engine->shell().load_document("shell/loading.rml");
}

void SampleGameApp::show_results() {
    auto& s = m_engine->shell();
    s.load_document("shell/results.rml");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Time: %.1f s",
                  m_engine->last_session_elapsed_seconds());
    s.set_element_text("time_label", buf);
    s.bind("back", [this] { m_engine->set_state(AppState::Menu); });
}

} // namespace uldum
