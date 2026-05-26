#include "asset/asset.h"
#include "core/log.h"
#include "core/types.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

// uldum_server: the orchestrator. Single deployment serves every Uldum
// game. Game backends POST `/sessions` over HTTP to provision a match;
// the orchestrator forks an `uldum_worker` per session, hands the
// game backend a `{addr, port, ...}` payload, and (in a later step)
// dispatches a webhook on game-end with the worker's result JSON.
//
// Phase 24 step 3: HTTP server up; `POST /sessions` actually spawns
// an `uldum_worker` child process and tracks it in a session table.
// Token-based auth, stdin config, stdout webhook dispatch land in
// subsequent steps.

static constexpr const char* TAG = "Server";

namespace {

struct ServerArgs {
    uldum::u16 port = 8080;
    uldum::u16 worker_port_min = 9000;
    uldum::u16 worker_port_max = 9999;
    std::string worker_binary = "uldum_worker";
    std::string public_addr   = "127.0.0.1";
    std::set<std::string> allowed_maps; // auto-discovered from <cwd>/maps/
};

struct Session {
    std::string id;
    uldum::u16  port = 0;
    std::string map;
    std::string webhook_url;   // empty = no webhook dispatch
#ifdef _WIN32
    HANDLE process_handle = nullptr;
    DWORD  pid = 0;
    HANDLE stdout_read = nullptr;
#else
    pid_t pid = -1;
    int   stdout_read = -1;
#endif
};

std::atomic<bool> g_running{true};
std::mutex                                       g_session_mutex;
std::unordered_map<std::string, Session>         g_sessions;       // session_id → Session
std::set<uldum::u16>                             g_used_ports;     // in-use worker ports

std::string random_hex(size_t bytes) {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen{rd()};
    std::uniform_int_distribution<uldum::u32> dist;
    std::string out;
    out.reserve(bytes * 2);
    const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i) {
        uldum::u8 b = static_cast<uldum::u8>(dist(gen) & 0xff);
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0xf]);
    }
    return out;
}

uldum::u16 pick_free_port(const ServerArgs& args) {
    // Linear scan over the configured range. Adequate for indie scale
    // (dozens to a few thousand sessions); revisit when we need O(1).
    for (uldum::u16 p = args.worker_port_min; p <= args.worker_port_max; ++p) {
        if (!g_used_ports.contains(p)) return p;
    }
    return 0;
}

// ── Cross-platform process spawn ─────────────────────────────────────────
//
// Spawns `argv[0]` with the given arguments. After spawn, writes
// `stdin_data` to the child's stdin and closes the write end (so the
// child's stdin read sees EOF). The worker's stdin config protocol is
// JSON-over-pipe — see worker_main.cpp.
//
// Cwd is set explicitly so the worker can find `engine.uldpak` next to
// its own binary regardless of where the orchestrator was launched from.

