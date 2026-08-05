#pragma once

#include "core/types.h"
#include "simulation/command.h"
#include "network/lobby.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

namespace uldum::network {

// ── Message type IDs ──────────────────────────────────────────────────────

// Numbering convention: top nibble is the category, so a stray `0x5X` in
// a packet log is immediately readable as "server entity sync". Gaps inside
// each block leave room for future message types without re-shuffling.
//
//   0x0X  client → server, lobby
//   0x1X  client → server, playing
//   0x2X  client → server, any phase
//   0x4X  server → client, lobby / pre-game
//   0x5X  server → client, playing / entity sync — organized by TIER
//   0x6X  server → client, playing / session events
//   0x7X  server → client, HUD sync (16c-v)
//   0x8X  server → client, audio (script-initiated)
//   0x9X  server → client, environment
//
// ── Entity-sync tier taxonomy (the 0x5X block) ────────────────────────────
// Every server→client entity message belongs to exactly one tier:
//   MATERIALIZE    entity enters client knowledge (birth / fog-reveal / join).
//                  Reliable. S_SPAWN / S_SHOW (slim identity) + an S_COLD BATCH right after.
//   HOT            per-tick position/state while in sight. Unreliable (self-heals next tick).
//                  S_UNIT_STATE (carries health → death) / S_PROJECTILE_STATE.
//   COLD           cold state. Reliable. One grouped envelope S_COLD, count-prefixed:
//                  1 record on-change, N records = the materialize batch.
//   EVENT          discrete one-shots while in sight. Reliable. S_PROJECTILE_DYING /
//                  S_SOUND / S_EFFECT_*.
//   DEMATERIALIZE  entity leaves client knowledge. Reliable. S_HIDE (still in world) / S_DESTROY (gone).
//
// INVARIANT: the MATERIALIZE S_COLD batch is exactly every COLD record the entity currently has
// (health→death, states, abilities, cooldowns, attrs, current anim). COLD applies one record;
// MATERIALIZE applies the set. Same records, same client apply. Cold state survives a fog round-trip,
// and death is just the Health record (no "dead" component is ever replicated).
enum class MsgType : u8 {
    // Sentinel for empty / corrupted packets. No real message uses 0.
    Invalid         = 0x00,

    // ── Client → Server ──

    // Lobby
    C_JOIN          = 0x01,
    C_CLAIM_SLOT    = 0x02,
    C_RELEASE_SLOT  = 0x03,
    C_LOAD_DONE     = 0x04,

    // Playing
    C_ORDER         = 0x10,
    C_NODE_EVENT    = 0x11,   // client forwards a HUD atom event (button press, etc.)

    // Any phase
    C_LEAVE         = 0x20,

    // ── Server → Client ──

    // Lobby / pre-game
    S_REJECT        = 0x40,
    S_LOBBY_ASSIGN  = 0x41,   // tells a newly-joined peer its peer_id
    S_LOBBY_STATE   = 0x42,   // full lobby snapshot (broadcast on change)
    S_LOBBY_COMMIT  = 0x43,   // host locked the lobby — start loading the map
    S_WELCOME       = 0x44,   // bridge: final player_id, sent at end of Loading

    // Playing — entity sync (0x5X), grouped by TIER (see taxonomy above).

    // MATERIALIZE (reliable): entity enters client knowledge.
    // S_SPAWN/S_SHOW carry SLIM IDENTITY only. Cold state (health→death, states,
    // abilities, cooldowns, attrs, the current script anim) rides an S_COLD BATCH
    // sent right after, for every stateful category — that batch IS the snapshot.
    S_SPAWN         = 0x50,   // BORN into the world → client materializes + plays birth clip
    S_SHOW          = 0x51,   // already-alive entity ENTERS live sight → materialize at state, NO birth

    // HOT (unreliable, self-healing): per-tick state while in sight.
    S_UNIT_STATE       = 0x52, // rich: pos/facing/flags/health/states — UNITS only
    S_PROJECTILE_STATE = 0x53, // lean: pos/facing — PROJECTILES only

    // COLD (reliable): cold state. ONE grouped envelope (see ColdKind), carrying a
    // COUNT-PREFIXED batch of records: 1 record = on-change while in sight; N
    // records = the MATERIALIZE batch after S_SPAWN/S_SHOW. Death is NOT here — it
    // rides the Health record (health < threshold ⇒ dead) + the HOT health field.
    S_COLD          = 0x54,

    // EVENT (reliable): discrete one-shots while in sight.
    S_PROJECTILE_DYING = 0x55, // projectile entered dying state — play death clip once
    S_SOUND            = 0x56, // positional world SFX
    S_EFFECT_CREATE    = 0x57, // CreateEffect / CreateEffectOnUnit (with handle)
    S_EFFECT_DESTROY   = 0x58, // DestroyEffect (handle)

    // DEMATERIALIZE (reliable): entity leaves client knowledge.
    S_HIDE          = 0x59,   // leaves live sight (still exists) → client: static→snapshot, mobile→drop
    S_DESTROY       = 0x5A,   // client must forget this materialization (removed, decayed, or ShowUnit(false))

    // Playing — session events
    S_START         = 0x60,   // all players loaded, game begins
    S_END           = 0x61,   // game over, includes results
    S_PAUSE_STATE   = 0x62,   // mid-game: list of disconnected players + timers
    S_SCENE_SWITCH  = 0x63,   // host requests a scene swap; clients tear down
                              // local state and ack via C_LOAD_DONE

    // Playing — scripted-camera commands. Target-based pose
    // (target xyz + distance + pitch + yaw). Each is targeted at a
    // single recipient; the host sends per-player. Lua scripts produce
    // these via CameraSetupApply / CameraSetTargetPosition / etc.
    // Pitch / yaw on the wire are radians (matches Camera's internal
    // storage); Lua's degree values are converted at the host.
    S_CAMERA_APPLY_SETUP            = 0x64,  // target xyz, distance, pitch_rad, yaw_rad, duration: 7 × f32
    S_CAMERA_SET_TARGET_POSITION    = 0x65,  // x, y, z, duration: 4 × f32
    S_CAMERA_SET_SOURCE_DISTANCE    = 0x66,  // distance, duration: 2 × f32
    S_CAMERA_SHAKE                  = 0x67,  // intensity, duration: 2 × f32
    S_CAMERA_SET_TARGET_CONTROLLER  = 0x68,  // entity_id: u32 (UINT32_MAX = unlock)

    // Playing — scripted selection lock. SetControlledUnit(unit) is routed to the
    // unit's owner only (a P1 client never inherits P0's hero) and replayed on join
    // (main() runs before a client finishes loading).
    S_SET_CONTROLLED_UNIT           = 0x69,  // entity_id: u32 (UINT32_MAX = clear)

    // Playing — HUD sync
    S_HUD_CREATE_NODE         = 0x70, // template instantiation + placement
    S_HUD_DESTROY_NODE        = 0x71,
    S_HUD_SET_LABEL_TEXT      = 0x72,
    S_HUD_SET_BAR_FILL        = 0x73,
    S_HUD_SET_NODE_VISIBLE    = 0x74,
    S_HUD_SET_IMAGE_SOURCE    = 0x75,
    S_HUD_SET_BUTTON_ENABLED  = 0x76,
    S_HUD_CREATE_TEXT_TAG     = 0x77,
    S_HUD_DISPLAY_MESSAGE     = 0x78, // queue one line into composites.display_message
    S_HUD_DESTROY_TEXT_TAG    = 0x79, // remove a permanent text tag by shared ECS id
    S_HUD_ACTION_BAR_SET_SLOT = 0x7A, // manual-mode slot→ability binding (empty ability = clear)

