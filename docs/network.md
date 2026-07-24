# Uldum Engine — Network Protocol

Server-authoritative multiplayer with client-side interpolation. No client-side prediction — for an RTS, a small command delay (~60ms on LAN) is expected and acceptable.

## Architecture

```
               ┌──────────────────────┐
               │      GameServer      │
               │  Simulation (32 Hz)  │
               │  ScriptEngine        │
               └──────┬───────────────┘
                      │
         ┌────────────┼────────────┐
         │            │            │
    ┌────▼────┐  ┌────▼────┐  ┌───▼─────┐
    │ Local   │  │ Remote  │  │ Remote  │
    │ Client  │  │ Client  │  │ Client  │
    │ (host)  │  │ (peer)  │  │ (peer)  │
    └─────────┘  └─────────┘  └─────────┘
```

**Host** runs the GameServer in-process and also acts as a client (player 0). Remote clients connect over the network. Single player is just a host with no remote clients.

## Modes

| Command | Behavior |
|---|---|
| `uldum_dev.exe` | Local only — no networking, same as before |
| `uldum_dev.exe --host` | Host game on port 7777, play as player 0 |
| `uldum_dev.exe --connect <ip>` | Connect to host, assigned next available player slot |

Default port: 7777. The `--host` flag enables the ENet listener; without it, networking is completely inactive.

## Transport Layer

Abstracted behind a `Transport` interface so ENet can be replaced later (e.g., with QUIC or TCP fallback for environments that block UDP).

```cpp
class Transport {
public:
    virtual bool host(u16 port, u32 max_clients) = 0;
    virtual bool connect(std::string_view address, u16 port) = 0;
    virtual void disconnect() = 0;
    virtual void send(u32 peer_id, std::span<const u8> data, bool reliable) = 0;
    virtual void poll() = 0;

    std::function<void(u32 peer_id)> on_connect;
    std::function<void(u32 peer_id)> on_disconnect;
    std::function<void(u32 peer_id, std::span<const u8> data)> on_receive;
};
```

Phase 13b uses **ENet** (reliable/unreliable UDP):
- Channel 0 (reliable ordered): commands, join/leave, entity create/destroy
- Channel 1 (unreliable sequenced): state snapshots

## Protocol

All messages are binary with a 1-byte type header. No FlatBuffers — messages are simple fixed-size records.

Message IDs are organized by top-nibble category: client/server direction + phase. Gaps in each block leave room for future messages.

### Client → Server

| Type | ID | Reliability | Content |
|---|---|---|---|
| **Lobby** | | | |
| `C_JOIN` | 0x01 | reliable | `u32 map_hash, string player_name` — verify map + identify player |
| `C_CLAIM_SLOT` | 0x02 | reliable | `u32 slot` — claim slot as Human (me) |
| `C_RELEASE_SLOT` | 0x03 | reliable | `u32 slot` — release my claim |
| `C_LOAD_DONE` | 0x04 | reliable | (empty) — map content finished loading |
| **Playing** | | | |
| `C_ORDER` | 0x10 | reliable | serialized `GameCommand` — unit order |
| **Any phase** | | | |
| `C_LEAVE` | 0x20 | reliable | (empty) — clean disconnect |

`C_ORDER` is a serialized `GameCommand`. The server validates ownership (the commanding player must own the units) before executing.

### Server → Client

