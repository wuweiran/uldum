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

### Client → Server

| Type | ID | Reliability | Content |
|---|---|---|---|
| `C_JOIN` | 0x01 | reliable | `u32 map_hash` — verify same map |
| `C_ORDER` | 0x02 | reliable | `u8 order_type, u32 target_id, f32 x, f32 y, u8 unit_count, u32[] unit_ids` |
| `C_LEAVE` | 0x03 | reliable | (empty) |

`C_ORDER` is a serialized `GameCommand`. The server validates ownership (the commanding player must own the units) before executing.

### Server → Client

| Type | ID | Reliability | Content |
|---|---|---|---|
| `S_WELCOME` | 0x10 | reliable | `u32 player_id, u32 player_count, u32 tick_rate` |
| `S_REJECT` | 0x11 | reliable | `u8 reason` (0=full, 1=wrong map, 2=started) |
| `S_SPAWN` | 0x20 | reliable | `u32 entity_id, u32 type_hash, u8 owner, f32 x, f32 y, f32 facing` |
| `S_DESTROY` | 0x21 | reliable | `u32 entity_id` |
| `S_STATE` | 0x30 | unreliable | `u32 tick, u16 count, EntityState[]` — see below |
| `S_SOUND` | 0x31 | unreliable | `u16 path_len, char[] path, f32 x, f32 y, f32 z` |
| `S_START` | 0x40 | reliable | (empty) — all players connected, game begins |
| `S_END` | 0x41 | reliable | `u32 winner_id, u16 stats_len, char[] stats_json` |

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
Host starts (--host)      → listening, simulation paused
Clients connect (--connect) → C_JOIN → S_WELCOME → S_SPAWN burst
All expected players joined → S_START broadcast → simulation begins ticking
  ... gameplay ...
Lua calls EndGame(winner, stats) → S_END broadcast → session over
```

The host reads expected player count from the map manifest. Simulation does not tick until all players connect. Offline mode skips all of this — simulation starts immediately.

### EndGame

```lua
-- Map script calls this when a win condition is met
EndGame(0, '{"kills": 15, "time": 302}')
```

The engine fires `on_game_end` event (for triggers), then broadcasts `S_END` with the winner ID and a JSON stats string. The stats format is entirely map-defined — the engine just passes it through.

## Future Work

- **Delta compression**: only send entities that changed since last acknowledged snapshot
- **Transport fallback**: TCP or QUIC for environments that block UDP
- **Reconnect** (Phase 13d): timeout window, full state catch-up, configurable pause
- **Dedicated server**: headless mode with no renderer (Phase 15)
- **Bandwidth optimization**: variable send rate, priority-based entity updates