    // Playing — audio (script-initiated). Sim sound effects from the
    // combat / ability systems use S_SOUND directly; these mirror the
    // Lua-level audio API surface so script-driven cues reach all
    // clients (UI sounds, music start/stop, ambient loops, …).
    S_SOUND_PLAY_2D    = 0x80,
    S_MUSIC_PLAY       = 0x81,
    S_MUSIC_STOP       = 0x82,
    S_AMBIENT_START    = 0x83,
    S_AMBIENT_STOP     = 0x84,

    // Playing — environment.
    S_SET_SUN_DIRECTION = 0x90,
};

// Event kinds for C_NODE_EVENT. Keep numeric so unknown kinds can be
// ignored / logged without a string parse.
enum class NodeEventKind : u8 {
    ButtonPressed = 0,
};

enum class RejectReason : u8 {
    Full         = 0,
    WrongMap     = 1,
    Started      = 2,
    Unauthorized = 3,
};

// ── Binary read/write helpers ─────────────────────────────────────────────
//
// The wire format writes multi-byte scalars (u16/u32/f32) as raw
// native-memory bytes and memcpy's them back on read. That round-trips
// correctly only between little-endian, IEEE-754 hosts. Both current
// targets (Windows x64, Android arm64/x64) satisfy this; the assert makes
// the assumption a hard compile error if a big-endian target is ever added,
// rather than a silent wire-format mismatch.
static_assert(std::endian::native == std::endian::little,
              "protocol.h assumes a little-endian host; add explicit byte-order "
              "conversion to ByteWriter/ByteReader before targeting big-endian.");

class ByteWriter {
public:
    void write_u8(u8 v)   { m_buf.push_back(v); }
    void write_u16(u16 v) { append(&v, 2); }
    void write_u32(u32 v) { append(&v, 4); }
    void write_f32(f32 v) { append(&v, 4); }
    void write_vec3(glm::vec3 v) { write_f32(v.x); write_f32(v.y); write_f32(v.z); }
    void write_string(std::string_view s) {
        write_u16(static_cast<u16>(s.size()));
        m_buf.insert(m_buf.end(), s.begin(), s.end());
    }
    void write_bool(bool v) { write_u8(v ? 1 : 0); }
    void write_bytes(const u8* p, size_t n) { m_buf.insert(m_buf.end(), p, p + n); }

    const std::vector<u8>& data() const { return m_buf; }
    std::vector<u8>& data() { return m_buf; }

private:
    void append(const void* p, size_t n) {
        auto* b = static_cast<const u8*>(p);
        m_buf.insert(m_buf.end(), b, b + n);
    }
    std::vector<u8> m_buf;
};

class ByteReader {
public:
    ByteReader(std::span<const u8> data) : m_data(data) {}

    bool has(size_t n) const { return m_pos + n <= m_data.size(); }
    // Sticky truncation flag — set the moment any read would go OOB.
    // Subsequent reads silently return zero/empty so handlers walking
    // a multi-field payload don't have to gate every line individually.
    // Callers that care explicitly check ok() once before acting on
    // the decoded values; most just rely on zero defaults being inert.
    bool ok() const { return !m_truncated; }

    u8  read_u8()  { u8 v = 0;  read(&v, 1); return v; }
    u16 read_u16() { u16 v = 0; read(&v, 2); return v; }
    u32 read_u32() { u32 v = 0; read(&v, 4); return v; }
    f32 read_f32() { f32 v = 0; read(&v, 4); return v; }
    glm::vec3 read_vec3() { return {read_f32(), read_f32(), read_f32()}; }
    std::string read_string() {
        // Length-prefixed string. A hostile sender can claim any length
        // up to 65535; clamp to the bytes actually left in the buffer
        // and flag the packet truncated. The handler still gets a
        // string of the wrong length, but no OOB read and no UB.
        u16 len = read_u16();
        if (m_truncated) return {};
        const size_t avail = m_data.size() - m_pos;
        if (len > avail) {
            m_truncated = true;
            len = static_cast<u16>(avail);
        }
        std::string s(m_data.begin() + m_pos, m_data.begin() + m_pos + len);
        m_pos += len;
        return s;
    }
    bool read_bool() { return read_u8() != 0; }
    void read_bytes(u8* p, size_t n) { read(p, n); }

private:
    void read(void* p, size_t n) {
        if (m_pos + n > m_data.size()) {
            // Underflow: leave the output buffer at its caller-provided
            // initial value (the public read_* helpers zero-initialize
            // before calling this). Set the sticky flag so ok() returns
            // false. Advance m_pos to data.size() so further reads also
            // short-circuit through the same path.
            m_truncated = true;
            m_pos = m_data.size();
            return;
        }
        std::memcpy(p, m_data.data() + m_pos, n);
        m_pos += n;
    }
    std::span<const u8> m_data;
    size_t m_pos = 0;
    bool   m_truncated = false;
};

// ── Message builders ──────────────────────────────────────────────────────

// Returns the message type from a raw packet's first byte. Returns
// MsgType::Invalid for an empty packet (defensive — every real
// packet starts with a type byte, but a hostile / corrupted send
// could deliver zero bytes).
inline MsgType peek_type(std::span<const u8> data) {
    if (data.empty()) return MsgType::Invalid;
    return static_cast<MsgType>(data[0]);
}

// ── Client → Server ──────────────────────────────────────────────────────

inline std::vector<u8> build_join(const std::array<u8, 32>& map_hash,
                                  std::span<const u8> auth_token,
                                  std::string_view player_name) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::C_JOIN));
    w.write_bytes(map_hash.data(), map_hash.size());
    // Auth token: opaque bytes, validated by the worker's auth-on-join
    // callback. Empty = LAN / dev (worker with no token table accepts all).
    w.write_u16(static_cast<u16>(auth_token.size()));
    if (!auth_token.empty()) {
        w.write_bytes(auth_token.data(), auth_token.size());
    }
    w.write_string(player_name);
    return std::move(w.data());
}

inline std::vector<u8> build_order(const simulation::GameCommand& cmd) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::C_ORDER));
    w.write_bool(cmd.queued);
    w.write_u8(static_cast<u8>(cmd.units.size()));
    for (auto& u : cmd.units) w.write_u32(u.id);

    std::visit([&](auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        w.write_u16(static_cast<u16>(T::ID));   // stable per-order wire tag
        if constexpr (std::is_same_v<T, simulation::orders::Move>) {
            w.write_vec3(payload.target);
            w.write_u32(payload.target_widget.id);
            w.write_f32(payload.range);
        } else if constexpr (std::is_same_v<T, simulation::orders::Attack>) {
            w.write_vec3(payload.target);
            w.write_u32(payload.target_widget.id);
        } else if constexpr (std::is_same_v<T, simulation::orders::Stop>) {
            // no payload
        } else if constexpr (std::is_same_v<T, simulation::orders::HoldPosition>) {
            // no payload
        } else if constexpr (std::is_same_v<T, simulation::orders::Patrol>) {
            w.write_u8(static_cast<u8>(payload.waypoints.size()));
            for (auto& wp : payload.waypoints) w.write_vec3(wp);
        } else if constexpr (std::is_same_v<T, simulation::orders::Cast>) {
            w.write_string(payload.ability_id);
            w.write_u32(payload.target_unit.id);
            w.write_vec3(payload.target_pos);
            w.write_u32(payload.source_item.id);   // item-slot cast → GetTriggerItem + charge spend
        } else if constexpr (std::is_same_v<T, simulation::orders::Build>) {
            w.write_string(payload.building_type_id);
            w.write_vec3(payload.pos);
        } else if constexpr (std::is_same_v<T, simulation::orders::PickupItem>) {
            w.write_u32(payload.item.id);
        } else if constexpr (std::is_same_v<T, simulation::orders::DropItem>) {
            w.write_u32(payload.item.id);
            w.write_vec3(payload.pos);
        } else if constexpr (std::is_same_v<T, simulation::orders::SwapInventorySlot>) {
            w.write_u32(static_cast<u32>(payload.slot_a));
            w.write_u32(static_cast<u32>(payload.slot_b));
        } else if constexpr (std::is_same_v<T, simulation::orders::MoveDirection>) {
            w.write_f32(payload.dir.x);
            w.write_f32(payload.dir.y);
        }
    }, cmd.order);

    return std::move(w.data());
}

