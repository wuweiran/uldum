#include "editor/map_bootstrap.h"
#include "editor/overlay_decals.h"

#include "map/terrain_data.h"
#include "map/map.h"
#include "map/placements_bin.h"
#include "core/log.h"

#include <cmath>
#include <fstream>
#include <vector>

namespace uldum::editor::map_bootstrap {

namespace {

constexpr const char* TAG = "MapBootstrap";
namespace fs = std::filesystem;

bool write_text(const fs::path& path, const std::string& contents) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(f);
}

// Flat RGBA fill (opaque).
std::vector<u8> solid_rgba(u32 w, u32 h, u8 r, u8 g, u8 b) {
    std::vector<u8> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i+0] = r; px[i+1] = g; px[i+2] = b; px[i+3] = 255;
    }
    return px;
}

// One skybox face of a smooth zenith→horizon→ground gradient. The color of a
// texel is a function of its true world DIRECTION (its elevation), not its row —
// so the gradient is continuous across every cube-face seam and smooth in real
// elevation. A per-row gradient looks sharp because elevation doesn't run along
// the same texture axis on every face (it's V on ±Y, U on ±X). Faces are the
// standard cubemap order 0..5 = +X,-X,+Y,-Y,+Z,-Z, with cube axes = game axes
// (X=right, Y=forward, Z=up), so elevation is the sampled direction's Z.
std::vector<u8> skybox_face(u32 size, int face) {
    const float sky[3]     = { 88.0f, 150.0f, 226.0f };   // zenith blue
    const float horizon[3] = {236.0f, 224.0f, 190.0f };   // warm cream haze
    const float ground[3]  = {150.0f, 120.0f,  74.0f };   // sunlit brown-yellow

    std::vector<u8> px(static_cast<size_t>(size) * size * 4);
    for (u32 y = 0; y < size; ++y) {
        float t  = (y + 0.5f) / static_cast<float>(size);
        float vc = 2.0f * t - 1.0f;
        for (u32 x = 0; x < size; ++x) {
            float s  = (x + 0.5f) / static_cast<float>(size);
            float uc = 2.0f * s - 1.0f;

            // Standard cubemap texel → direction (cube axes); dz = up component.
            float dx, dy, dz;
            switch (face) {
                case 0: dx =  1.0f; dy = -vc;   dz = -uc;   break;  // +X right
                case 1: dx = -1.0f; dy = -vc;   dz =  uc;   break;  // -X left
                case 2: dx =  uc;   dy =  1.0f; dz =  vc;   break;  // +Y forward
                case 3: dx =  uc;   dy = -1.0f; dz = -vc;   break;  // -Y back
                case 4: dx =  uc;   dy = -vc;   dz =  1.0f; break;  // +Z up
                default:dx = -uc;   dy = -vc;   dz = -1.0f; break;  // -Z down
            }
            float len = std::sqrt(dx*dx + dy*dy + dz*dz);
            float e   = dz / len;                 // sin(elevation), -1..1
            float p   = 0.5f * (e + 1.0f);        // 0 nadir → 0.5 horizon → 1 zenith

            float rgb[3];
            for (int c = 0; c < 3; ++c) {
                // Linear across the whole hemisphere = gentle, no steep horizon.
                rgb[c] = (p >= 0.5f)
                    ? horizon[c] + (sky[c]    - horizon[c]) * ((p - 0.5f) / 0.5f)
                    : ground[c]  + (horizon[c] - ground[c]) * (p / 0.5f);
            }
            size_t i = (static_cast<size_t>(y) * size + x) * 4;
            px[i+0] = static_cast<u8>(rgb[0] + 0.5f);
            px[i+1] = static_cast<u8>(rgb[1] + 0.5f);
            px[i+2] = static_cast<u8>(rgb[2] + 0.5f);
            px[i+3] = 255;
        }
    }
    return px;
}

} // namespace

