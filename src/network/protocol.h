#pragma once

#include "core/types.h"
#include "input/command.h"
#include "network/lobby.h"

#include <glm/vec3.hpp>

#include <cstring>
#include <span>
#include <string>
#include <string_view>
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
//   0x5X  server → client, playing / entity sync
//   0x6X  server → client, playing / session events
enum class MsgType : u8 {
    // ── Client → Server ──

    // Lobby
    C_JOIN          = 0x01,
    C_CLAIM_SLOT    = 0x02,
    C_RELEASE_SLOT  = 0x03,
    C_LOAD_DONE     = 0x04,

    // Playing
    C_ORDER         = 0x10,

    // Any phase
    C_LEAVE         = 0x20,

    // ── Server → Client ──

    // Lobby / pre-game
    S_REJECT        = 0x40,
    S_LOBBY_ASSIGN  = 0x41,   // tells a newly-joined peer its peer_id
    S_LOBBY_STATE   = 0x42,   // full lobby snapshot (broadcast on change)
    S_LOBBY_COMMIT  = 0x43,   // host locked the lobby — start loading the map
    S_WELCOME       = 0x44,   // bridge: final player_id, sent at end of Loading

    // Playing — entity sync
    S_SPAWN         = 0x50,
    S_DESTROY       = 0x51,
    S_STATE         = 0x52,
    S_UPDATE        = 0x53,   // on-change: attribute, state, or ability update
    S_SOUND         = 0x54,

    // Playing — session events
    S_START         = 0x60,   // all players loaded, game begins
    S_END           = 0x61,   // game over, includes results
    S_PAUSE_STATE   = 0x62,   // mid-game: list of disconnected players + timers
};

enum class RejectReason : u8 {
    Full     = 0,
    WrongMap = 1,
    Started  = 2,
};

// ── Binary read/write helpers ─────────────────────────────────────────────

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

    u8  read_u8()  { u8 v = 0;  read(&v, 1); return v; }
    u16 read_u16() { u16 v = 0; read(&v, 2); return v; }
    u32 read_u32() { u32 v = 0; read(&v, 4); return v; }
    f32 read_f32() { f32 v = 0; read(&v, 4); return v; }
    glm::vec3 read_vec3() { return {read_f32(), read_f32(), read_f32()}; }
    std::string read_string() {
        u16 len = read_u16();
        std::string s(m_data.begin() + m_pos, m_data.begin() + m_pos + len);
        m_pos += len;
        return s;
    }
    bool read_bool() { return read_u8() != 0; }

private:
    void read(void* p, size_t n) {
        std::memcpy(p, m_data.data() + m_pos, n);
        m_pos += n;
    }
    std::span<const u8> m_data;
    size_t m_pos = 0;
};

// ── Message builders ──────────────────────────────────────────────────────

// Returns the message type from a raw packet's first byte.
inline MsgType peek_type(std::span<const u8> data) {
    return static_cast<MsgType>(data[0]);
}

// ── Client → Server ──────────────────────────────────────────────────────

inline std::vector<u8> build_join(u32 map_hash, std::string_view player_name) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::C_JOIN));
    w.write_u32(map_hash);
    w.write_string(player_name);
    return std::move(w.data());
}

inline std::vector<u8> build_order(const input::GameCommand& cmd) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::C_ORDER));
    w.write_bool(cmd.queued);
    w.write_u8(static_cast<u8>(cmd.units.size()));
    for (auto& u : cmd.units) {
        w.write_u32(u.id);
        w.write_u32(u.generation);
    }
    // Serialize the variant index + payload
    u8 order_idx = static_cast<u8>(cmd.order.index());
    w.write_u8(order_idx);

    std::visit([&](auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, simulation::orders::Move>) {
            w.write_vec3(payload.target);
        } else if constexpr (std::is_same_v<T, simulation::orders::AttackMove>) {
            w.write_vec3(payload.target);
        } else if constexpr (std::is_same_v<T, simulation::orders::Attack>) {
            w.write_u32(payload.target.id);
            w.write_u32(payload.target.generation);
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
            w.write_u32(payload.target_unit.generation);
            w.write_vec3(payload.target_pos);
        } else if constexpr (std::is_same_v<T, simulation::orders::Train>) {
            w.write_string(payload.unit_type_id);
        } else if constexpr (std::is_same_v<T, simulation::orders::Research>) {
            w.write_string(payload.research_id);
        } else if constexpr (std::is_same_v<T, simulation::orders::Build>) {
            w.write_string(payload.building_type_id);
            w.write_vec3(payload.pos);
        } else if constexpr (std::is_same_v<T, simulation::orders::PickupItem>) {
            w.write_u32(payload.item.id);
            w.write_u32(payload.item.generation);
        } else if constexpr (std::is_same_v<T, simulation::orders::DropItem>) {
            w.write_u32(payload.item.id);
            w.write_u32(payload.item.generation);
            w.write_vec3(payload.pos);
        }
    }, cmd.order);

    return std::move(w.data());
}