inline std::vector<u8> build_leave() {
    return {static_cast<u8>(MsgType::C_LEAVE)};
}

// C_NODE_EVENT — client reports a HUD atom event (e.g., button pressed)
// back to the host so server-side triggers can fire. Fire-and-forget,
// reliable delivery expected.
inline std::vector<u8> build_node_event(std::string_view node_id, NodeEventKind kind) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::C_NODE_EVENT));
    w.write_string(node_id);
    w.write_u8(static_cast<u8>(kind));
    return std::move(w.data());
}

// ── Server → Client ──────────────────────────────────────────────────────

inline std::vector<u8> build_welcome(u32 player_id, u32 player_count, u32 tick_rate,
                                     u32 placement_count) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_WELCOME));
    w.write_u32(player_id);
    w.write_u32(player_count);
    w.write_u32(tick_rate);
    w.write_u32(placement_count);
    return std::move(w.data());
}

inline std::vector<u8> build_reject(RejectReason reason) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_REJECT));
    w.write_u8(static_cast<u8>(reason));
    return std::move(w.data());
}

// `variation` selects the model for entities whose type def lists multiple
// (destructables/doodads: `def.models[variation % N]`). The client resolves the
// model itself from `type_id + variation` — it has the full type registry — so
// the wire carries 1 byte, not a model string. Projectiles are the one exception
// (not in any registry): for them the host puts the model PATH in the `type_id`
// field (projectiles are sentinel-dispatched client-side), and `variation` is
// unused. Units/items ignore `variation` (single model from the type).
//
// SLIM IDENTITY ONLY. All cold/render-sticky state (health→death, states,
// abilities, cooldowns, attrs, current script anim) rides the S_COLD BATCH sent
// right after this — one mechanism for materialize, on-change, and re-show.
inline std::vector<u8> build_spawn(u32 entity_id, std::string_view type_id,
                                    u8 owner, f32 x, f32 y, f32 facing,
                                    bool newly_created = false,
                                    u8 variation = 0) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_SPAWN));
    w.write_u32(entity_id);
    w.write_string(type_id);
    w.write_u8(owner);
    w.write_f32(x);
    w.write_f32(y);
    w.write_f32(facing);
    w.write_bool(newly_created);
    w.write_u8(variation);
    return std::move(w.data());
}

inline std::vector<u8> build_destroy(u32 entity_id) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_DESTROY));
    w.write_u32(entity_id);
    return std::move(w.data());
}

// S_SHOW — an already-alive entity entered the peer's live sight. Same payload
// as S_SPAWN (slim identity) so the client can build it if it doesn't have it,
// but semantically "show, don't birth": the client comes up at current state,
// no birth clip. (Distinct opcode from S_SPAWN so the client never confuses
// re-entry with birth.) Cold state follows in the S_COLD batch.
inline std::vector<u8> build_show(u32 entity_id, std::string_view type_id,
                                  u8 owner, f32 x, f32 y, f32 facing,
                                  u8 variation = 0) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_SHOW));
    w.write_u32(entity_id);
    w.write_string(type_id);
    w.write_u8(owner);
    w.write_f32(x);
    w.write_f32(y);
    w.write_f32(facing);
    w.write_u8(variation);
    return std::move(w.data());
}

// S_HIDE — entity left the peer's live sight (still exists in the world). The
// client decides: static → snapshot & freeze; mobile → drop. Id-only.
inline std::vector<u8> build_hide(u32 entity_id) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_HIDE));
    w.write_u32(entity_id);
    return std::move(w.data());
}

// Per-unit record in the per-tick S_UNIT_STATE snapshot — the HOT tier for
// UNITS. Carries ONLY fields that change ~every tick, all engine-native (no
// derived fractions, no quantization): position, facing, runtime flags, and the
// AUTHENTIC current values of health + map-defined states. The cold tier
// (max / regen / abilities / attributes / cooldowns) rides S_COLD, not here.
// Statics (destructables/doodads/items) are NOT in this packet at all — they
// don't move or regen; their lifecycle rides S_SPAWN/S_SHOW/S_HIDE/S_DESTROY.
//
// States are KEYLESS: the client owns the same frozen, sorted per-unit state
// schema (StateBlock seeded from the unit type at spawn — Lua can't add keys),
// so the wire sends only the current values in that order. `health_current` is
// present only when the has_health flag (0x20) is set. `target_id` is present
// only when the attacking flag (0x02) is set.
struct UnitState {
    u32 entity_id;
    f32 x, y, z;
    f32 facing;
    u8  flags;       // 0x01 moving, 0x02 attacking, 0x04 casting, 0x20 has_health (death is derived from the health field)
    f32 health_current;              // authentic Health.current; valid iff (flags & 0x20)
    u32 target_id;                   // valid iff (flags & 0x02)
    std::vector<f32> state_currents; // authentic StateValue.current, in the unit's sorted state-key order
};

inline std::vector<u8> build_unit_state(u32 tick, const std::vector<UnitState>& units) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_UNIT_STATE));
    w.write_u32(tick);
    w.write_u16(static_cast<u16>(units.size()));
    for (auto& e : units) {
        w.write_u32(e.entity_id);
        w.write_f32(e.x);
        w.write_f32(e.y);
        w.write_f32(e.z);
        w.write_f32(e.facing);
        w.write_u8(e.flags);
        if (e.flags & 0x20) w.write_f32(e.health_current);
        if (e.flags & 0x02) w.write_u32(e.target_id);
        w.write_u8(static_cast<u8>(std::min<usize>(e.state_currents.size(), 255)));
        usize n = std::min<usize>(e.state_currents.size(), 255);
        for (usize i = 0; i < n; ++i) w.write_f32(e.state_currents[i]);
    }
    return std::move(w.data());
}

// Per-projectile record in the per-tick S_PROJECTILE_STATE snapshot — the LEAN
// HOT tier. Projectiles move every tick but have no health / states / combat, so
// they carry position + facing only. Separate packet from units so neither pays
// for the other's fields.
struct ProjectileState {
    u32 entity_id;
    f32 x, y, z;
    f32 facing;
};

inline std::vector<u8> build_projectile_state(u32 tick, const std::vector<ProjectileState>& projs) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_PROJECTILE_STATE));
    w.write_u32(tick);
    w.write_u16(static_cast<u16>(projs.size()));
    for (auto& p : projs) {
        w.write_u32(p.entity_id);
        w.write_f32(p.x);
        w.write_f32(p.y);
        w.write_f32(p.z);
        w.write_f32(p.facing);
    }
    return std::move(w.data());
}

inline std::vector<u8> build_sound(std::string_view path, glm::vec3 pos) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_SOUND));
    w.write_string(path);
    w.write_vec3(pos);
    return std::move(w.data());
}

// Non-positional one-shot SFX (UI cues, voice lines) — no world pos
// because the sound doesn't attenuate by distance.
inline std::vector<u8> build_sound_2d(std::string_view path) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_SOUND_PLAY_2D));
    w.write_string(path);
    return std::move(w.data());
}

// Music stream control. PLAY crossfades from the current track at the
// authored `fade_in` (default 1.0s on the server). STOP fades the
// current track out at `fade_out` (default 1.0s).
inline std::vector<u8> build_music_play(std::string_view path, f32 fade_in) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_MUSIC_PLAY));
    w.write_string(path);
    w.write_f32(fade_in);
    return std::move(w.data());
}

inline std::vector<u8> build_music_stop(f32 fade_out) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_MUSIC_STOP));
    w.write_f32(fade_out);
    return std::move(w.data());
}

