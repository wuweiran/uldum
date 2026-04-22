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
| `S_SPAWN` | 0x50 | reliable | `u32 entity_id, u32 type_hash, u8 owner, f32 x, f32 y, f32 facing` |
| `S_DESTROY` | 0x51 | reliable | `u32 entity_id` |
| `S_STATE` | 0x52 | unreliable | `u32 tick, u16 count, EntityState[]` — see below |
| `S_UPDATE` | 0x53 | reliable | on-change attribute / state / ability delta |
| `S_SOUND` | 0x54 | unreliable | `u16 path_len, char[] path, f32 x, f32 y, f32 z` |
| **Playing — session events** | | | |
| `S_START` | 0x60 | reliable | (empty) — all players loaded, game begins |
| `S_END` | 0x61 | reliable | `u32 winner_id, u16 stats_len, char[] stats_json` |
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

- Server sends `S_STATE` every tick (32 Hz) containing all entities visible to that player
- Server sends `S_SPAWN` / `S_DESTROY` when entities are created/removed
- Server filters by fog of war — enemies outside the player's vision are not included in S_STATE

### Client Interpolation

The client does not run `Simulation.tick()`. It buffers the two most recent `S_STATE` snapshots and interpolates between them based on wall-clock time:

```
render_time = current_time - one_tick_duration
alpha = (render_time - snapshot_old.time) / (snapshot_new.time - snapshot_old.time)
position = lerp(snapshot_old.position, snapshot_new.position, alpha)
```

This means the client always renders one tick behind the server (~31ms at 32 Hz). Visually imperceptible, but guarantees smooth movement even with network jitter.

### Fog of War

Each client computes its own fog of war locally from the entities the server sends it. Since the server only sends visible entities (fog-filtered), the client's fog naturally matches — tiles with no visible allied units stay dark.

The client still runs `FogOfWar::update()` and `update_visual()` locally for smooth fog transitions.

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

The dedicated `uldum_server` auto-commits Start the moment all slots are filled (no UI to press a button). Multi-lobby support on the server process is deferred; see the "Deferred / Future Work" section in [design.md](design.md).

## Reconnect

When a client disconnects, the server keeps their player state (units, buildings) alive:

1. Disconnect detected → player moved to disconnected list, `on_player_disconnected` Lua event fires
2. If `manifest.reconnect.pause` is true, simulation pauses for all players
3. Timer counts down (`manifest.reconnect.timeout`, default 60s)
4. If client reconnects (sends C_JOIN again) → gets S_WELCOME + full S_SPAWN burst + S_START, game resumes
5. If timeout expires → `on_player_dropped` Lua event fires, game resumes, map script decides what happens to the player's units

Manifest config:
```json
"reconnect": {
    "timeout": 60,
    "pause": true
}
```

### EndGame

```lua
-- Map script calls this when a win condition is met
EndGame(0, '{"kills": 15, "time": 302}')
```

The engine fires `on_game_end` event (for triggers), then broadcasts `S_END` with the winner ID and a JSON stats string. The stats format is entirely map-defined — the engine just passes it through.

- Server can serve one game at a time (multi-game deferred)

## Future Work

- **Delta compression**: only send entities that changed since last acknowledged snapshot
- **Transport fallback**: TCP or QUIC for environments that block UDP
- **Bandwidth optimization**: variable send rate, priority-based entity updates