#ifdef _WIN32
bool spawn_worker(const std::vector<std::string>& argv,
                  const std::string& cwd,
                  const std::string& stdin_data,
                  Session& out) {
    // Stdin pipe: parent → child config JSON
    // Stdout pipe: child → parent result JSON
    // Both pipes need their child-end inheritable and parent-end non-
    // inheritable. We set the SA inheritable up front, then strip
    // HANDLE_FLAG_INHERIT off whichever end stays in the parent.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE child_stdin_r = nullptr;
    HANDLE parent_stdin_w = nullptr;
    if (!CreatePipe(&child_stdin_r, &parent_stdin_w, &sa, 0)) {
        uldum::log::error(TAG, "CreatePipe (stdin) failed (error {})", GetLastError());
        return false;
    }
    SetHandleInformation(parent_stdin_w, HANDLE_FLAG_INHERIT, 0);

    HANDLE parent_stdout_r = nullptr;
    HANDLE child_stdout_w  = nullptr;
    if (!CreatePipe(&parent_stdout_r, &child_stdout_w, &sa, 0)) {
        uldum::log::error(TAG, "CreatePipe (stdout) failed (error {})", GetLastError());
        CloseHandle(child_stdin_r);
        CloseHandle(parent_stdin_w);
        return false;
    }
    SetHandleInformation(parent_stdout_r, HANDLE_FLAG_INHERIT, 0);

    std::ostringstream oss;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) oss << ' ';
        oss << '"' << argv[i] << '"';
    }
    std::string cmd = oss.str();
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = child_stdin_r;
    si.hStdOutput = child_stdout_w;
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);  // inherit ours — worker logs go to stderr
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessA(
        nullptr, cmd_buf.data(),
        nullptr, nullptr,
        TRUE,
        0,
        nullptr,
        cwd.empty() ? nullptr : cwd.c_str(),
        &si, &pi);
    if (!ok) {
        DWORD err = GetLastError();
        uldum::log::error(TAG, "CreateProcess failed for '{}' (error {})", argv[0], err);
        CloseHandle(child_stdin_r);
        CloseHandle(parent_stdin_w);
        CloseHandle(parent_stdout_r);
        CloseHandle(child_stdout_w);
        return false;
    }
    CloseHandle(child_stdin_r);
    CloseHandle(child_stdout_w);
    CloseHandle(pi.hThread);

    if (!stdin_data.empty()) {
        DWORD written = 0;
        WriteFile(parent_stdin_w, stdin_data.data(),
                  static_cast<DWORD>(stdin_data.size()), &written, nullptr);
    }
    CloseHandle(parent_stdin_w);

    out.process_handle = pi.hProcess;
    out.pid            = pi.dwProcessId;
    out.stdout_read    = parent_stdout_r;
    return true;
}

bool worker_still_running(const Session& s) {
    if (!s.process_handle) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(s.process_handle, &code)) return false;
    return code == STILL_ACTIVE;
}

std::string drain_worker_stdout(Session& s) {
    std::string out;
    if (!s.stdout_read) return out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(s.stdout_read, buf, sizeof(buf), &n, nullptr) && n > 0) {
        out.append(buf, n);
    }
    return out;
}

void close_worker_handles(Session& s) {
    if (s.process_handle) { CloseHandle(s.process_handle); s.process_handle = nullptr; }
    if (s.stdout_read)    { CloseHandle(s.stdout_read);    s.stdout_read    = nullptr; }
}
#else
bool spawn_worker(const std::vector<std::string>& argv,
                  const std::string& cwd,
                  const std::string& stdin_data,
                  Session& out) {
    if (argv.empty()) return false;

    int stdin_pipe[2]  = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) < 0) {
        uldum::log::error(TAG, "pipe(stdin) failed (errno {})", errno);
        return false;
    }
    if (pipe(stdout_pipe) < 0) {
        uldum::log::error(TAG, "pipe(stdout) failed (errno {})", errno);
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        return false;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // Child stdin ← parent's stdin_pipe write end
    posix_spawn_file_actions_adddup2(&fa, stdin_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&fa, stdin_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, stdin_pipe[1]);
    // Child stdout → parent's stdout_pipe read end
    posix_spawn_file_actions_adddup2(&fa, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&fa, stdout_pipe[1]);

    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (const auto& a : argv) raw.push_back(const_cast<char*>(a.c_str()));
    raw.push_back(nullptr);

    std::string saved_cwd;
    if (!cwd.empty()) {
        char buf[4096];
        if (::getcwd(buf, sizeof(buf))) saved_cwd = buf;
        if (::chdir(cwd.c_str()) != 0) {
            uldum::log::error(TAG, "chdir('{}') failed (errno {})", cwd, errno);
            posix_spawn_file_actions_destroy(&fa);
            close(stdin_pipe[0]); close(stdin_pipe[1]);
            close(stdout_pipe[0]); close(stdout_pipe[1]);
            return false;
        }
    }

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, raw[0], &fa, nullptr, raw.data(), environ);

    if (!saved_cwd.empty()) (void)::chdir(saved_cwd.c_str());
    posix_spawn_file_actions_destroy(&fa);
    close(stdin_pipe[0]);   // parent doesn't need stdin read-end
    close(stdout_pipe[1]);  // parent doesn't need stdout write-end

    if (rc != 0) {
        uldum::log::error(TAG, "posix_spawnp failed for '{}' (errno {})", argv[0], rc);
        close(stdin_pipe[1]); close(stdout_pipe[0]);
        return false;
    }

    if (!stdin_data.empty()) {
        ssize_t total = 0;
        while (total < static_cast<ssize_t>(stdin_data.size())) {
            ssize_t n = write(stdin_pipe[1], stdin_data.data() + total, stdin_data.size() - total);
            if (n <= 0) break;
            total += n;
        }
    }
    close(stdin_pipe[1]);

    out.pid         = pid;
    out.stdout_read = stdout_pipe[0];
    return true;
}