// Looping positional ambient. `handle` is host-assigned and used by
// the matching STOP. Clients keep their own handle → audio-engine
// id map so the same handle stops the right loop on every peer.
inline std::vector<u8> build_ambient_start(u32 handle, std::string_view path, f32 x, f32 y) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_AMBIENT_START));
    w.write_u32(handle);
    w.write_string(path);
    w.write_f32(x);
    w.write_f32(y);
    return std::move(w.data());
}

inline std::vector<u8> build_ambient_stop(u32 handle, f32 fade_out) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_AMBIENT_STOP));
    w.write_u32(handle);
    w.write_f32(fade_out);
    return std::move(w.data());
}

inline std::vector<u8> build_set_sun_direction(f32 x, f32 y, f32 z) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_SET_SUN_DIRECTION));
    w.write_f32(x);
    w.write_f32(y);
    w.write_f32(z);
    return std::move(w.data());
}

// CreateEffect — server-assigned handle + an effect payload. Client
// maps the server handle to its local instance so a later
// S_EFFECT_DESTROY can find it.
struct EffectCreateData {
    u32              server_id = 0;
    std::string      name;
    simulation::Unit entity;
    glm::vec3        pos{0};
    std::string      attach_point;
};

inline std::vector<u8> build_effect_create(u32 server_id, std::string_view name,
                                            simulation::Unit entity, glm::vec3 pos,
                                            std::string_view attach_point) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_EFFECT_CREATE));
    w.write_u32(server_id);
    w.write_string(name);
    w.write_u32(entity.id);
    w.write_vec3(pos);
    w.write_string(attach_point);
    return std::move(w.data());
}

inline EffectCreateData parse_effect_create(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    EffectCreateData e;
    e.server_id         = r.read_u32();
    e.name              = r.read_string();
    e.entity.id    = r.read_u32();
    e.pos          = r.read_vec3();
    e.attach_point      = r.read_string();
    return e;
}

inline std::vector<u8> build_effect_destroy(u32 server_id) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_EFFECT_DESTROY));
    w.write_u32(server_id);
    return std::move(w.data());
}

inline u32 parse_effect_destroy(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    return r.read_u32();
}

// PROJECTILE_DYING — broadcast when a projectile enters its dying
// state on the server (gameplay over, death animation about to play).
// The entity itself is still alive on both sides; this just tells the
// client to queue the model's "death" clip. The follow-up S_DESTROY
// arrives ~0.6s later when the server tears the entity down.
inline std::vector<u8> build_projectile_dying(u32 entity_id) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_PROJECTILE_DYING));
    w.write_u32(entity_id);
    return std::move(w.data());
}

inline u32 parse_projectile_dying(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    return r.read_u32();
}

// ── Client-side deserialization helpers ───────────────────────────────────

inline std::optional<simulation::GameCommand> parse_order(std::span<const u8> data, simulation::Player player) {
    ByteReader r(data);
    r.read_u8();  // skip MsgType

    simulation::GameCommand cmd;
    cmd.player = player;
    cmd.queued = r.read_bool();
    u8 unit_count = r.read_u8();
    for (u8 i = 0; i < unit_count; ++i) {
        cmd.units.push_back(simulation::Unit{r.read_u32()});
    }

    switch (static_cast<simulation::orders::OrderId>(r.read_u16())) {
    using enum simulation::orders::OrderId;
    case Move: {
        glm::vec3 t = r.read_vec3();
        simulation::Unit tu{r.read_u32()};
        f32 range = r.read_f32();
        cmd.order = simulation::orders::Move{t, tu, range};
        break;
    }
    case Attack: {
        glm::vec3 pt = r.read_vec3();
        simulation::Unit w{r.read_u32()};
        cmd.order = simulation::orders::Attack{pt, w};
        break;
    }
    case Stop: cmd.order = simulation::orders::Stop{}; break;
    case HoldPosition: cmd.order = simulation::orders::HoldPosition{}; break;
    case Patrol: {
        simulation::orders::Patrol p;
        u8 wpc = r.read_u8();
        for (u8 i = 0; i < wpc; ++i) p.waypoints.push_back(r.read_vec3());
        cmd.order = std::move(p);
        break;
    }
    case Cast: {
        std::string ability = r.read_string();
        simulation::Unit tu{r.read_u32()};
        glm::vec3 tp = r.read_vec3();
        simulation::Item src{r.read_u32()};   // source_item (0/INVALID for non-item casts)
        cmd.order = simulation::orders::Cast{std::move(ability), tu, tp, src};
        break;
    }
    case Build: {
        std::string bt = r.read_string();
        glm::vec3 p = r.read_vec3();
        cmd.order = simulation::orders::Build{std::move(bt), p};
        break;
    }
    case PickupItem: {
        cmd.order = simulation::orders::PickupItem{
            simulation::Item{r.read_u32()}};
        break;
    }
    case DropItem: {
        simulation::Item item{r.read_u32()};
        glm::vec3 p = r.read_vec3();
        cmd.order = simulation::orders::DropItem{item, p};
        break;
    }
    case SwapInventorySlot: {
        i32 a = static_cast<i32>(r.read_u32());
        i32 b = static_cast<i32>(r.read_u32());
        cmd.order = simulation::orders::SwapInventorySlot{a, b};
        break;
    }
    case MoveDirection: {
        f32 dx = r.read_f32();
        f32 dy = r.read_f32();
        cmd.order = simulation::orders::MoveDirection{{dx, dy}};
        break;
    }
    default:
        // Unknown order ID — a corrupt or hostile C_ORDER, or a retired
        // tag. Reject rather than fall through to a variant alternative.
        return std::nullopt;
    }
    // Drop packets that ran off the end mid-decode (truncated / malformed).
    if (!r.ok()) return std::nullopt;
    return cmd;
}

struct WelcomeData {
    u32 player_id;
    u32 player_count;
    u32 tick_rate;
    u32 placement_count;
};

inline WelcomeData parse_welcome(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();  // skip MsgType
    WelcomeData w;
    w.player_id = r.read_u32();
    w.player_count = r.read_u32();
    w.tick_rate = r.read_u32();
    w.placement_count = r.read_u32();
    return w;
}

struct SpawnData {
    u32 entity_id;
    std::string type_id;   // registry type id; for projectiles, the model path
    u8 owner;
    f32 x, y, facing;
    bool newly_created = false;
    u8 variation = 0;      // model selector for multi-model types (destructable/doodad)
};

inline SpawnData parse_spawn(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();  // skip MsgType
    SpawnData s;
    s.entity_id     = r.read_u32();
    s.type_id       = r.read_string();
    s.owner         = r.read_u8();
    s.x             = r.read_f32();
    s.y             = r.read_f32();
    s.facing        = r.read_f32();
    s.newly_created = r.read_bool();
    s.variation     = r.read_u8();
    return s;
}

inline u32 parse_destroy(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    return r.read_u32();
}

// S_SHOW reuses SpawnData (newly_created stays false → the client materializes
// without a birth clip). Payload matches build_show (no newly_created field).
inline SpawnData parse_show(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();  // skip MsgType
    SpawnData s;
    s.entity_id  = r.read_u32();
    s.type_id    = r.read_string();
    s.owner      = r.read_u8();
    s.x          = r.read_f32();
    s.y          = r.read_f32();
    s.facing     = r.read_f32();
    s.newly_created = false;   // show ≠ birth
    s.variation  = r.read_u8();
    return s;
}

inline u32 parse_hide(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    return r.read_u32();
}

struct UnitStateData {
    u32 tick;
    std::vector<UnitState> units;
};

