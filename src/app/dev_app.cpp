#include "app/dev_app.h"

#include "app/engine.h"
#include "app/dev_console.h"
#include "core/log.h"

#include <utility>

namespace uldum {

static constexpr const char* TAG = "DevApp";

DevApp::DevApp() = default;

DevApp::~DevApp() {
    // DevConsole's cleanup is in an explicit `shutdown()` method (the
    // implicit destructor doesn't tear down ImGui state on its own).
    // Run it before the unique_ptr resets.
    if (m_console) m_console->shutdown();
}

void DevApp::on_init(Engine& engine) {
    m_engine = &engine;
    m_console = std::make_unique<DevConsole>();
    if (!m_console->init(m_engine->rhi(), m_engine->platform(),
                         m_engine->settings(), [eng = m_engine] { eng->save_settings(); })) {
        log::error(TAG, "DevConsole init failed");
        m_console.reset();
    }
}

void DevApp::on_update(f32 dt) {
    if (!m_console || !m_engine) return;

    m_console->update(dt, m_engine->state(), m_engine->network());

    // Translate dev-console actions into Engine state transitions
    // through Engine's public verbs — the same surface a future
    // SampleGameApp would use.
    auto action = m_console->poll_action();
    using A = DevConsole::ActionType;
    auto& args = m_engine->launch_args();

    if (action.type == A::EnterLobbyOffline && m_engine->state() == AppState::Menu) {
        args.map_path = action.map_path;
        args.net_mode = network::Mode::Offline;
        if (m_engine->enter_lobby()) {
            m_engine->set_state(AppState::Lobby);
            log::info(TAG, "EnterLobby Offline '{}'", args.map_path);
        } else {
            log::error(TAG, "enter_lobby failed");
            m_engine->leave_lobby();
        }
    } else if (action.type == A::EnterLobbyHost && m_engine->state() == AppState::Menu) {
        args.map_path = action.map_path;
        args.net_mode = network::Mode::Host;
        args.port     = action.port;
        if (m_engine->enter_lobby()) {
            m_engine->set_state(AppState::Lobby);
            log::info(TAG, "EnterLobby Host '{}' port {}", args.map_path, args.port);
        } else {
            m_engine->leave_lobby();
        }
    } else if (action.type == A::EnterLobbyClient && m_engine->state() == AppState::Menu) {
        args.map_path        = action.map_path;
        args.net_mode        = network::Mode::Client;
        args.connect_address = action.connect_address;
        args.port            = action.port;
        if (m_engine->enter_lobby()) {
            m_engine->set_state(AppState::Lobby);
            log::info(TAG, "EnterLobby Client {}:{}", args.connect_address, args.port);
        } else {
            m_engine->leave_lobby();
        }
    } else if (action.type == A::ClaimSlot && m_engine->state() == AppState::Lobby) {
        m_engine->network().send_claim_slot(action.slot);
    } else if (action.type == A::ReleaseSlot && m_engine->state() == AppState::Lobby) {
        m_engine->network().send_release_slot(action.slot);
    } else if (action.type == A::StartGame && m_engine->state() == AppState::Lobby) {
        auto& net = m_engine->network();
        u32 my_peer = (args.net_mode == network::Mode::Client)
            ? net.client_peer_id() : network::LOCAL_PEER;
        u32 my_slot = network::lobby_slot_for_peer(net.lobby_state(), my_peer);
        args.local_slot = (my_slot == UINT32_MAX) ? 0 : my_slot;
        if (args.net_mode == network::Mode::Host) {
            net.host_commit_start();
        }
        m_engine->set_state(AppState::Loading);
        log::info(TAG, "StartGame (local slot {})", args.local_slot);
    } else if (action.type == A::LeaveLobby && m_engine->state() == AppState::Lobby) {
        m_engine->leave_lobby();
        m_engine->set_state(AppState::Menu);
    } else if (action.type == A::EndSession && m_engine->is_session_active()) {
        m_engine->end_session();
        m_engine->set_state(AppState::Menu);
    } else if (action.type == A::Quit) {
        m_engine->request_quit();
    }
}

void DevApp::on_render(rhi::CommandList& cmd) {
    if (m_console) m_console->render(cmd);
}

void DevApp::on_session_ended(const SessionResult&) {
    // No-op for now. The dev console's results-screen rendering is
    // driven from `Engine::update_shell_for_state` based on the App
    // state, unchanged by this refactor.
}

void DevApp::set_active_locale(std::string code) {
    if (m_console) m_console->set_active_locale(std::move(code));
}

} // namespace uldum