| Type | ID | Reliability | Content |
|---|---|---|---|
| **Lobby / pre-game** | | | |
| `S_REJECT` | 0x40 | reliable | `u8 reason` (0=full, 1=wrong map, 2=started) |
| `S_LOBBY_ASSIGN` | 0x41 | reliable | `u32 peer_id` — sent once on lobby join so the client knows which rows are "mine" |
| `S_LOBBY_STATE` | 0x42 | reliable | full snapshot of the lobby (map + slot table) |
| `S_LOBBY_COMMIT` | 0x43 | reliable | (empty) — host locked the lobby, enter Loading |
| `S_WELCOME` | 0x44 | reliable | `u32 player_id, u32 player_count, u32 tick_rate` — sent at end of Loading with the peer's finalized slot |
| **Playing — entity sync** | | | |
| `S_SPAWN` | 0x50 | reliable | `u32 entity_id, u32 type_hash, u8 owner, f32 x, f32 y, f32 facing` — entity born in the player's sight (plays birth) |
| `S_SHOW` | 0x55 | reliable | same payload as `S_SPAWN` — an existing entity entered the player's sight (no birth) |
| `S_STATE` | 0x52 | unreliable | `u32 tick, u16 count, EntityState[]` — see below (dead is a state flag, not a separate message) |
| `S_HIDE` | 0x56 | reliable | `u32 entity_id` — entity left the player's sight (client decides whether to remember it) |
| `S_DESTROY` | 0x51 | reliable | `u32 entity_id` — entity removed from the world (not the same as "killed" or "left sight") |
| `S_UPDATE` | 0x53 | reliable | on-change attribute / state / ability / inventory delta |
| `S_SOUND` | 0x54 | unreliable | `u16 path_len, char[] path, f32 x, f32 y, f32 z` |
| **Playing — session events** | | | |
| `S_START` | 0x60 | reliable | (empty) — all players loaded, game begins |
| `S_END` | 0x61 | reliable | `u32 winning_team, u16 stats_len, char[] stats_json` (winning_team = UINT32_MAX → no winner) |
| `S_PAUSE_STATE` | 0x62 | reliable | mid-game disconnect snapshot — see below |

### LobbyState snapshot (S_LOBBY_STATE payload)

```
string  map_path
string  map_name
u16     slot_count
for each slot (array index = player id):
  u32    team
  string color
  u8     occupant   (0=Open, 1=Computer, 2=Human)
  bool   locked     (true for map-declared Computer slots)
  u32    peer_id    (valid iff occupant=Human)
  string display_name
```

Host is authoritative: on every mutation (claim / release / peer join or leave) host broadcasts a fresh `S_LOBBY_STATE`. Clients mirror.

### EntityState (per-entity in S_STATE)

```
u32  entity_id
f32  x, y, z          // position
f32  facing            // radians
f32  health_frac       // 0.0–1.0 (current / max)
u8   flags             // bit 0: moving, bit 1: attacking, bit 2: casting, bit 3: dead
u32  target_id         // combat/cast target (0 = none)
```

28 bytes per entity. At 100 entities, S_STATE is ~2.8 KB per tick, ~90 KB/s at 32 Hz. Manageable for LAN; delta compression can reduce this later.

## State Sync

### Initial Join

When a client connects and sends `C_JOIN`:
1. Server verifies map hash matches
2. Server sends `S_WELCOME` with assigned player ID
3. Server sends a burst of `S_SPAWN` for every existing entity
4. Client starts receiving `S_STATE` snapshots
5. Client begins rendering after receiving two consecutive snapshots (for interpolation)

### Ongoing

- Server sends `S_STATE` every tick (32 Hz) containing all entities currently visible to that player
- Server sends `S_SPAWN` (born in sight) / `S_SHOW` (entered sight) / `S_HIDE` (left sight) / `S_DESTROY` (removed from the world) as entities enter and leave each player's vision
- Server filters by fog of war — it ships only what a player can currently see, and never tells a client what to remember. A killed unit is not a separate message: death is a flag in `S_STATE`, and the client shows the corpse like any other state.

### Client-owned fog memory

The server only ever sends a player what is **currently visible**. What to do when something leaves sight is the client's decision, not the server's: on `S_HIDE` the client remembers static things (buildings, destructables) by freezing their last-seen state, and forgets mobile units. When the tile is scouted again, the client drops the remembered copy and resumes live updates. This keeps a player's out-of-sight memory entirely local and identical whether the player is the host or a remote client.

### Client Interpolation