inline UnitStateData parse_unit_state(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    UnitStateData s;
    s.tick = r.read_u32();
    u16 count = r.read_u16();
    s.units.resize(count);
    for (u16 i = 0; i < count; ++i) {
        auto& e = s.units[i];
        e.entity_id = r.read_u32();
        e.x = r.read_f32();
        e.y = r.read_f32();
        e.z = r.read_f32();
        e.facing = r.read_f32();
        e.flags = r.read_u8();
        e.health_current = (e.flags & 0x20) ? r.read_f32() : 0.0f;
        e.target_id      = (e.flags & 0x02) ? r.read_u32() : UINT32_MAX;
        u8 nstates = r.read_u8();
        e.state_currents.reserve(nstates);
        for (u8 j = 0; j < nstates; ++j) e.state_currents.push_back(r.read_f32());
    }
    return s;
}

struct ProjectileStateData {
    u32 tick;
    std::vector<ProjectileState> projectiles;
};

inline ProjectileStateData parse_projectile_state(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    ProjectileStateData s;
    s.tick = r.read_u32();
    u16 count = r.read_u16();
    s.projectiles.resize(count);
    for (u16 i = 0; i < count; ++i) {
        auto& p = s.projectiles[i];
        p.entity_id = r.read_u32();
        p.x = r.read_f32();
        p.y = r.read_f32();
        p.z = r.read_f32();
        p.facing = r.read_f32();
    }
    return s;
}

struct SoundData {
    std::string path;
    glm::vec3 pos;
};

inline SoundData parse_sound(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    SoundData s;
    s.path = r.read_string();
    s.pos = r.read_vec3();
    return s;
}

// ── Lobby messages ───────────────────────────────────────────────────────

inline std::vector<u8> build_claim_slot(u32 slot) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::C_CLAIM_SLOT));
    w.write_u32(slot);
    return std::move(w.data());
}

inline std::vector<u8> build_release_slot(u32 slot) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::C_RELEASE_SLOT));
    w.write_u32(slot);
    return std::move(w.data());
}

inline u32 parse_claim_or_release_slot(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    return r.read_u32();
}

inline std::vector<u8> build_load_done() {
    return {static_cast<u8>(MsgType::C_LOAD_DONE)};
}

inline std::vector<u8> build_lobby_commit() {
    return {static_cast<u8>(MsgType::S_LOBBY_COMMIT)};
}

inline std::vector<u8> build_lobby_assign(u32 peer_id) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_LOBBY_ASSIGN));
    w.write_u32(peer_id);
    return std::move(w.data());
}

inline u32 parse_lobby_assign(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    return r.read_u32();
}

inline std::vector<u8> build_lobby_state(const LobbyState& s) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_LOBBY_STATE));
    w.write_string(s.map_path);
    w.write_string(s.map_name);
    w.write_u16(static_cast<u16>(s.slots.size()));
    for (const auto& a : s.slots) {
        w.write_u32(a.team);
        w.write_string(a.color);
        w.write_u8(static_cast<u8>(a.occupant));
        w.write_bool(a.locked);
        w.write_u32(a.peer_id);
        w.write_string(a.display_name);
    }
    return std::move(w.data());
}

inline LobbyState parse_lobby_state(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    LobbyState s;
    s.map_path = r.read_string();
    s.map_name = r.read_string();
    u16 n = r.read_u16();
    s.slots.resize(n);
    for (u16 i = 0; i < n; ++i) {
        auto& a = s.slots[i];
        a.team = r.read_u32();
        a.color = r.read_string();
        a.occupant = static_cast<SlotOccupant>(r.read_u8());
        a.locked = r.read_bool();
        a.peer_id = r.read_u32();
        a.display_name = r.read_string();
    }
    return s;
}

// ── Session messages ─────────────────────────────────────────────────────

inline std::vector<u8> build_start() {
    return {static_cast<u8>(MsgType::S_START)};
}

// Scene-switch request. Host sends to every client when Lua calls
// LoadScene(name); clients tear down local scene state and ack with
// C_LOAD_DONE. Host runs the new scene's main() and bursts entity /
// HUD spawns once everyone has acked.
inline std::vector<u8> build_scene_switch(std::string_view scene_name) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_SCENE_SWITCH));
    w.write_string(scene_name);
    return std::move(w.data());
}

inline std::string parse_scene_switch(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    return r.read_string();
}

// ── Scripted-camera commands ────────────────────────────────────────
// Each command has a fixed-size payload (no strings) and is sent per
// player. Recipient is implied by the transport peer the host sent
// to, so the message itself doesn't carry a player id.

// Full CameraSetup application — every axis tweens at once over
// `duration` seconds (snap if 0). Pitch/yaw in radians on the wire.
inline std::vector<u8> build_camera_apply_setup(f32 tx, f32 ty, f32 tz,
                                                  f32 distance,
                                                  f32 pitch_rad, f32 yaw_rad,
                                                  f32 duration) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_CAMERA_APPLY_SETUP));
    w.write_f32(tx); w.write_f32(ty); w.write_f32(tz);
    w.write_f32(distance);
    w.write_f32(pitch_rad);
    w.write_f32(yaw_rad);
    w.write_f32(duration);
    return std::move(w.data());
}

inline std::vector<u8> build_camera_set_target_position(f32 x, f32 y, f32 z, f32 duration) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_CAMERA_SET_TARGET_POSITION));
    w.write_f32(x); w.write_f32(y); w.write_f32(z);
    w.write_f32(duration);
    return std::move(w.data());
}

inline std::vector<u8> build_camera_set_source_distance(f32 distance, f32 duration) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_CAMERA_SET_SOURCE_DISTANCE));
    w.write_f32(distance);
    w.write_f32(duration);
    return std::move(w.data());
}

inline std::vector<u8> build_camera_shake(f32 intensity, f32 duration) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_CAMERA_SHAKE));
    w.write_f32(intensity);
    w.write_f32(duration);
    return std::move(w.data());
}

inline std::vector<u8> build_camera_set_target_controller(u32 entity_id) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_CAMERA_SET_TARGET_CONTROLLER));
    w.write_u32(entity_id);
    return std::move(w.data());
}

inline std::vector<u8> build_set_controlled_unit(u32 entity_id) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_SET_CONTROLLED_UNIT));
    w.write_u32(entity_id);
    return std::move(w.data());
}

// Mid-game pause snapshot. Host broadcasts to all clients so everyone sees
// the same "Player X disconnected, 57s remaining" dialog. Sent on every
// change (disconnect / reconnect / drop) and once per second while paused
// so clients see the countdown.
struct DisconnectedView {
    u32         player_id;
    std::string display_name;
    f32         seconds_remaining;
};

inline std::vector<u8> build_pause_state(bool paused, const std::vector<DisconnectedView>& list) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_PAUSE_STATE));
    w.write_bool(paused);
    w.write_u16(static_cast<u16>(list.size()));
    for (const auto& d : list) {
        w.write_u32(d.player_id);
        w.write_string(d.display_name);
        w.write_f32(d.seconds_remaining);
    }
    return std::move(w.data());
}

struct PauseState {
    bool                          paused = false;
    std::vector<DisconnectedView> disconnected;
};

inline PauseState parse_pause_state(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    PauseState s;
    s.paused = r.read_bool();
    u16 n = r.read_u16();
    s.disconnected.resize(n);
    for (u16 i = 0; i < n; ++i) {
        auto& d = s.disconnected[i];
        d.player_id         = r.read_u32();
        d.display_name      = r.read_string();
        d.seconds_remaining = r.read_f32();
    }
    return s;
}

// S_END carries the winning team and a Lua-defined stats table serialized as JSON string.
inline std::vector<u8> build_end(u32 winning_team, std::string_view stats_json) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_END));
    w.write_u32(winning_team);
    w.write_string(stats_json);
    return std::move(w.data());
}

struct EndData {
    // Winning team (manifest team index). UINT32_MAX = no winner — a draw, or a
    // session that ended without a result (abandoned / never-started). In a FFA
    // map each player is their own team, so this doubles as the winning player.
    u32 winning_team = UINT32_MAX;
    std::string stats_json;
};