inline std::vector<u8> build_leave() {
    return {static_cast<u8>(MsgType::C_LEAVE)};
}

// ── Server → Client ──────────────────────────────────────────────────────

inline std::vector<u8> build_welcome(u32 player_id, u32 player_count, u32 tick_rate) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_WELCOME));
    w.write_u32(player_id);
    w.write_u32(player_count);
    w.write_u32(tick_rate);
    return std::move(w.data());
}

inline std::vector<u8> build_reject(RejectReason reason) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_REJECT));
    w.write_u8(static_cast<u8>(reason));
    return std::move(w.data());
}

inline std::vector<u8> build_spawn(u32 entity_id, std::string_view type_id,
                                    u8 owner, f32 x, f32 y, f32 facing,
                                    bool newly_created = false) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_SPAWN));
    w.write_u32(entity_id);
    w.write_string(type_id);
    w.write_u8(owner);
    w.write_f32(x);
    w.write_f32(y);
    w.write_f32(facing);
    w.write_bool(newly_created);
    return std::move(w.data());
}

inline std::vector<u8> build_destroy(u32 entity_id) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_DESTROY));
    w.write_u32(entity_id);
    return std::move(w.data());
}

// Per-entity record in S_STATE
struct EntityState {
    u32 entity_id;
    f32 x, y, z;
    f32 facing;
    f32 health_frac;
    u8  flags;       // bit 0: moving, bit 1: attacking, bit 2: casting, bit 3: dead
    u32 target_id;
};

inline std::vector<u8> build_state(u32 tick, const std::vector<EntityState>& entities) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_STATE));
    w.write_u32(tick);
    w.write_u16(static_cast<u16>(entities.size()));
    for (auto& e : entities) {
        w.write_u32(e.entity_id);
        w.write_f32(e.x);
        w.write_f32(e.y);
        w.write_f32(e.z);
        w.write_f32(e.facing);
        w.write_f32(e.health_frac);
        w.write_u8(e.flags);
        w.write_u32(e.target_id);
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

// ── Client-side deserialization helpers ───────────────────────────────────

inline input::GameCommand parse_order(std::span<const u8> data, simulation::Player player) {
    ByteReader r(data);
    r.read_u8();  // skip MsgType

    input::GameCommand cmd;
    cmd.player = player;
    cmd.queued = r.read_bool();
    u8 unit_count = r.read_u8();
    for (u8 i = 0; i < unit_count; ++i) {
        simulation::Unit u;
        u.id = r.read_u32();
        u.generation = r.read_u32();
        cmd.units.push_back(u);
    }

    u8 order_idx = r.read_u8();
    switch (order_idx) {
    case 0: cmd.order = simulation::orders::Move{r.read_vec3()}; break;
    case 1: cmd.order = simulation::orders::AttackMove{r.read_vec3()}; break;
    case 2: {
        simulation::Unit t;
        t.id = r.read_u32();
        t.generation = r.read_u32();
        cmd.order = simulation::orders::Attack{t};
        break;
    }
    case 3: cmd.order = simulation::orders::Stop{}; break;
    case 4: cmd.order = simulation::orders::HoldPosition{}; break;
    case 5: {
        simulation::orders::Patrol p;
        u8 wpc = r.read_u8();
        for (u8 i = 0; i < wpc; ++i) p.waypoints.push_back(r.read_vec3());
        cmd.order = std::move(p);
        break;
    }
    case 6: {
        std::string ability = r.read_string();
        simulation::Unit tu;
        tu.id = r.read_u32();
        tu.generation = r.read_u32();
        glm::vec3 tp = r.read_vec3();
        cmd.order = simulation::orders::Cast{std::move(ability), tu, tp};
        break;
    }
    case 7: cmd.order = simulation::orders::Train{r.read_string()}; break;
    case 8: cmd.order = simulation::orders::Research{r.read_string()}; break;
    case 9: {
        std::string bt = r.read_string();
        glm::vec3 p = r.read_vec3();
        cmd.order = simulation::orders::Build{std::move(bt), p};
        break;
    }
    case 10: {
        simulation::Item it;
        it.id = r.read_u32();
        it.generation = r.read_u32();
        cmd.order = simulation::orders::PickupItem{it};
        break;
    }
    case 11: {
        simulation::Item it;
        it.id = r.read_u32();
        it.generation = r.read_u32();
        glm::vec3 p = r.read_vec3();
        cmd.order = simulation::orders::DropItem{it, p};
        break;
    }
    }
    return cmd;
}

struct WelcomeData {
    u32 player_id;
    u32 player_count;
    u32 tick_rate;
};

inline WelcomeData parse_welcome(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();  // skip MsgType
    WelcomeData w;
    w.player_id = r.read_u32();
    w.player_count = r.read_u32();
    w.tick_rate = r.read_u32();
    return w;
}

struct SpawnData {
    u32 entity_id;
    std::string type_id;
    u8 owner;
    f32 x, y, facing;
    bool newly_created = false;
};

inline SpawnData parse_spawn(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();  // skip MsgType
    SpawnData s;
    s.entity_id = r.read_u32();
    s.type_id = r.read_string();
    s.owner = r.read_u8();
    s.x = r.read_f32();
    s.y = r.read_f32();
    s.facing = r.read_f32();
    s.newly_created = r.read_bool();
    return s;
}

inline u32 parse_destroy(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    return r.read_u32();
}

struct StateData {
    u32 tick;
    std::vector<EntityState> entities;
};

inline StateData parse_state(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    StateData s;
    s.tick = r.read_u32();
    u16 count = r.read_u16();
    s.entities.resize(count);
    for (u16 i = 0; i < count; ++i) {
        auto& e = s.entities[i];
        e.entity_id = r.read_u32();
        e.x = r.read_f32();
        e.y = r.read_f32();
        e.z = r.read_f32();
        e.facing = r.read_f32();
        e.health_frac = r.read_f32();
        e.flags = r.read_u8();
        e.target_id = r.read_u32();
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

// S_END carries a winner player ID and a Lua-defined stats table serialized as JSON string.
inline std::vector<u8> build_end(u32 winner_id, std::string_view stats_json) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_END));
    w.write_u32(winner_id);
    w.write_string(stats_json);
    return std::move(w.data());
}