The client does not run `Simulation.tick()`. It buffers the two most recent `S_STATE` snapshots and interpolates between them based on wall-clock time:

```
render_time = current_time - one_tick_duration
alpha = (render_time - snapshot_old.time) / (snapshot_new.time - snapshot_old.time)
position = lerp(snapshot_old.position, snapshot_new.position, alpha)
```

This means the client always renders one tick behind the server (~31ms at 32 Hz). Visually imperceptible, but guarantees smooth movement even with network jitter.

### Fog of War

Each client computes its own fog of war locally from the entities the server sends it. Since the server only sends currently-visible entities (fog-filtered), the client's fog naturally matches — tiles with no visible allied units stay dark. Remembered statics (see [Client-owned fog memory](#client-owned-fog-memory)) are drawn dimmed from the client's own snapshot, not from anything the server keeps sending.

The client still runs `Vision::update()` and `update_visual()` locally for smooth fog transitions.

## Command Flow

### Single Player (no networking)

```
Input → CommandSystem → issue_order() → World (immediate)
```

Same as Phase 13a. No change.

### Host

```
Local Input → CommandSystem → issue_order() → World (immediate)
Remote C_ORDER → validate ownership → issue_order() → World
```

The host's local commands execute immediately (zero latency). Remote commands arrive via ENet and are applied the same way.

### Remote Client

```
Input → serialize C_ORDER → send to server
                                    ↓
Server receives → validate → issue_order() → World
                                    ↓
S_STATE broadcast → client receives → interpolate → render
```

The remote client sees the result of its command after one round trip (~2 ticks on LAN).

## Session Lifecycle

```
Host starts lobby    → listening on port, m_phase=Lobby
Clients connect      → C_JOIN(map_hash, player_name) → S_LOBBY_ASSIGN + S_LOBBY_STATE
Peers pick slots     → C_CLAIM_SLOT / C_RELEASE_SLOT → host mutates → S_LOBBY_STATE broadcast
Host presses Start   → host_commit_start:
                          bind each peer.player to its claimed slot
                          S_WELCOME + S_SPAWN burst per peer
                          S_START broadcast
                          m_phase=Playing
Simulation ticks     → S_STATE broadcast per tick
Lua calls EndGame    → S_END broadcast → session over
```

A `NetworkManager::Phase` flag (`None / Lobby / Playing`) gates which incoming messages are honored. Offline mode skips all of this — simulation starts immediately.

### Worker auto-start + auto-exit (headless)

The dedicated `uldum_worker` has no UI to press Start, so it manages the lobby → playing → over transitions itself:

- **Start (countdown).** Once every *connected* peer has claimed a slot (and at least one peer is present), the worker begins a **5-second countdown**, then `host_commit_start`. Open slots are allowed at start — they go unused — so a 2-player map starts with a single player. Any lobby change that unseats a peer or empties the lobby (a join, a leave, a slot release) cancels the countdown; it restarts when the seated condition holds again. (A dev `--host` uses its lobby **Start** button instead; only the headless worker counts down.)
- **Exit on abandonment.** A started session with no connected peers and an empty reconnect list is unplayable — nobody can rejoin. The worker ends it via the normal EndGame path (no winner), writes its result to stdout, and exits; the orchestrator's reaper then frees the port. This is what lets a spawned worker clean itself up after the last player leaves (see [Reconnect](#reconnect) for the timeout that drains the reconnect list first).