inline EndData parse_end(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    EndData e;
    e.winning_team = r.read_u32();
    e.stats_json = r.read_string();
    return e;
}

// ── COLD tier: on-change cold state + the MATERIALIZE batch (both S_COLD) ─────
//
// A ColdRecord is one unit of cold state. S_COLD is count-prefixed and used two ways:
//   • 1 record  — on-change while the entity is in sight.
//   • N records — the MATERIALIZE batch, sent right after S_SPAWN/S_SHOW, so cold
//                 state (incl. health→death) survives a fog round-trip.
// write_cold_record / read_cold_record is the single wire-layout source of truth;
// apply_cold_record (network.cpp) is the single client apply. Snapshot = apply all;
// on-change = apply one.
enum class ColdKind : u8 {
    // vitals
    Health          = 0,   // current + max (max has no other channel — this IS the max sync)
    State           = 1,   // a map-defined state's current + max (mana, energy, …)
    Attribute       = 2,   // numeric attribute (armor, strength, …)
    StringAttribute = 3,   // string attribute (armor_type, …)
    // abilities
    AbilityAdd      = 4,
    AbilityRemove   = 5,
    AbilityModifier = 6,   // SetAbilityModifier(unit, ability, key, value)
    Cooldown        = 7,   // SetAbilityCooldown / ResetAbilityCooldown
    // identity / status / transform
    Owner           = 8,   // ownership changed
    Status          = 9,   // manual status bits (SetUnitStatus) — snapshot + on-change
    Transform       = 10,  // static teleport/facing (units self-heal via HOT S_UNIT_STATE)
    // items (item-entity cold state)
    ItemCharges     = 11,
    ItemLevel       = 12,
    Inventory       = 13,  // item entered/left a carrier's inventory slot (pickup/drop)
    // render-sticky
    Anim            = 14,  // scripted animation queue (SetUnitAnimation/Queue/Reset); empty list = reset
    Construction    = 15,  // building under-construction state (drives the time-scaled birth clip on the client)
};

struct ColdRecord {
    ColdKind kind;
    std::string key;         // attribute/state/ability id
    f32 value = 0;           // primary scalar (attr value, state current, cooldown secs, health current, …)
    f32 value2 = 0;          // secondary scalar (state/health max)
    std::string str_value;   // string attr value / ability modifier key
    u32 uint_value = 0;      // ability level / owner / status flag / inventory slot
    u32 uint_value2 = 0;     // inventory item_id (UINT32_MAX = clear slot / drop)
    u8 byte_value = 0;       // AbilitySourceKind
    bool bool_value = false; // all-instances mode / status on-off / anim looping
    f32 x = 0, y = 0, z = 0; // inventory drop pos / transform pos
    f32 facing = 0;          // transform facing
    std::vector<std::string> clips;  // Anim: script clip list (empty = reset)
};

// Wire layout of one record: kind byte + kind-specific payload. NO entity_id and
// NO opcode here — the caller (S_COLD single or batch) frames those.
inline void write_cold_record(ByteWriter& w, const ColdRecord& rec) {
    w.write_u8(static_cast<u8>(rec.kind));
    switch (rec.kind) {
    case ColdKind::Health:
        w.write_f32(rec.value); w.write_f32(rec.value2);
        break;
    case ColdKind::State:
        w.write_string(rec.key); w.write_f32(rec.value); w.write_f32(rec.value2);
        break;
    case ColdKind::Attribute:
        w.write_string(rec.key); w.write_f32(rec.value);
        break;
    case ColdKind::StringAttribute:
        w.write_string(rec.key); w.write_string(rec.str_value);
        break;
    case ColdKind::AbilityAdd:
        w.write_string(rec.key); w.write_u32(rec.uint_value); w.write_u8(rec.byte_value);
        break;
    case ColdKind::AbilityRemove:
        w.write_string(rec.key); w.write_u8(rec.byte_value); w.write_bool(rec.bool_value);
        break;
    case ColdKind::AbilityModifier:
        w.write_string(rec.key); w.write_string(rec.str_value); w.write_f32(rec.value);
        break;
    case ColdKind::Cooldown:
        w.write_string(rec.key); w.write_f32(rec.value);
        break;
    case ColdKind::Owner:
        w.write_u8(static_cast<u8>(rec.uint_value));
        break;
    case ColdKind::Status:
        w.write_u32(rec.uint_value); w.write_bool(rec.bool_value);
        break;
    case ColdKind::Transform:
        w.write_f32(rec.x); w.write_f32(rec.y); w.write_f32(rec.z); w.write_f32(rec.facing);
        break;
    case ColdKind::ItemCharges:
    case ColdKind::ItemLevel:
        w.write_u32(rec.uint_value);
        break;
    case ColdKind::Inventory:
        w.write_u32(rec.uint_value); w.write_u32(rec.uint_value2);
        w.write_f32(rec.x); w.write_f32(rec.y); w.write_f32(rec.z);
        break;
    case ColdKind::Anim: {
        w.write_u8(static_cast<u8>(std::min<usize>(rec.clips.size(), 255)));
        usize n = std::min<usize>(rec.clips.size(), 255);
        for (usize i = 0; i < n; ++i) w.write_string(rec.clips[i]);
        w.write_bool(rec.bool_value);  // looping
        break;
    }
    case ColdKind::Construction:
        w.write_bool(rec.bool_value);  // under_construction
        w.write_f32(rec.value);        // build_time_total
        w.write_f32(rec.value2);       // build_progress
        break;
    }
}

inline ColdRecord read_cold_record(ByteReader& r) {
    ColdRecord rec;
    rec.kind = static_cast<ColdKind>(r.read_u8());
    switch (rec.kind) {
    case ColdKind::Health:
        rec.value = r.read_f32(); rec.value2 = r.read_f32();
        break;
    case ColdKind::State:
        rec.key = r.read_string(); rec.value = r.read_f32(); rec.value2 = r.read_f32();
        break;
    case ColdKind::Attribute:
        rec.key = r.read_string(); rec.value = r.read_f32();
        break;
    case ColdKind::StringAttribute:
        rec.key = r.read_string(); rec.str_value = r.read_string();
        break;
    case ColdKind::AbilityAdd:
        rec.key = r.read_string(); rec.uint_value = r.read_u32(); rec.byte_value = r.read_u8();
        break;
    case ColdKind::AbilityRemove:
        rec.key = r.read_string(); rec.byte_value = r.read_u8(); rec.bool_value = r.read_bool();
        break;
    case ColdKind::AbilityModifier:
        rec.key = r.read_string(); rec.str_value = r.read_string(); rec.value = r.read_f32();
        break;
    case ColdKind::Cooldown:
        rec.key = r.read_string(); rec.value = r.read_f32();
        break;
    case ColdKind::Owner:
        rec.uint_value = r.read_u8();
        break;
    case ColdKind::Status:
        rec.uint_value = r.read_u32(); rec.bool_value = r.read_bool();
        break;
    case ColdKind::Transform:
        rec.x = r.read_f32(); rec.y = r.read_f32(); rec.z = r.read_f32(); rec.facing = r.read_f32();
        break;
    case ColdKind::ItemCharges:
    case ColdKind::ItemLevel:
        rec.uint_value = r.read_u32();
        break;
    case ColdKind::Inventory:
        rec.uint_value = r.read_u32(); rec.uint_value2 = r.read_u32();
        rec.x = r.read_f32(); rec.y = r.read_f32(); rec.z = r.read_f32();
        break;
    case ColdKind::Anim: {
        u8 n = r.read_u8();
        rec.clips.reserve(n);
        for (u8 i = 0; i < n; ++i) rec.clips.push_back(r.read_string());
        rec.bool_value = r.read_bool();  // looping
        break;
    }
    case ColdKind::Construction:
        rec.bool_value = r.read_bool();  // under_construction
        rec.value      = r.read_f32();   // build_time_total
        rec.value2     = r.read_f32();   // build_progress
        break;
    }
    return rec;
}