bool worker_still_running(const Session& s) {
    if (s.pid <= 0) return false;
    int status = 0;
    pid_t r = waitpid(s.pid, &status, WNOHANG);
    return r == 0;
}

std::string drain_worker_stdout(Session& s) {
    std::string out;
    if (s.stdout_read < 0) return out;
    char buf[4096];
    ssize_t n;
    while ((n = read(s.stdout_read, buf, sizeof(buf))) > 0) {
        out.append(buf, n);
    }
    return out;
}

void close_worker_handles(Session& s) {
    if (s.stdout_read >= 0) { close(s.stdout_read); s.stdout_read = -1; }
}
#endif

// Parse the worker's stdout: with logs now going to stderr, the worker's
// stdout should contain exactly one JSON object (the final result).
// Be tolerant of stray whitespace / blank lines around it.
bool parse_worker_result(const std::string& stdout_text, nlohmann::json& out) {
    if (stdout_text.empty()) return false;
    try {
        out = nlohmann::json::parse(stdout_text);
        return true;
    } catch (...) {
        // Fallback: scan backward for the last JSON-shaped block.
        size_t end = stdout_text.find_last_of('}');
        if (end == std::string::npos) return false;
        size_t start = stdout_text.find_last_of('{', end);
        if (start == std::string::npos) return false;
        try {
            out = nlohmann::json::parse(stdout_text.substr(start, end - start + 1));
            return true;
        } catch (...) {
            return false;
        }
    }
}

void dispatch_webhook(const std::string& webhook_url,
                     const std::string& session_id,
                     const nlohmann::json& result) {
    // Parse webhook URL into base + path. cpp-httplib expects "scheme://host[:port]"
    // for the Client constructor and a separate path for the verb call.
    auto scheme_end = webhook_url.find("://");
    if (scheme_end == std::string::npos) {
        uldum::log::warn(TAG, "Webhook URL missing scheme: '{}'", webhook_url);
        return;
    }
    size_t host_start = scheme_end + 3;
    size_t path_start = webhook_url.find('/', host_start);
    std::string base = (path_start == std::string::npos)
        ? webhook_url
        : webhook_url.substr(0, path_start);
    std::string path = (path_start == std::string::npos)
        ? "/"
        : webhook_url.substr(path_start);

    httplib::Client cli(base);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(10, 0);

    nlohmann::json body;
    body["session_id"] = session_id;
    body["result"]     = result;

    auto res = cli.Post(path, body.dump(), "application/json");
    if (!res) {
        uldum::log::warn(TAG, "Webhook POST to '{}' failed: {}",
                         webhook_url, httplib::to_string(res.error()));
        return;
    }
    if (res->status >= 200 && res->status < 300) {
        uldum::log::info(TAG, "Webhook delivered for session '{}' (HTTP {})",
                         session_id, res->status);
    } else {
        uldum::log::warn(TAG, "Webhook for session '{}' returned HTTP {}: {}",
                         session_id, res->status, res->body);
    }
}