std::string create(const fs::path& dest_dir, const Options& opts, const EncodeFn& encode) {
    std::error_code ec;
    fs::create_directories(dest_dir, ec);
    if (ec) return "cannot create '" + dest_dir.string() + "': " + ec.message();

    // ── Generated assets (no engine default) ─────────────────────────────

    // Terrain diffuse — flat brown dirt (256²).
    {
        auto px = solid_rgba(256, 256, 96, 74, 52);
        std::string err = encode(dest_dir / "textures/terrain/dirt.ktx2",
                                 px.data(), 256, 256, /*linear=*/false);
        if (!err.empty()) return "terrain texture: " + err;
    }

    // Overlay decals — style matched to the preset.
    {
        auto style = opts.input_preset == "action" ? overlay_decals::Style::Action
                                                    : overlay_decals::Style::Original;
        for (const auto& d : overlay_decals::generate(style)) {
            std::string err = encode(dest_dir / "textures/overlays" / d.filename,
                                     d.pixels.data(), d.w, d.h, /*linear=*/true);
            if (!err.empty()) return "overlay '" + d.filename + "': " + err;
        }
    }

    // Skybox — 6 gradient cubemap faces.
    {
        const char* faces[6] = { "px", "nx", "py", "ny", "pz", "nz" };
        for (int i = 0; i < 6; ++i) {
            auto px = skybox_face(256, i);
            std::string err = encode(dest_dir / "textures/skybox" / (std::string(faces[i]) + ".ktx2"),
                                     px.data(), 256, 256, /*linear=*/false);
            if (!err.empty()) return std::string("skybox face '") + faces[i] + "': " + err;
        }
    }

    // ── Config that wires the assets ─────────────────────────────────────

    // manifest.json — only fields with no sane default, plus the skybox wiring.
    {
        std::string m =
            "{\n"
            "    \"format\": 1,\n"
            "    \"id\": \"" + opts.id + "\",\n"
            "    \"name\": \"" + opts.name + "\",\n"
            "    \"tileset\": \"tileset.json\",\n"
            "    \"start_scene\": \"scene_01\",\n"
            "    \"input\": { \"preset\": \"" + opts.input_preset + "\" },\n"
            "    \"environment\": {\n"
            "        \"skybox\": {\n"
            "            \"right\":  \"textures/skybox/px.ktx2\",\n"
            "            \"left\":   \"textures/skybox/nx.ktx2\",\n"
            "            \"front\":  \"textures/skybox/py.ktx2\",\n"
            "            \"back\":   \"textures/skybox/ny.ktx2\",\n"
            "            \"top\":    \"textures/skybox/pz.ktx2\",\n"
            "            \"bottom\": \"textures/skybox/nz.ktx2\"\n"
            "        }\n"
            "    }\n"
            "}\n";
        if (!write_text(dest_dir / "manifest.json", m)) return "failed to write manifest.json";
    }

    // tileset.json — one dirt layer.
    {
        std::string t =
            "{\n"
            "    \"name\": \"Default\",\n"
            "    \"layers\": [\n"
            "        { \"id\": 0, \"name\": \"dirt\", \"diffuse\": \"textures/terrain/dirt.ktx2\" }\n"
            "    ]\n"
            "}\n";
        if (!write_text(dest_dir / "tileset.json", t)) return "failed to write tileset.json";
    }

    // hud.json — binds the overlay decal slots (unbound → no rings/AoE preview).
    {
        std::string h =
            "{\n"
            "    \"targeting\": {\n"
            "        \"selection_texture\": \"textures/overlays/ring_stroke.ktx2\",\n"
            "        \"range_ring\":  { \"texture\": \"textures/overlays/ring_stroke.ktx2\" },\n"
            "        \"cast_curve\":  { \"texture\": \"textures/overlays/curve_stroke.ktx2\" },\n"
            "        \"snap_target\": { \"texture\": \"textures/overlays/snap_target.ktx2\" },\n"
            "        \"aoe\": {\n"
            "            \"circle_texture\": \"textures/overlays/aoe_circle.ktx2\",\n"
            "            \"cone_texture\":   \"textures/overlays/aoe_cone.ktx2\",\n"
            "            \"line_texture\":   \"textures/overlays/aoe_line.ktx2\"\n"
            "        }\n"
            "    }\n"
            "}\n";
        if (!write_text(dest_dir / "hud.json", h)) return "failed to write hud.json";
    }

    // ── Scene: flat terrain + empty placements + camera bounds ────────────

    map::TerrainData td;
    td.tiles_x = opts.tiles_x;
    td.tiles_y = opts.tiles_y;
    u32 vcount = td.vertex_count();
    td.heightmap.assign(vcount, 0.0f);
    td.cliff_level.assign(vcount, 0);
    td.tile_layer.assign(vcount, 0);          // layer 0 = dirt
    td.pathing.assign(vcount, 0);

    fs::path scene_dir = dest_dir / "scenes/scene_01";
    fs::create_directories(scene_dir, ec);
    if (!map::save_terrain(td, (scene_dir / "terrain.bin").string()))
        return "failed to write terrain.bin";

    // Empty placements (valid bytes: all counts zero).
    {
        map::SceneData empty;
        auto bytes = map::write_placements(empty);
        std::ofstream f(scene_dir / "placements.bin", std::ios::binary);
        if (!f) return "failed to write placements.bin";
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        if (!f) return "failed to write placements.bin";
    }

    // scene.json — camera bounds sized to the terrain (centered coords).
    {
        f32 hw = 0.5f * static_cast<f32>(opts.tiles_x) * td.tile_size;
        f32 hh = 0.5f * static_cast<f32>(opts.tiles_y) * td.tile_size;
        auto num = [](f32 v) { return std::to_string(static_cast<i32>(v)); };
        std::string s =
            "{\n"
            "    \"camera_bounds\": { \"min_x\": " + num(-hw) + ", \"min_y\": " + num(-hh) +
            ", \"max_x\": " + num(hw) + ", \"max_y\": " + num(hh) + " }\n"
            "}\n";
        if (!write_text(scene_dir / "scene.json", s)) return "failed to write scene.json";
    }

    log::info(TAG, "Created new map '{}' ({}x{}, {}) at {}",
              opts.id, opts.tiles_x, opts.tiles_y, opts.input_preset, dest_dir.string());
    return {};
}

} // namespace uldum::editor::map_bootstrap