// S_COLD — a COUNT-PREFIXED batch of cold records for one entity. Wire:
// {entity_id, u16 count, ColdRecord[count]}. One record = on-change while in
// sight; N records = the MATERIALIZE batch sent right after S_SPAWN/S_SHOW. Same
// records, same client apply (apply_cold_record) — batch = apply all.
inline std::vector<u8> build_cold_batch(u32 entity_id, const std::vector<ColdRecord>& recs) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_COLD));
    w.write_u32(entity_id);
    w.write_u16(static_cast<u16>(recs.size()));
    for (auto& rec : recs) write_cold_record(w, rec);
    return std::move(w.data());
}

// Convenience for the common on-change case (one record).
inline std::vector<u8> build_cold(u32 entity_id, const ColdRecord& rec) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_COLD));
    w.write_u32(entity_id);
    w.write_u16(1);
    write_cold_record(w, rec);
    return std::move(w.data());
}

struct ColdData {
    u32 entity_id;
    std::vector<ColdRecord> records;
};

inline ColdData parse_cold(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();  // skip MsgType
    ColdData c;
    c.entity_id = r.read_u32();
    u16 n = r.read_u16();
    c.records.reserve(n);
    for (u16 i = 0; i < n; ++i) c.records.push_back(read_cold_record(r));
    return c;
}

// Record makers — the one place each kind's fields are set, shared by the single
// on-change builders below AND the host's collect_cold_records batch.
inline ColdRecord cold_health(f32 current, f32 max) {
    ColdRecord r; r.kind = ColdKind::Health; r.value = current; r.value2 = max; return r;
}
inline ColdRecord cold_state(std::string_view id, f32 current, f32 max) {
    ColdRecord r; r.kind = ColdKind::State; r.key = id; r.value = current; r.value2 = max; return r;
}
inline ColdRecord cold_attr_rec(std::string_view key, f32 value) {
    ColdRecord r; r.kind = ColdKind::Attribute; r.key = key; r.value = value; return r;
}
inline ColdRecord cold_str_attr_rec(std::string_view key, std::string_view value) {
    ColdRecord r; r.kind = ColdKind::StringAttribute; r.key = key; r.str_value = value; return r;
}
inline ColdRecord cold_ability_add_rec(std::string_view ability_id, u32 level, u8 source_kind) {
    ColdRecord r; r.kind = ColdKind::AbilityAdd; r.key = ability_id; r.uint_value = level; r.byte_value = source_kind; return r;
}
inline ColdRecord cold_ability_remove_rec(std::string_view ability_id, u8 source_kind, bool all_instances) {
    ColdRecord r; r.kind = ColdKind::AbilityRemove; r.key = ability_id; r.byte_value = source_kind; r.bool_value = all_instances; return r;
}
inline ColdRecord cold_ability_modifier_rec(std::string_view ability_id, std::string_view modifier_key, f32 value) {
    ColdRecord r; r.kind = ColdKind::AbilityModifier; r.key = ability_id; r.str_value = modifier_key; r.value = value; return r;
}
inline ColdRecord cold_cooldown_rec(std::string_view ability_id, f32 secs) {
    ColdRecord r; r.kind = ColdKind::Cooldown; r.key = ability_id; r.value = secs; return r;
}
inline ColdRecord cold_owner_rec(u8 new_owner) {
    ColdRecord r; r.kind = ColdKind::Owner; r.uint_value = new_owner; return r;
}
inline ColdRecord cold_status_rec(u32 flag, bool on) {
    ColdRecord r; r.kind = ColdKind::Status; r.uint_value = flag; r.bool_value = on; return r;
}
inline ColdRecord cold_transform_rec(f32 x, f32 y, f32 z, f32 facing) {
    ColdRecord r; r.kind = ColdKind::Transform; r.x = x; r.y = y; r.z = z; r.facing = facing; return r;
}
inline ColdRecord cold_item_charges_rec(i32 charges) {
    ColdRecord r; r.kind = ColdKind::ItemCharges; r.uint_value = static_cast<u32>(charges); return r;
}
inline ColdRecord cold_item_level_rec(i32 level) {
    ColdRecord r; r.kind = ColdKind::ItemLevel; r.uint_value = static_cast<u32>(level); return r;
}
inline ColdRecord cold_inventory_rec(u32 slot, u32 item_id, f32 x, f32 y, f32 z) {
    ColdRecord r; r.kind = ColdKind::Inventory; r.uint_value = slot; r.uint_value2 = item_id;
    r.x = x; r.y = y; r.z = z; return r;
}
inline ColdRecord cold_anim_rec(const std::vector<std::string>& clips, bool looping) {
    ColdRecord r; r.kind = ColdKind::Anim; r.clips = clips; r.bool_value = looping; return r;
}
inline ColdRecord cold_construction_rec(bool under_construction, f32 build_time_total, f32 build_progress) {
    ColdRecord r; r.kind = ColdKind::Construction;
    r.bool_value = under_construction; r.value = build_time_total; r.value2 = build_progress;
    return r;
}

// Single on-change message builders (rename of the old build_update_* — same
// arg lists, so call sites are a pure rename). Each wraps a record maker.
inline std::vector<u8> build_cold_health(u32 entity_id, f32 current, f32 max) {
    return build_cold(entity_id, cold_health(current, max));
}
inline std::vector<u8> build_cold_attr(u32 entity_id, std::string_view key, f32 value) {
    return build_cold(entity_id, cold_attr_rec(key, value));
}
inline std::vector<u8> build_cold_str_attr(u32 entity_id, std::string_view key, std::string_view value) {
    return build_cold(entity_id, cold_str_attr_rec(key, value));
}
inline std::vector<u8> build_cold_state(u32 entity_id, std::string_view state_id, f32 current, f32 max) {
    return build_cold(entity_id, cold_state(state_id, current, max));
}
inline std::vector<u8> build_cold_ability_add(u32 entity_id, std::string_view ability_id, u32 level, u8 source_kind) {
    return build_cold(entity_id, cold_ability_add_rec(ability_id, level, source_kind));
}
inline std::vector<u8> build_cold_ability_remove(u32 entity_id, std::string_view ability_id, u8 source_kind, bool all_instances) {
    return build_cold(entity_id, cold_ability_remove_rec(ability_id, source_kind, all_instances));
}
inline std::vector<u8> build_cold_ability_modifier(u32 entity_id, std::string_view ability_id,
                                                    std::string_view modifier_key, f32 value) {
    return build_cold(entity_id, cold_ability_modifier_rec(ability_id, modifier_key, value));
}
inline std::vector<u8> build_cold_status(u32 entity_id, u32 flag, bool on) {
    return build_cold(entity_id, cold_status_rec(flag, on));
}
inline std::vector<u8> build_cold_owner(u32 entity_id, u8 new_owner) {
    return build_cold(entity_id, cold_owner_rec(new_owner));
}
inline std::vector<u8> build_cold_transform(u32 entity_id, f32 x, f32 y, f32 z, f32 facing) {
    return build_cold(entity_id, cold_transform_rec(x, y, z, facing));
}
inline std::vector<u8> build_cold_cooldown(u32 entity_id, std::string_view ability_id, f32 secs) {
    return build_cold(entity_id, cold_cooldown_rec(ability_id, secs));
}
inline std::vector<u8> build_cold_item_charges(u32 item_entity_id, i32 charges) {
    return build_cold(item_entity_id, cold_item_charges_rec(charges));
}
inline std::vector<u8> build_cold_item_level(u32 item_entity_id, i32 level) {
    return build_cold(item_entity_id, cold_item_level_rec(level));
}
inline std::vector<u8> build_cold_inventory(u32 carrier_id, u32 slot, u32 item_id,
                                             f32 x = 0, f32 y = 0, f32 z = 0) {
    return build_cold(carrier_id, cold_inventory_rec(slot, item_id, x, y, z));
}
inline std::vector<u8> build_cold_anim(u32 entity_id, const std::vector<std::string>& clips, bool looping) {
    return build_cold(entity_id, cold_anim_rec(clips, looping));
}
inline std::vector<u8> build_cold_construction(u32 entity_id, bool under_construction,
                                               f32 build_time_total, f32 build_progress) {
    return build_cold(entity_id, cold_construction_rec(under_construction, build_time_total, build_progress));
}

