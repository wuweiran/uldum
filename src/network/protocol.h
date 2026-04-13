#pragma once

#include "core/types.h"
#include "input/command.h"

#include <glm/vec3.hpp>

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uldum::network {

// ── Message type IDs ──────────────────────────────────────────────────────

enum class MsgType : u8 {
    // Client → Server
    C_JOIN   = 0x01,
    C_ORDER  = 0x02,
    C_LEAVE  = 0x03,

    // Server → Client
    S_WELCOME = 0x10,
    S_REJECT  = 0x11,
    S_SPAWN   = 0x20,
    S_DESTROY = 0x21,
    S_STATE   = 0x30,
    S_SOUND   = 0x31,
    S_START   = 0x40,   // all players connected, game begins
    S_END     = 0x41,   // game over, includes results
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

inline std::vector<u8> build_join(u32 map_hash) {
    ByteWriter w;
    w.write_u8(static_cast<u8>(MsgType::C_JOIN));
    w.write_u32(map_hash);
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

// ── Session messages ─────────────────────────────────────────────────────

inline std::vector<u8> build_start() {
    return {static_cast<u8>(MsgType::S_START)};
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

} // namespace uldum::network
