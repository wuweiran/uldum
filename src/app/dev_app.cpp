#include "app/dev_app.h"

#include "app/engine.h"
#include "app/dev_console.h"
#include "network/network.h"
#include "network/lobby.h"
#include "core/log.h"

#ifdef ULDUM_ORCHESTRATOR_CLIENT
#include "app/session_request.h"
#include <string>
#endif

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

    // Dev convenience: a joining client auto-claims the first open slot so
    // you never have to click Claim. Runs while in the Lobby with no slot
    // yet held; sends C_CLAIM_SLOT for the first Open slot and remembers it
    // so we don't re-send every frame. If the host rejects (someone beat us
    // to it), the next S_LOBBY_STATE shows that slot taken → we pick the
    // next Open one → race resolves itself. Reset once we hold a slot or
    // leave the lobby.
    if (m_engine->state() == AppState::Lobby &&
        m_engine->launch_args().net_mode == network::Mode::Client) {
        auto& net = m_engine->network();
        const auto& lobby = net.lobby_state();
        u32 my_peer = net.client_peer_id();
        // Only once the host has assigned us a peer id and sent slots.
        if (my_peer != UINT32_MAX && !lobby.slots.empty()) {
            u32 mine = network::lobby_slot_for_peer(lobby, my_peer);
            if (mine != UINT32_MAX) {
                m_auto_claim_attempted = mine;  // seated — nothing to do
            } else {
                u32 open = UINT32_MAX;
                for (u32 i = 0; i < lobby.slots.size(); ++i) {
                    const auto& a = lobby.slots[i];
                    if (a.occupant == network::SlotOccupant::Open && !a.locked) { open = i; break; }
                }
                // Send only when the target changed (new snapshot moved the
                // first-open slot), so we don't spam the same claim each frame.
                if (open != UINT32_MAX && open != m_auto_claim_attempted) {
                    net.send_claim_slot(open);
                    m_auto_claim_attempted = open;
                    log::info(TAG, "Client auto-claiming slot {}", open);
                }
            }
        }
    } else {
        m_auto_claim_attempted = 0xFFFFFFFFu;  // reset outside the client lobby
    }

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
            if (m_console) {
                m_console->show_error(
                    "Failed to host on port " + std::to_string(args.port) +
                    " — the port may already be in use.");
            }
        }
    } else if (action.type == A::EnterLobbyClient && m_engine->state() == AppState::Menu) {
        args.map_path        = action.map_path;
        args.net_mode        = network::Mode::Client;
        args.connect_address = action.connect_address;
        args.port            = action.port;
        // Bearer token (empty for plain LAN). Engine forwards it to the
        // client's C_JOIN, which an orchestrator-spawned worker requires.
        args.auth_token.assign(action.token.begin(), action.token.end());
        if (m_engine->enter_lobby()) {
            m_engine->set_state(AppState::Lobby);
            log::info(TAG, "EnterLobby Client {}:{}", args.connect_address, args.port);
        } else {
            m_engine->leave_lobby();
            if (m_console) {
                m_console->show_error(
                    "Failed to connect to " + args.connect_address + ":" +
                    std::to_string(args.port) + ".");
            }
        }
#ifdef ULDUM_ORCHESTRATOR_CLIENT
    } else if (action.type == A::HostViaServer && m_engine->state() == AppState::Menu) {
        // Ask the orchestrator for a fresh worker, then join it as a client.
        // Blocking HTTP (5s timeout) — fine for the dev console.
        auto info = app::request_session(action.server_url, action.map_path);
        if (!info) {
            if (m_console)
                m_console->show_error("Host via Server failed — is uldum_server running at " +
                                      action.server_url + "?");
        } else {
            args.map_path        = action.map_path;
            args.net_mode        = network::Mode::Client;
            args.connect_address = info->addr;
            args.port            = info->port;
            const std::string& my_token = info->tokens[0];
            args.auth_token.assign(my_token.begin(), my_token.end());
            if (m_engine->enter_lobby()) {
                m_engine->set_state(AppState::Lobby);
                log::info(TAG, "Host via Server → worker {}:{}", info->addr, info->port);
                // Surface addr:port + spare tokens so a 2nd dev can join.
                std::string share = info->addr + ":" + std::to_string(info->port);
                if (info->tokens.size() > 1) {
                    share += "  token: " + info->tokens[1];
                    for (size_t i = 2; i < info->tokens.size(); ++i)
                        share += " / " + info->tokens[i];
                }
                if (m_console) m_console->show_session_info(share);
            } else {
                m_engine->leave_lobby();
                if (m_console)
                    m_console->show_error("Failed to connect to the spawned worker at " +
                                          info->addr + ":" + std::to_string(info->port) + ".");
            }
        }
#endif
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