// ── HUD sync builders + parsers (16c-v) ──────────────────────────────────
// One message per atom mutation. Opcodes live in the 0x7X block. Target
// filtering is transport-level: server picks which peer(s) to send each
// message to based on the node's `players_mask`. UINT32_MAX = broadcast.

inline std::vector<u8> build_hud_create_node(std::string_view template_id,
                                              std::string_view anchor,
                                              f32 x, f32 y, f32 w, f32 h) {
    ByteWriter wr;
    wr.write_u8(static_cast<u8>(MsgType::S_HUD_CREATE_NODE));
    wr.write_string(template_id);
    wr.write_string(anchor);
    wr.write_f32(x); wr.write_f32(y); wr.write_f32(w); wr.write_f32(h);
    return std::move(wr.data());
}

inline std::vector<u8> build_hud_destroy_node(std::string_view node_id) {
    ByteWriter wr;
    wr.write_u8(static_cast<u8>(MsgType::S_HUD_DESTROY_NODE));
    wr.write_string(node_id);
    return std::move(wr.data());
}

// Label text is a LocalizedString — the host ships {key, args}; each
// client resolves with its own active locale. Empty key = blank label.
inline std::vector<u8> build_hud_set_label_text(
        std::string_view node_id,
        std::string_view loc_key,
        const std::vector<std::pair<std::string, std::string>>& loc_args) {
    ByteWriter wr;
    wr.write_u8(static_cast<u8>(MsgType::S_HUD_SET_LABEL_TEXT));
    wr.write_string(node_id);
    wr.write_string(loc_key);
    usize n = std::min<usize>(loc_args.size(), 255);
    wr.write_u8(static_cast<u8>(n));
    for (usize i = 0; i < n; ++i) {
        wr.write_string(loc_args[i].first);
        wr.write_string(loc_args[i].second);
    }
    return std::move(wr.data());
}

inline std::vector<u8> build_hud_set_bar_fill(std::string_view node_id, f32 fill) {
    ByteWriter wr;
    wr.write_u8(static_cast<u8>(MsgType::S_HUD_SET_BAR_FILL));
    wr.write_string(node_id);
    wr.write_f32(fill);
    return std::move(wr.data());
}

inline std::vector<u8> build_hud_set_node_visible(std::string_view node_id, bool visible) {
    ByteWriter wr;
    wr.write_u8(static_cast<u8>(MsgType::S_HUD_SET_NODE_VISIBLE));
    wr.write_string(node_id);
    wr.write_bool(visible);
    return std::move(wr.data());
}

inline std::vector<u8> build_hud_set_image_source(std::string_view node_id, std::string_view path) {
    ByteWriter wr;
    wr.write_u8(static_cast<u8>(MsgType::S_HUD_SET_IMAGE_SOURCE));
    wr.write_string(node_id);
    wr.write_string(path);
    return std::move(wr.data());
}

inline std::vector<u8> build_hud_set_button_enabled(std::string_view node_id, bool enabled) {
    ByteWriter wr;
    wr.write_u8(static_cast<u8>(MsgType::S_HUD_SET_BUTTON_ENABLED));
    wr.write_string(node_id);
    wr.write_bool(enabled);
    return std::move(wr.data());
}

// Text tags — fire-and-forget broadcast at creation; animation runs
// locally on each side from identical starting params. The tag's `id` is
// a host-allocated shared ECS entity id, identical on every client, and
// is the key DestroyTextTag uses. Permanent tags (lifespan == 0) are
// replayed to late joiners; transient ones self-expire per side.
// Text-tag create wire format. The text payload is a LocalizedString:
//   - key: string (empty key = nothing to render; the tag still spawns
//          for animation timing, but draws nothing locally).
//   - args_count: u8.
//   - args: count × (name: string, value: string).
//
// Each receiving client resolves the key against its own active locale
// before rendering. There is no literal-text path — player-facing text
// always flows through L() on the server.
inline std::vector<u8> build_hud_create_text_tag(
        u32 id,
        std::string_view loc_key,
        const std::vector<std::pair<std::string, std::string>>& loc_args,
        u8 style,
        f32 px_size,
        f32 wx, f32 wy, f32 wz,
        simulation::Unit unit, f32 z_offset,
        u32 color_rgba,
        f32 speed, f32 spread, f32 scale_end,
        f32 lifespan, f32 fade) {
    ByteWriter wr;
    wr.write_u8(static_cast<u8>(MsgType::S_HUD_CREATE_TEXT_TAG));
    wr.write_u32(id);
    wr.write_string(loc_key);
    usize n = std::min<usize>(loc_args.size(), 255);
    wr.write_u8(static_cast<u8>(n));
    for (usize i = 0; i < n; ++i) {
        wr.write_string(loc_args[i].first);
        wr.write_string(loc_args[i].second);
    }
    wr.write_u8(style);
    wr.write_f32(px_size);
    wr.write_f32(wx); wr.write_f32(wy); wr.write_f32(wz);
    wr.write_u32(unit.id);
    wr.write_f32(z_offset);
    wr.write_u32(color_rgba);
    wr.write_f32(speed); wr.write_f32(spread); wr.write_f32(scale_end);
    wr.write_f32(lifespan); wr.write_f32(fade);
    return std::move(wr.data());
}

// Destroy a permanent text tag by its shared ECS id.
inline std::vector<u8> build_hud_destroy_text_tag(u32 id) {
    ByteWriter wr;
    wr.write_u8(static_cast<u8>(MsgType::S_HUD_DESTROY_TEXT_TAG));
    wr.write_u32(id);
    return std::move(wr.data());
}

// Display-message wire format. The text payload is a LocalizedString
// (key + args); duration is in seconds (≤ 0 → use the composite's
// authored default_lifespan).
inline std::vector<u8> build_hud_display_message(
        std::string_view loc_key,
        const std::vector<std::pair<std::string, std::string>>& loc_args,
        f32 duration) {
    ByteWriter wr;
    wr.write_u8(static_cast<u8>(MsgType::S_HUD_DISPLAY_MESSAGE));
    wr.write_string(loc_key);
    usize n = std::min<usize>(loc_args.size(), 255);
    wr.write_u8(static_cast<u8>(n));
    for (usize i = 0; i < n; ++i) {
        wr.write_string(loc_args[i].first);
        wr.write_string(loc_args[i].second);
    }
    wr.write_f32(duration);
    return std::move(wr.data());
}

// Manual-mode action-bar slot binding (ActionBarSetSlot / ClearSlot). Runs in
// host/worker Lua only, so it must be synced to clients + replayed on join —
// otherwise the client's slots keep the hud.json default (empty bound_ability)
// and the manual-mode bar renders nothing. slot is 0-based; empty ability_id
// clears the slot.
inline std::vector<u8> build_hud_action_bar_set_slot(u32 slot, std::string_view ability_id) {
    ByteWriter wr;
    wr.write_u8(static_cast<u8>(MsgType::S_HUD_ACTION_BAR_SET_SLOT));
    wr.write_u32(slot);
    wr.write_string(ability_id);
    return std::move(wr.data());
}

} // namespace uldum::network
