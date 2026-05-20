#include "map/placements_bin.h"
#include "core/log.h"

#include <cstring>
#include <unordered_map>

#include <glm/gtc/constants.hpp>

namespace uldum::map {

static constexpr const char* TAG = "PlacementsBin";

// On-disk facing is degrees (matches the previous JSON authoring
// convention + the editor's display units). In-memory `m_scene` keeps
// facings in radians (matches simulation::create_* signatures and the
// existing JSON parser's behavior). Convert at the file boundary so
// callers don't have to think about the unit mismatch.
static constexpr f32 kDegToRad = glm::pi<f32>() / 180.0f;
static constexpr f32 kRadToDeg = 180.0f / glm::pi<f32>();

namespace {

// ── Reader ───────────────────────────────────────────────────────────────
//
// Simple offset-bumping cursor over a byte span. Each `read_*` returns
// false on short read; callers check `ok` once at the end (saves a
// branch on every field, matches how `parse_state` does it in protocol.h).

struct Reader {
    const u8* p   = nullptr;
    usize     rem = 0;
    bool      ok  = true;

    void take(void* dst, usize n) {
        if (rem < n) { ok = false; return; }
        std::memcpy(dst, p, n);
        p += n; rem -= n;
    }
    u8  read_u8()  { u8  v = 0; take(&v, 1); return v; }
    u16 read_u16() { u16 v = 0; take(&v, 2); return v; }
    u32 read_u32() { u32 v = 0; take(&v, 4); return v; }
    f32 read_f32() { f32 v = 0; take(&v, 4); return v; }

    // Returns a string of `len` bytes; the underlying span is not held
    // (we copy into the std::string).
    std::string read_bytes(usize len) {
        if (rem < len) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p), len);
        p += len; rem -= len;
        return s;
    }
};

// ── Writer ───────────────────────────────────────────────────────────────

struct Writer {
    std::vector<u8> buf;

    void put(const void* src, usize n) {
        auto base = buf.size();
        buf.resize(base + n);
        std::memcpy(buf.data() + base, src, n);
    }
    void write_u8 (u8  v) { put(&v, 1); }
    void write_u16(u16 v) { put(&v, 2); }
    void write_u32(u32 v) { put(&v, 4); }
    void write_f32(f32 v) { put(&v, 4); }
    void write_string_u8len(std::string_view s) {
        // Cap at 255 bytes (u8 length prefix). Authors exceeding the
        // cap get a warning + truncation rather than a silent failure.
        usize n = s.size();
        if (n > 255) {
            log::warn(TAG, "id '{}...' exceeds 255-byte cap; truncating",
                      s.substr(0, 32));
            n = 255;
        }
        write_u8(static_cast<u8>(n));
        put(s.data(), n);
    }
};

} // namespace

// ── Read ─────────────────────────────────────────────────────────────────

bool read_placements(std::span<const u8> data, SceneData& out) {
    Reader r;
    r.p   = data.data();
    r.rem = data.size();

    // Type-id string table.
    u32 type_id_count = r.read_u32();
    std::vector<std::string> type_ids;
    type_ids.reserve(type_id_count);
    for (u32 i = 0; i < type_id_count && r.ok; ++i) {
        u8 len = r.read_u8();
        type_ids.push_back(r.read_bytes(len));
    }
    auto resolve = [&](u16 idx) -> std::string {
        return (idx < type_ids.size()) ? type_ids[idx] : std::string{};
    };

    // Units.
    u32 unit_count = r.read_u32();
    out.units.reserve(unit_count);
    for (u32 i = 0; i < unit_count && r.ok; ++i) {
        PlacedUnit u;
        u.type   = resolve(r.read_u16());
        u.owner  = r.read_u8();
        u.x      = r.read_f32();
        u.y      = r.read_f32();
        u.facing = r.read_f32() * kDegToRad;   // deg on disk → rad in-memory
        out.units.push_back(std::move(u));
    }

    // Destructables.
    u32 dest_count = r.read_u32();
    out.destructables.reserve(dest_count);
    for (u32 i = 0; i < dest_count && r.ok; ++i) {
        PlacedDestructable d;
        d.type      = resolve(r.read_u16());
        d.x         = r.read_f32();
        d.y         = r.read_f32();
        d.facing    = r.read_f32() * kDegToRad;
        d.variation = r.read_u8();
        out.destructables.push_back(std::move(d));
    }

    // Items.
    u32 item_count = r.read_u32();
    out.items.reserve(item_count);
    for (u32 i = 0; i < item_count && r.ok; ++i) {
        PlacedItem it;
        it.type = resolve(r.read_u16());
        it.x    = r.read_f32();
        it.y    = r.read_f32();
        out.items.push_back(std::move(it));
    }

    // Doodads.
    u32 dood_count = r.read_u32();
    out.doodads.reserve(dood_count);
    for (u32 i = 0; i < dood_count && r.ok; ++i) {
        PlacedDoodad d;
        d.type      = resolve(r.read_u16());
        d.x         = r.read_f32();
        d.y         = r.read_f32();
        d.facing    = r.read_f32() * kDegToRad;
        d.variation = r.read_u8();
        out.doodads.push_back(std::move(d));
    }

    // Regions.
    u32 region_count = r.read_u32();
    out.regions.reserve(region_count);
    for (u32 i = 0; i < region_count && r.ok; ++i) {
        Region reg;
        u8 id_len = r.read_u8();
        reg.id = r.read_bytes(id_len);
        u16 rect_count = r.read_u16();
        reg.rects.reserve(rect_count);
        for (u16 j = 0; j < rect_count && r.ok; ++j) {
            RegionRect rect;
            rect.x0 = r.read_f32(); rect.y0 = r.read_f32();
            rect.x1 = r.read_f32(); rect.y1 = r.read_f32();
            reg.rects.push_back(rect);
        }
        u16 circle_count = r.read_u16();
        reg.circles.reserve(circle_count);
        for (u16 j = 0; j < circle_count && r.ok; ++j) {
            RegionCircle c;
            c.cx = r.read_f32();
            c.cy = r.read_f32();
            c.r  = r.read_f32();
            reg.circles.push_back(c);
        }
        out.regions.push_back(std::move(reg));
    }

    // Cameras.
    u32 cam_count = r.read_u32();
    out.cameras.reserve(cam_count);
    for (u32 i = 0; i < cam_count && r.ok; ++i) {
        CameraSetup cam;
        u8 id_len = r.read_u8();
        cam.id        = r.read_bytes(id_len);
        cam.target_x  = r.read_f32();
        cam.target_y  = r.read_f32();
        cam.target_z  = r.read_f32();
        cam.distance  = r.read_f32();
        cam.pitch_deg = r.read_f32();
        cam.yaw_deg   = r.read_f32();
        out.cameras.push_back(std::move(cam));
    }

    if (!r.ok) {
        log::error(TAG, "placements.bin truncated or malformed");
        return false;
    }
    return true;
}