Multi-session orchestration lives in `uldum_server` on top of multiple worker processes — see [Production Deployment Topology](#production-deployment-topology).

### Running a server-client session in dev

`uldum_server` is the **orchestrator** (an HTTP control plane, default port **8080**), not a game server. It never runs a game — it spawns one `uldum_worker` per session on a UDP port from **9000–9999**. Three distinct listeners are in play, and it's easy to conflate them:

| Port | Who | Protocol | Role |
|---|---|---|---|
| 7777 | `uldum_dev --host` / `--connect` | UDP | Direct-LAN **game** port (the dev host *is* the game server) |
| 8080 | `uldum_server` | HTTP | Orchestrator control-plane API (`POST /sessions`) — runs no game |
| 9000–9999 | each `uldum_worker` | UDP | The **game** port for one spawned session |

So `7777` is a game port, never the orchestrator's. To run the orchestrator loop (run both from `build/bin/` — the map allowlist scans `build/bin/maps/` for packed `.uldmap` **files**; repo-root `maps/*.uldmap` are source dirs and get a 403):

```
# once:
./uldum_server.exe                 # HTTP :8080, spawns workers on 9000-9999

# each session — CLI:
./uldum_dev.exe --server http://127.0.0.1:8080 --map maps/test_map.uldmap

# ...or GUI (desktop dev console): launch ./uldum_dev.exe, pick a map, click
# "Host via Server" (URL prefilled http://127.0.0.1:8080).
```

`uldum_dev` acts as its own game-backend: it `POST`s `/sessions`, the orchestrator forks a worker and returns `{ addr, port, tokens }`, and the dev connects to that worker as a client with `tokens[0]` — you never type the 9000-9999 port. The orchestrator mints **one token per player slot** and the worker accepts **any** of them (set membership, not slot-pinned), so a second dev can join the same session: the first dev's console shows the worker `addr:port` + the spare token(s) (with a **Copy** button), and the second dev pastes those into the console's **Connect** address + **Token** fields. (The `--server` / "Host via Server" orchestrator flow is desktop-only; the Android dev build is offline-only.)

## Reconnect

When a client disconnects, the server keeps their player state (units, buildings) alive:

1. Disconnect detected → player moved to disconnected list, `on_player_disconnected` Lua event fires
2. If `manifest.reconnect.pause` is true, simulation pauses for all players
3. Timer counts down (`manifest.reconnect.timeout`, default 60s)
4. If client reconnects (sends C_JOIN again) → gets S_WELCOME + full S_SPAWN burst + S_START, game resumes
5. If timeout expires → `on_player_dropped` Lua event fires, game resumes, map script decides what happens to the player's units. On a headless `uldum_worker`, if that drop leaves *no* connected players and an empty reconnect list, the session is abandoned and the worker exits (see [Worker auto-start + auto-exit](#worker-auto-start--auto-exit-headless)).

Manifest config:
```json
"reconnect": {
    "timeout": 60,
    "pause": true
}
```

### EndGame

```lua
-- Map script calls this when a win condition is met.
-- First arg is the winning TEAM (manifest team index; each player is
-- their own team in a FFA map). Use 0xFFFFFFFF for a draw / no winner.
EndGame(0, '{"kills": 15, "time": 302}')
```

The engine fires `on_game_end` event (for triggers), then broadcasts `S_END` with the winning team and a JSON stats string. The stats format is entirely map-defined — the engine just passes it through. The worker forwards `{ winning_team, stats, duration_s }` to the game backend's webhook on exit.

- Server can serve one game at a time (multi-game deferred)

## Production Deployment Topology

Everything above describes the wire protocol between a worker and its clients. For production deployment, an orchestrator (`uldum_server`) sits on top of multiple worker processes (`uldum_worker`), and per-game backends handle identity and persistence entirely outside the engine. This section is the end-to-end picture.

### Actors

| Actor | Who writes it | Where it runs |
|---|---|---|
| **Client app** | Game dev | Player's phone — App Store / Play Store binary embeds the engine |
| **Game backend** | Game dev (any language) | Game dev's cloud — owns login + identity + profiles + persistence; never inside the engine repo |
| **`uldum_server`** | Engine (game-agnostic) | Single deployment that all games on Uldum share. HTTP API for game backends, spawns workers, dispatches game-end webhooks |
| **`uldum_worker`** | Engine (game-agnostic) | Spawned by `uldum_server` per game session; one process per active session; exits when the game ends |

### Sequence

```
Player    Client App     Game Backend        uldum_server         uldum_worker
  │           │              │                     │                   │
  │  Sign in / tap Play (game-side UI, vendor SDK of game dev's choice)│
  │           │ login + auth │                     │                   │
  │           │─────────────>│                     │                   │
  │           │              │                     │                   │
  │           │ "find game"  │                     │                   │
  │           │─────────────>│                     │                   │
  │           │              │  POST /sessions     │                   │
  │           │              │  { map,             │                   │
  │           │              │    webhook_url,     │                   │
  │           │              │    initial_data }   │                   │
  │           │              │────HTTPS via proxy─>│                   │
  │           │              │  (reverse proxy     │                   │
  │           │              │   terminates cloud- │                   │
  │           │              │   native auth here) │                   │
  │           │              │                     │ pick free port    │
  │           │              │                     │ generate tokens   │
  │           │              │                     │ fork+exec ───────>│ (starts)
  │           │              │                     │ pass config via   │
  │           │              │                     │ child's stdin     │
  │           │              │                     │                   │
  │           │              │ { server_addr,      │                   │
  │           │              │   port, tokens[] }  │                   │
  │           │              │<───HTTPS────────────│                   │
  │           │              │                                         │
  │           │ { addr, port,│                                         │
  │           │   token }    │                                         │
  │           │<─────────────│                                         │
  │           │                                                        │
  │           │ UDP connect ──────────────────────────────────────────>│
  │           │ C_JOIN { map_hash, token, name } ─────────────────────>│
  │           │                                                        │ auth callback:
  │           │                                                        │   check token in
  │           │                                                        │   expected-joiners
  │           │                                                        │   table (in-memory)
  │           │                                                        │
  │           │ S_LOBBY_STATE ◄────────────────────────────────────────│
  │           │                                                        │
  │           │ ... game proceeds ...                                  │
  │           │                                                        │ game ends
  │           │                                                        │ writes result JSON
  │           │                                                        │ to stdout, exits
  │           │              │                     │ reads child stdout│
  │           │              │                     │ on process exit   │
  │           │              │ POST <webhook_url>  │                   │
  │           │              │ { results, ... }    │                   │
  │           │              │<───HTTPS────────────│                   │
  │           │              │ game backend awards │                   │
  │           │              │ XP, updates profile,│                   │
  │           │              │ etc.                │                   │
```

### Step responsibilities

| # | Step | Owner |
|---|------|-------|
| 1 | Player launches client app, logs in via game backend | Game dev (client + backend) |
| 2 | Game backend authenticates the player however it wants | Game dev's backend |
| 3 | Client asks game backend for a match | Game dev's client + backend |
| 4 | Game backend POSTs `/sessions` to `uldum_server` with the match config | Game dev's backend |
| 5 | (Production) reverse proxy in front of `uldum_server` terminates cloud-native auth (Azure AD / IAM / etc.); `uldum_server` itself stays cloud-neutral | Operator / proxy |
| 6 | `uldum_server` picks a free UDP port from its range, generates random bearer tokens (one per player declared in the map's manifest) | Engine — `uldum_server` |
| 6 | `uldum_server` fork+execs `uldum_worker`, passes config (map, tokens, port, etc.) via the child's stdin | Engine — `uldum_server` |
| 7 | `uldum_server` fork+execs `uldum_worker`, passes config (map, tokens, etc.) via the child's stdin | Engine — `uldum_server` |
| 8 | `uldum_server` responds with `{ addr, port, tokens }` to the game backend | Engine — `uldum_server` |
| 9 | Game backend gives each player their token + connection info | Game dev's backend |
| 10 | Players connect to the worker via UDP, present token in `C_JOIN` | Engine — worker / client |
| 11 | `uldum_worker` checks the token against its in-memory expected-joiners table | Engine — `uldum_worker` |
| 12 | Game proceeds. On game-end, worker writes result JSON to stdout, exits | Engine — `uldum_worker` |
| 13 | `uldum_server` reads the worker's stdout, POSTs results to the game backend's webhook URL, frees the port | Engine — `uldum_server` |

### Tokens

Per-player session tokens are simple **random bearer tokens** (e.g. 128-bit UUIDs), held in an in-memory table on the worker. No crypto on the engine side: HTTPS between game backend ↔ player protects the token in transit, the token is unguessable due to randomness, and it never leaves the trust circle (game backend, player, worker).

When a worker spawns, the orchestrator hands it a list of expected `(token, display_name)` pairs via stdin config. On every `C_JOIN`, the worker checks the presented token against this table. Unknown token → `S_REJECT(Unauthorized)`.

### Caller auth (game backend → `uldum_server`)

`uldum_server` itself is **unauthenticated**. It does not validate the caller's identity. This is a deliberate choice: every cloud has its own service-to-service auth pattern (Azure AD / Managed Identity, AWS IAM / SigV4, GCP Workload Identity, Kubernetes ServiceAccount tokens), and baking any one of them into the engine would lock deployments into that cloud — or require maintaining N codepaths.

Instead, production deployments terminate caller auth at a reverse proxy in front of the orchestrator:

```
game backend  →  Azure API Management / nginx / Cloudflare Access  →  uldum_server
              (validates the cloud-native identity token here,
               drops requests that don't authenticate)
```

The orchestrator is reachable only through the proxy (network-layer isolation). Local dev points game-backend clients (or `uldum_dev`) directly at the orchestrator's port; no proxy involved.

### Auth-on-join callback (server-side seam)

`uldum_worker` exposes a configurable validator that fires on every `C_JOIN`:

```cpp
using AuthCallback = std::function<bool(std::span<const u8> token,
                                        std::string_view peer_name)>;
```

**Default behavior is set by the spawn mode:**

| `uldum_worker` startup | Behavior |
|---|---|
| Spawned by `uldum_server` (production) | Validator checks the token against the expected-joiners table passed via stdin |
| Standalone (LAN / dev / single-player, no stdin config) | No tokens table → accept all; Phase 23's "operator supplies a map" path keeps working unchanged |

### Inter-process communication

The orchestrator ↔ worker channel is whatever the OS gives us by default: the worker's stdout/stderr handles, captured by the orchestrator at spawn time.

- **stdin (worker)**: orchestrator writes the JSON config blob once at spawn (map path, tokens, expected players, initial_data, webhook URL).
- **stdout (worker)**: reserved for one final JSON line at game-end (results), written right before exit.
- **stderr (worker)**: regular logs, the orchestrator can pipe them to a logfile or `/dev/null`.

No sockets, no named pipes, no shared files. Cross-platform via `posix_spawn` on Linux / `CreateProcess` on Windows.

### What game devs customize vs what the engine ships

`uldum_server` and `uldum_worker` are **game-agnostic binaries**. They ship from this repo and never need to be forked — game devs deploy them as-is and call into the HTTP API from their own backend.

All game-specific concerns — login, identity vendor (Firebase / Apple / Google), profiles, persistence, payments, leaderboards, anti-cheat business logic — live entirely in the game backend, which the game dev writes in any language they want. The engine doesn't know about identity vendors and doesn't need to.

### LAN / dev / single-player path

Skips the orchestrator entirely. `uldum_worker` runs standalone (today's `uldum_server` behavior, just renamed), accepts any client because there's no tokens table. This is Phase 23's path, unchanged.

## Future Work

- **Delta compression**: only send entities that changed since last acknowledged snapshot
- **Transport fallback**: TCP / WebSocket-over-TLS for the ~5-10% of sessions on restricted networks that drop UDP (see [design.md](design.md) Deferred / Future Work)
- **Transport encryption**: DTLS or QUIC for on-wire confidentiality + integrity (deferred — see [design.md](design.md))
- **Protocol-version handshake**: clean rejection on stale-client / stale-server mismatches (deferred until shipped clients are out-of-sync with running servers)
- **Bandwidth optimization**: variable send rate, priority-based entity updates