struct EndData {
    u32 winner_id;
    std::string stats_json;
};

inline EndData parse_end(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();
    EndData e;
    e.winner_id = r.read_u32();
    e.stats_json = r.read_string();
    return e;
}

// ── On-change unit updates ───────────────────────────────────────────────

enum class UpdateType : u8 {
    Attribute      = 0,   // numeric attribute changed (armor, strength, etc.)
    StringAttribute = 1,  // string attribute changed (armor_type, etc.)
    State          = 2,   // state current/max changed (mana, energy, etc.)
    AbilityAdd     = 3,   // ability added
    AbilityRemove  = 4,   // ability removed
    Owner          = 5,   // ownership changed
};

inline std::vector<u8> build_update_attr(u32 entity_id, std::string_view key, f32 value) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_UPDATE));
    w.write_u32(entity_id);
    w.write_u8(static_cast<u8>(UpdateType::Attribute));
    w.write_string(key);
    w.write_f32(value);
    return std::move(w.data());
}

inline std::vector<u8> build_update_str_attr(u32 entity_id, std::string_view key, std::string_view value) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_UPDATE));
    w.write_u32(entity_id);
    w.write_u8(static_cast<u8>(UpdateType::StringAttribute));
    w.write_string(key);
    w.write_string(value);
    return std::move(w.data());
}

inline std::vector<u8> build_update_state(u32 entity_id, std::string_view state_id, f32 current, f32 max) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_UPDATE));
    w.write_u32(entity_id);
    w.write_u8(static_cast<u8>(UpdateType::State));
    w.write_string(state_id);
    w.write_f32(current);
    w.write_f32(max);
    return std::move(w.data());
}

inline std::vector<u8> build_update_ability_add(u32 entity_id, std::string_view ability_id, u32 level) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_UPDATE));
    w.write_u32(entity_id);
    w.write_u8(static_cast<u8>(UpdateType::AbilityAdd));
    w.write_string(ability_id);
    w.write_u32(level);
    return std::move(w.data());
}

inline std::vector<u8> build_update_ability_remove(u32 entity_id, std::string_view ability_id) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_UPDATE));
    w.write_u32(entity_id);
    w.write_u8(static_cast<u8>(UpdateType::AbilityRemove));
    w.write_string(ability_id);
    return std::move(w.data());
}

inline std::vector<u8> build_update_owner(u32 entity_id, u8 new_owner) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::S_UPDATE));
    w.write_u32(entity_id);
    w.write_u8(static_cast<u8>(UpdateType::Owner));
    w.write_u8(new_owner);
    return std::move(w.data());
}

struct UpdateData {
    u32 entity_id;
    UpdateType type;
    std::string key;
    f32 value = 0;
    f32 value2 = 0;    // max for State
    std::string str_value;
    u32 uint_value = 0; // level for AbilityAdd, owner for Owner
};

inline UpdateData parse_update(std::span<const u8> data) {
    ByteReader r(data);
    r.read_u8();  // skip MsgType
    UpdateData u;
    u.entity_id = r.read_u32();
    u.type = static_cast<UpdateType>(r.read_u8());
    switch (u.type) {
    case UpdateType::Attribute:
        u.key = r.read_string();
        u.value = r.read_f32();
        break;
    case UpdateType::StringAttribute:
        u.key = r.read_string();
        u.str_value = r.read_string();
        break;
    case UpdateType::State:
        u.key = r.read_string();
        u.value = r.read_f32();
        u.value2 = r.read_f32();
        break;
    case UpdateType::AbilityAdd:
        u.key = r.read_string();
        u.uint_value = r.read_u32();
        break;
    case UpdateType::AbilityRemove:
        u.key = r.read_string();
        break;
    case UpdateType::Owner:
        u.uint_value = r.read_u8();
        break;
    }
    return u;
}

} // namespace uldum::network