// ── Write ────────────────────────────────────────────────────────────────

std::vector<u8> write_placements(const SceneData& scene) {
    Writer w;

    // Build the dedupe table by walking every placement category.
    std::unordered_map<std::string, u16> type_id_index;
    std::vector<std::string> type_ids;
    auto intern = [&](const std::string& s) -> u16 {
        auto it = type_id_index.find(s);
        if (it != type_id_index.end()) return it->second;
        // The wire format caps at 65535 type ids — practical maps stay
        // well under this. A map that overflows is almost certainly a
        // bug in the editor or a content-pipeline issue.
        if (type_ids.size() >= 0xFFFFu) {
            log::error(TAG, "more than 65535 unique type ids in scene; "
                            "writing index 0 for excess. Map content audit needed.");
            return 0;
        }
        u16 idx = static_cast<u16>(type_ids.size());
        type_id_index.emplace(s, idx);
        type_ids.push_back(s);
        return idx;
    };
    for (const auto& u : scene.units)         intern(u.type);
    for (const auto& d : scene.destructables) intern(d.type);
    for (const auto& it: scene.items)         intern(it.type);
    for (const auto& d : scene.doodads)       intern(d.type);

    w.write_u32(static_cast<u32>(type_ids.size()));
    for (const auto& s : type_ids) w.write_string_u8len(s);

    w.write_u32(static_cast<u32>(scene.units.size()));
    for (const auto& u : scene.units) {
        w.write_u16(intern(u.type));
        w.write_u8(static_cast<u8>(u.owner));
        w.write_f32(u.x);
        w.write_f32(u.y);
        w.write_f32(u.facing * kRadToDeg);   // rad in-memory → deg on disk
    }

    w.write_u32(static_cast<u32>(scene.destructables.size()));
    for (const auto& d : scene.destructables) {
        w.write_u16(intern(d.type));
        w.write_f32(d.x);
        w.write_f32(d.y);
        w.write_f32(d.facing * kRadToDeg);
        w.write_u8(d.variation);
    }

    w.write_u32(static_cast<u32>(scene.items.size()));
    for (const auto& it : scene.items) {
        w.write_u16(intern(it.type));
        w.write_f32(it.x);
        w.write_f32(it.y);
    }

    w.write_u32(static_cast<u32>(scene.doodads.size()));
    for (const auto& d : scene.doodads) {
        w.write_u16(intern(d.type));
        w.write_f32(d.x);
        w.write_f32(d.y);
        w.write_f32(d.facing * kRadToDeg);
        w.write_u8(d.variation);
    }

    w.write_u32(static_cast<u32>(scene.regions.size()));
    for (const auto& r : scene.regions) {
        w.write_string_u8len(r.id);
        w.write_u16(static_cast<u16>(r.rects.size()));
        for (const auto& rc : r.rects) {
            w.write_f32(rc.x0); w.write_f32(rc.y0);
            w.write_f32(rc.x1); w.write_f32(rc.y1);
        }
        w.write_u16(static_cast<u16>(r.circles.size()));
        for (const auto& c : r.circles) {
            w.write_f32(c.cx); w.write_f32(c.cy); w.write_f32(c.r);
        }
    }

    w.write_u32(static_cast<u32>(scene.cameras.size()));
    for (const auto& cam : scene.cameras) {
        w.write_string_u8len(cam.id);
        w.write_f32(cam.target_x);
        w.write_f32(cam.target_y);
        w.write_f32(cam.target_z);
        w.write_f32(cam.distance);
        w.write_f32(cam.pitch_deg);
        w.write_f32(cam.yaw_deg);
    }

    return std::move(w.buf);
}

} // namespace uldum::map