// ── Reaper thread ────────────────────────────────────────────────────────
//
// Walks the session table once a second; for each session whose worker
// has exited it: drains the captured stdout, parses the result JSON,
// fires the webhook (off-thread so a slow webhook doesn't block the
// next reap), frees the port, removes the session.
void reaper_loop() {
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        struct WebhookJob {
            std::string url;
            std::string session_id;
            nlohmann::json result;
        };
        std::vector<WebhookJob> webhooks_to_dispatch;

        {
            std::lock_guard lk(g_session_mutex);
            for (auto it = g_sessions.begin(); it != g_sessions.end(); ) {
                if (!worker_still_running(it->second)) {
                    uldum::log::info(TAG, "Session '{}' worker exited, freeing port {}",
                                     it->first, it->second.port);

                    std::string stdout_text = drain_worker_stdout(it->second);
                    nlohmann::json result;
                    bool parsed = parse_worker_result(stdout_text, result);

                    if (!it->second.webhook_url.empty()) {
                        if (parsed) {
                            webhooks_to_dispatch.push_back({it->second.webhook_url,
                                                            it->first, std::move(result)});
                        } else {
                            uldum::log::warn(TAG, "Session '{}' produced no parseable result; skipping webhook",
                                             it->first);
                        }
                    }

                    g_used_ports.erase(it->second.port);
                    close_worker_handles(it->second);
                    it = g_sessions.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Dispatch webhooks outside the session lock so a slow webhook
        // doesn't block other reaper ticks / new POSTs.
        for (auto& job : webhooks_to_dispatch) {
            dispatch_webhook(job.url, job.session_id, job.result);
        }
    }
}

// ── Map allowlist + manifest helpers ─────────────────────────────────────

// Read a map's manifest.json without bringing in MapManager / Simulation.
// The .uldmap is just a packed archive; AssetManager already knows how
// to open one and read a file out of it.
//
// Returns the number of player slots declared in the manifest, or
// 0 on any error (caller turns 0 into 4xx).
uldum::u32 read_player_count(const std::filesystem::path& map_path) {
    uldum::asset::AssetManager assets;
    if (!assets.init("engine")) return 0;

    std::string mount_prefix = "session_map";
    if (!assets.open_package(map_path.string(), mount_prefix)) return 0;

    auto bytes = assets.read_file_bytes(mount_prefix + "/manifest.json");
    if (bytes.empty()) return 0;

    try {
        auto doc = nlohmann::json::parse(bytes.begin(), bytes.end());
        if (auto it = doc.find("players"); it != doc.end() && it->is_array()) {
            return static_cast<uldum::u32>(it->size());
        }
    } catch (...) { /* fall through */ }
    return 0;
}

// ── HTTP handlers ────────────────────────────────────────────────────────

void handle_create_session(const ServerArgs& args,
                            const httplib::Request& req,
                            httplib::Response& res) {
    // Caller auth is the operator's responsibility — production
    // deployments terminate cloud-native auth (Azure AD / IAM / etc.)
    // at a reverse proxy in front of this orchestrator, and the
    // network layer keeps strangers from reaching this endpoint
    // directly. The engine itself stays cloud-neutral.
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(std::string("{\"error\":\"invalid json: ") + e.what() + "\"}\n",
                        "application/json");
        return;
    }

    auto map_it = body.find("map");
    if (map_it == body.end() || !map_it->is_string()) {
        res.status = 400;
        res.set_content("{\"error\":\"missing or non-string 'map' field\"}\n",
                        "application/json");
        return;
    }
    std::string map = map_it->get<std::string>();

    // Optional `webhook_url`: when present, the orchestrator POSTs the
    // worker's end-of-session result JSON here once the worker exits.
    // Empty / missing = no webhook (the result is dropped after parsing).
    std::string webhook_url;
    if (auto it = body.find("webhook_url"); it != body.end() && it->is_string()) {
        webhook_url = it->get<std::string>();
    }

    // Optional `initial_data`: arbitrary JSON forwarded verbatim to the
    // worker via stdin under the key "session". The worker turns it
    // into a `GAME_SESSION` global the map's main() can read. Engine
    // doesn't interpret the contents — game backends use it for
    // loadouts, custom rules, save state, anything per-match.
    nlohmann::json initial_data = nlohmann::json::value_t::null;
    if (auto it = body.find("initial_data"); it != body.end()) {
        initial_data = *it;
    }

    // Allowlist check. Anything not auto-discovered at startup is
    // rejected — closes off arbitrary-path passing as an attack vector.
    if (!args.allowed_maps.contains(map)) {
        res.status = 403;
        res.set_content("{\"error\":\"map not in allowlist\"}\n", "application/json");
        uldum::log::warn(TAG, "Rejected session-create: map '{}' not in allowlist", map);
        return;
    }

    // 3. Resolve paths + read the map's manifest to learn how many
    //    player tokens to mint. The manifest is the source of truth for
    //    player count; the request body doesn't carry it.
    std::filesystem::path worker_path = std::filesystem::absolute(args.worker_binary);
    std::filesystem::path worker_cwd  = worker_path.parent_path();
    std::filesystem::path map_path    = std::filesystem::absolute(map);

    uldum::u32 player_count = read_player_count(map_path);
    if (player_count == 0) {
        res.status = 422;
        res.set_content("{\"error\":\"failed to read map manifest or no players declared\"}\n",
                        "application/json");
        return;
    }

    std::lock_guard lk(g_session_mutex);

    uldum::u16 port = pick_free_port(args);
    if (port == 0) {
        res.status = 503;
        res.set_content("{\"error\":\"no free worker ports\"}\n", "application/json");
        return;
    }

    const std::string session_id = random_hex(8);

    Session session;
    session.id          = session_id;
    session.port        = port;
    session.map         = map;
    session.webhook_url = webhook_url;

    std::vector<std::string> argv = {
        worker_path.string(),
        "--map",  map_path.string(),
        "--port", std::to_string(port),
    };

    // 4. Generate one bearer token per slot. Hand them to the worker
    //    via stdin and back to the caller in the response. The caller
    //    (game backend, or uldum_dev acting as one) decides which
    //    token goes to which player.
    nlohmann::json tokens_for_worker = nlohmann::json::array();
    nlohmann::json tokens_response   = nlohmann::json::array();
    for (uldum::u32 i = 0; i < player_count; ++i) {
        std::string tok = random_hex(16);
        tokens_for_worker.push_back(tok);
        tokens_response.push_back(tok);
    }
    nlohmann::json stdin_doc;
    stdin_doc["tokens"] = std::move(tokens_for_worker);
    if (!initial_data.is_null()) {
        stdin_doc["session"] = initial_data;
    }
    std::string stdin_payload = stdin_doc.dump();

    if (!spawn_worker(argv, worker_cwd.string(), stdin_payload, session)) {
        res.status = 500;
        res.set_content("{\"error\":\"failed to spawn worker\"}\n", "application/json");
        return;
    }
    g_used_ports.insert(port);
    g_sessions.emplace(session_id, std::move(session));

    nlohmann::json out;
    out["session_id"] = session_id;
    out["addr"]       = args.public_addr;
    out["port"]       = port;
    out["tokens"]     = std::move(tokens_response);
    res.status = 201;
    res.set_content(out.dump() + "\n", "application/json");
    uldum::log::info(TAG, "Created session '{}' (map='{}', port={}, players={})",
                     session_id, map, port, player_count);
}

void handle_list_sessions(httplib::Response& res) {
    nlohmann::json arr = nlohmann::json::array();
    std::lock_guard lk(g_session_mutex);
    for (const auto& [id, s] : g_sessions) {
        nlohmann::json item;
        item["session_id"] = id;
        item["port"]       = s.port;
        item["map"]        = s.map;
        arr.push_back(std::move(item));
    }
    res.set_content(arr.dump() + "\n", "application/json");
}

// ── Args ─────────────────────────────────────────────────────────────────

void print_usage() {
    std::fprintf(stderr,
        "Usage: uldum_server [options]\n"
        "  --port <n>             HTTP listen port for the game-backend API. Default: 8080.\n"
        "  --worker-binary <path> Path to uldum_worker (default: 'uldum_worker' on PATH).\n"
        "  --worker-port-min <n>  Lowest UDP port to assign workers. Default: 9000.\n"
        "  --worker-port-max <n>  Highest UDP port to assign workers. Default: 9999.\n"
        "  --public-addr <addr>   Address returned to game backends. Default: 127.0.0.1.\n"
        "\n"
        "This service is unauthenticated. Production deployments MUST terminate\n"
        "caller auth at a reverse proxy (Azure API Management, App Gateway,\n"
        "nginx, etc.) and isolate the orchestrator at the network layer.\n");
}

// Auto-discover allowed maps from `<cwd>/maps/`. Anything matching the
// `.uldmap` file extension is admitted.
void discover_allowed_maps(ServerArgs& args) {
    namespace fs = std::filesystem;
    fs::path maps_dir = fs::current_path() / "maps";
    if (!fs::exists(maps_dir) || !fs::is_directory(maps_dir)) return;
    for (const auto& entry : fs::directory_iterator(maps_dir)) {
        if (!entry.is_regular_file()) continue;
        auto p = entry.path();
        if (p.extension() == ".uldmap") {
            // Store both the relative form (what requests typically send,
            // matching the path style the dev maps are referenced as) and
            // the absolute form, so the allowlist check is tolerant of
            // both.
            args.allowed_maps.insert(("maps/" + p.filename().string()));
            args.allowed_maps.insert(p.string());
        }
    }
}

bool parse_args(int argc, char* argv[], ServerArgs& out) {
    for (int i = 1; i < argc; ++i) {
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--port") == 0) {
            const char* v = need_value("--port"); if (!v) return false;
            out.port = static_cast<uldum::u16>(std::stoi(v));
        } else if (std::strcmp(argv[i], "--worker-binary") == 0) {
            const char* v = need_value("--worker-binary"); if (!v) return false;
            out.worker_binary = v;
        } else if (std::strcmp(argv[i], "--worker-port-min") == 0) {
            const char* v = need_value("--worker-port-min"); if (!v) return false;
            out.worker_port_min = static_cast<uldum::u16>(std::stoi(v));
        } else if (std::strcmp(argv[i], "--worker-port-max") == 0) {
            const char* v = need_value("--worker-port-max"); if (!v) return false;
            out.worker_port_max = static_cast<uldum::u16>(std::stoi(v));
        } else if (std::strcmp(argv[i], "--public-addr") == 0) {
            const char* v = need_value("--public-addr"); if (!v) return false;
            out.public_addr = v;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            return false;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return false;
        }
    }
    if (out.worker_port_min > out.worker_port_max) {
        std::fprintf(stderr, "worker-port-min must be <= worker-port-max\n");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    ServerArgs args;
    if (!parse_args(argc, argv, args)) {
        print_usage();
        return 2;
    }
    discover_allowed_maps(args);

    uldum::log::info(TAG, "=== Uldum Server (orchestrator) ===");
    uldum::log::info(TAG, "Allowed maps: {} discovered under <cwd>/maps/",
                     args.allowed_maps.size() / 2);  // each map adds 2 forms (relative + absolute)
    uldum::log::info(TAG, "Listening on http://0.0.0.0:{}  (unauthenticated — front with a proxy in production)",
                     args.port);
    uldum::log::info(TAG, "Worker binary: '{}'", args.worker_binary);
    uldum::log::info(TAG, "Worker port range: {}-{}", args.worker_port_min, args.worker_port_max);
    uldum::log::info(TAG, "Public address: {}", args.public_addr);

    httplib::Server http;

    http.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK\n", "text/plain");
    });
    http.Get("/sessions", [](const httplib::Request&, httplib::Response& res) {
        handle_list_sessions(res);
    });
    http.Post("/sessions", [&args](const httplib::Request& req, httplib::Response& res) {
        handle_create_session(args, req, res);
    });

    std::signal(SIGINT, [](int) {
        uldum::log::info(TAG, "Shutdown signal received");
        g_running.store(false);
    });

    // Background reaper drops dead sessions.
    std::thread reaper(reaper_loop);

    // Watchdog stops the HTTP server when g_running flips false.
    std::thread watchdog([&http]() {
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        http.stop();
    });

    if (!http.listen("0.0.0.0", args.port)) {
        uldum::log::error(TAG, "Failed to bind http://0.0.0.0:{}", args.port);
        g_running.store(false);
        if (watchdog.joinable()) watchdog.join();
        if (reaper.joinable())   reaper.join();
        return 1;
    }

    g_running.store(false);
    if (watchdog.joinable()) watchdog.join();
    if (reaper.joinable())   reaper.join();
    uldum::log::info(TAG, "Server shut down");
    return 0;
}
