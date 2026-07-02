#include "render/terrain.h"
#include "render/gpu_mesh.h"
#include "rhi/rhi.h"
#include "map/terrain_data.h"
#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace uldum::render {

TerrainMesh build_terrain_mesh(rhi::Rhi& rhi, const map::TerrainData& td) {
    if (!td.is_valid()) {
        log::error("Terrain", "Cannot build mesh from invalid TerrainData");
        return {};
    }

    std::vector<TerrainVertex> vertices;
    std::vector<u32> indices;
    vertices.reserve(td.tile_count() * 4 + td.tile_count() * 2);
    indices.reserve(td.tile_count() * 6 + td.tile_count() * 6);

    auto layer_at = [&](u32 ix, u32 iy) -> u32 {
        return td.tile_layer[iy * td.verts_x() + ix];
    };

    auto texcoord_at = [&](u32 ix, u32 iy) -> glm::vec2 {
        return {
            static_cast<f32>(ix) / static_cast<f32>(td.tiles_x),
            static_cast<f32>(iy) / static_cast<f32>(td.tiles_y)
        };
    };

    struct TileVertexInfo {
        u32 layer_corners;  // c0|(c1<<8)|(c2<<16)|(c3<<24)
        u32 case_info;      // reserved
    };

    auto compute_tile_info = [&](u32 tx, u32 ty) -> TileVertexInfo {
        u32 c[4] = {layer_at(tx, ty), layer_at(tx+1, ty),
                     layer_at(tx, ty+1), layer_at(tx+1, ty+1)};

        u32 layer_corners = (c[0] & 0xFF) | ((c[1] & 0xFF) << 8)
                          | ((c[2] & 0xFF) << 16) | ((c[3] & 0xFF) << 24);
        return {layer_corners, 0};
    };

    auto add_vert = [&](f32 x, f32 y, f32 z, glm::vec2 uv,
                         const TileVertexInfo& ti) -> u32 {
        u32 idx = static_cast<u32>(vertices.size());
        TerrainVertex v;
        v.position = {x, y, z};
        v.texcoord = uv;
        v.layer_corners = ti.layer_corners;
        v.case_info = ti.case_info;
        v.normal = {0, 0, 1};
        vertices.push_back(v);
        return idx;
    };

    // Ensure CCW winding (viewed from +Z)
    auto add_tri = [&](u32 a, u32 b, u32 c) {
        glm::vec3 cross = glm::cross(
            vertices[b].position - vertices[a].position,
            vertices[c].position - vertices[a].position);
        if (cross.z >= 0) {
            indices.push_back(a); indices.push_back(b); indices.push_back(c);
        } else {
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
        }
    };

    // Duplicate a vertex (for wall quads — separate from surface so normals don't blend)
    auto dup_v = [&](u32 src) -> u32 {
        u32 idx = static_cast<u32>(vertices.size());
        vertices.push_back(vertices[src]);
        return idx;
    };

    // Emit a cliff/chamfer wall quad from its four corner vertices: a_h/b_h
    // are the high (top) edge, a_l/b_l the low (bottom) edge. All four are
    // assigned `wall_n` (callers pass duplicated verts so the wall's flat
    // normal doesn't blend into the surrounding surface). Winding is chosen
    // so the two triangles face along wall_n. This block was copy-pasted at
    // every cliff configuration (1-high / 2-high diagonal / 3-high / 4-way);
    // only how the four verts and wall_n are derived differs per site.
    auto add_wall_quad = [&](u32 a_h, u32 a_l, u32 b_h, u32 b_l, glm::vec3 wall_n) {
        vertices[a_h].normal = vertices[a_l].normal = wall_n;
        vertices[b_h].normal = vertices[b_l].normal = wall_n;
        glm::vec3 cross = glm::cross(
            vertices[b_h].position - vertices[a_h].position,
            vertices[a_l].position - vertices[a_h].position);
        if (glm::dot(cross, wall_n) >= 0) {
            indices.push_back(a_h); indices.push_back(b_h); indices.push_back(a_l);
            indices.push_back(b_h); indices.push_back(b_l); indices.push_back(a_l);
        } else {
            indices.push_back(a_h); indices.push_back(a_l); indices.push_back(b_h);
            indices.push_back(b_h); indices.push_back(a_l); indices.push_back(b_l);
        }
    };

    // ── Shared vertex grid (lazy creation) ─────────────────────────────
    // Flat and ramp tiles share vertices. Each grid vertex stores the tile
    // data for tile (ix, iy) — used when it's the provoking vertex.
    // Cliff wall tiles create their own duplicate vertices instead.
    std::vector<u32> grid(td.verts_x() * td.verts_y(), UINT32_MAX);
    auto grid_vert = [&](u32 ix, u32 iy) -> u32 {
        u32& idx = grid[iy * td.verts_x() + ix];
        if (idx == UINT32_MAX) {
            idx = static_cast<u32>(vertices.size());
            TerrainVertex v;
            v.position = {
                td.vertex_world_x(ix),
                td.vertex_world_y(iy),
                td.world_z_at(ix, iy)
            };
            v.texcoord = texcoord_at(ix, iy);
            v.normal = {0, 0, 1};
            if (ix < td.tiles_x && iy < td.tiles_y) {
                auto ti = compute_tile_info(ix, iy);
                v.layer_corners = ti.layer_corners;
                v.case_info = ti.case_info;
            }
            vertices.push_back(v);
        }
        return idx;
    };

    // ── Surface tiles ────────────────────────────────────────────────────

    // Helper: interpolate heightmap, texcoord, splatmap at fractional grid position
    auto lerp_height = [&](f32 fx, f32 fy) -> f32 {
        u32 ix = static_cast<u32>(fx), iy = static_cast<u32>(fy);
        ix = std::min(ix, td.tiles_x - 1);
        iy = std::min(iy, td.tiles_y - 1);
        f32 lx = fx - static_cast<f32>(ix), ly = fy - static_cast<f32>(iy);
        return td.height_at(ix, iy) * (1-lx)*(1-ly) + td.height_at(ix+1, iy) * lx*(1-ly)
             + td.height_at(ix, iy+1) * (1-lx)*ly + td.height_at(ix+1, iy+1) * lx*ly;
    };
    // Check if the neighboring tile across an edge is a ramp.
    // edge: 0=top(y-1), 1=bottom(y+1), 2=left(x-1), 3=right(x+1)
    auto neighbor_is_ramp = [&](u32 tx, u32 ty, u32 edge) -> bool {
        i32 nx = static_cast<i32>(tx), ny = static_cast<i32>(ty);
        switch (edge) {
            case 0: ny -= 1; break;  // top neighbor
            case 1: ny += 1; break;  // bottom neighbor
            case 2: nx -= 1; break;  // left neighbor
            default: nx += 1; break; // right neighbor
        }
        if (nx < 0 || ny < 0 || nx >= static_cast<i32>(td.tiles_x) || ny >= static_cast<i32>(td.tiles_y))
            return false;
        u32 ux = static_cast<u32>(nx), uy = static_cast<u32>(ny);
        u8 nc[4] = {
            td.cliff_at(ux, uy), td.cliff_at(ux+1, uy),
            td.cliff_at(ux, uy+1), td.cliff_at(ux+1, uy+1)
        };
        u8 nmin = std::min({nc[0], nc[1], nc[2], nc[3]});
        u8 nmax = std::max({nc[0], nc[1], nc[2], nc[3]});
        return (nmin != nmax) && (nmax - nmin == 1) &&
            (td.pathing_at(ux, uy) & map::PATHING_RAMP) &&
            (td.pathing_at(ux+1, uy) & map::PATHING_RAMP) &&
            (td.pathing_at(ux, uy+1) & map::PATHING_RAMP) &&
            (td.pathing_at(ux+1, uy+1) & map::PATHING_RAMP);
    };

    for (u32 ty = 0; ty < td.tiles_y; ++ty) {
        for (u32 tx = 0; tx < td.tiles_x; ++tx) {
            // Corner indices: TL=0, TR=1, BL=2, BR=3
            u8 c[4] = {
                td.cliff_at(tx, ty), td.cliff_at(tx+1, ty),
                td.cliff_at(tx, ty+1), td.cliff_at(tx+1, ty+1)
            };
            u8 cmin = std::min({c[0], c[1], c[2], c[3]});
            u8 cmax = std::max({c[0], c[1], c[2], c[3]});

            f32 x0 = td.vertex_world_x(tx);
            f32 x1 = td.vertex_world_x(tx + 1);
            f32 xm = (x0 + x1) * 0.5f;
            f32 y0 = td.vertex_world_y(ty);
            f32 y1 = td.vertex_world_y(ty + 1);
            f32 ym = (y0 + y1) * 0.5f;

            f32 high_z = static_cast<f32>(cmax) * td.layer_height;
            f32 low_z  = static_cast<f32>(cmin) * td.layer_height;

            u32 high_count = 0;
            for (u32 i = 0; i < 4; ++i) { if (c[i] == cmax) high_count++; }


            // Flat or ramp tile: use shared grid vertices
            bool is_ramp = (cmin != cmax) &&
                (td.pathing_at(tx, ty) & map::PATHING_RAMP) &&
                (td.pathing_at(tx+1, ty) & map::PATHING_RAMP) &&
                (td.pathing_at(tx, ty+1) & map::PATHING_RAMP) &&
                (td.pathing_at(tx+1, ty+1) & map::PATHING_RAMP) &&
                (cmax - cmin == 1);

            if (cmin == cmax || is_ramp) {
                u32 v0 = grid_vert(tx, ty);
                u32 v1 = grid_vert(tx + 1, ty);
                u32 v2 = grid_vert(tx, ty + 1);
                u32 v3 = grid_vert(tx + 1, ty + 1);
                // `layer_corners` is a `flat`-qualified vertex output that
                // the SDF transition shader reads from the provoking
                // vertex of each triangle. Of the four shared grid
                // vertices on this tile, only v0 (the SW corner) carries
                // THIS tile's data — v1/v2/v3 belong to neighboring
                // tiles. To make the shader read the right value we have
                // to make v0 the provoking vertex.
                //
                //   Vulkan: provoking = first vertex of the triangle.
                //           → emit (v0, ..., ...) so v0 is first.
                //   OpenGL ES: provoking = last vertex (no API to change).
                //           → emit (..., ..., v0) so v0 is last.
                //
                // Same triangles in both cases (same set of three
                // vertices, same CCW winding); only the cyclic rotation
                // differs. Cliff-wall paths below use add_vert (one
                // dedicated vertex per tile, all four carrying the same
                // layer_corners) so provoking-vertex selection doesn't
                // matter there.
#if defined(ULDUM_BACKEND_GLES)
                add_tri(v3, v2, v0);
                add_tri(v1, v3, v0);
#else
                add_tri(v0, v3, v2);
                add_tri(v0, v1, v3);
#endif
                continue;
            }

            // ── Cliff wall tile: duplicate vertices below ────────────────
            auto ti = compute_tile_info(tx, ty);

            // Per-tile corner cache. corner_v is called repeatedly for the
            // same (corner, height-band) across a tile's surface fans — e.g.
            // a ramp-collapse endpoint and the low-surface fan both ask for
            // the same low corner, and the both-ramps apex asks for the same
            // adjacent low corners the pentagon fan uses. Minting a fresh
            // vertex each time left co-located duplicates (and orphans) in the
            // buffer. Sharing them is safe: corner verts are only ever surface
            // verts or dup_v SOURCES — never handed straight to a wall — so the
            // wall/surface index split that keeps cliff normals from blending
            // (the hard-edge shadow fix) is preserved by dup_v downstream.
            // Key = corner<<1 | band (0=low at cmin, 1=high at cmax); a cliff
            // tile has cmin != cmax so the two bands never collide.
            u32 corner_cache[8];
            for (u32 i = 0; i < 8; ++i) corner_cache[i] = UINT32_MAX;

            // Helper: add a corner vertex at grid position
            auto corner_v = [&](u32 ci, f32 base_z) -> u32 {
                u32 band = (base_z >= high_z) ? 1u : 0u;
                u32& slot = corner_cache[(ci << 1) | band];
                if (slot != UINT32_MAX) return slot;
                u32 ix = (ci & 1) ? tx+1 : tx;
                u32 iy = (ci & 2) ? ty+1 : ty;
                slot = add_vert(td.vertex_world_x(ix),
                                td.vertex_world_y(iy),
                                base_z + td.height_at(ix, iy),
                                texcoord_at(ix, iy), ti);
                return slot;
            };

            // Helper: add a midpoint vertex on a tile edge
            auto mid_v = [&](u32 edge, f32 base_z) -> u32 {
                f32 mx, my, gfx, gfy;
                switch (edge) {
                    case 0: mx = xm; my = y0; gfx = tx + 0.5f; gfy = static_cast<f32>(ty); break;
                    case 1: mx = xm; my = y1; gfx = tx + 0.5f; gfy = static_cast<f32>(ty + 1); break;
                    case 2: mx = x0; my = ym; gfx = static_cast<f32>(tx); gfy = ty + 0.5f; break;
                    default: mx = x1; my = ym; gfx = static_cast<f32>(tx + 1); gfy = ty + 0.5f; break;
                }
                return add_vert(mx, my, base_z + lerp_height(gfx, gfy),
                                {gfx / td.tiles_x, gfy / td.tiles_y}, ti);
            };

            if (high_count == 1) {
                // 1 high corner: small triangle at high, rest at low.
                u32 hi = 0;
                for (u32 i = 0; i < 4; ++i) { if (c[i] == cmax) { hi = i; break; } }

                u32 edge_a, edge_b;
                switch (hi) {
                    case 0: edge_a = 0; edge_b = 2; break;
                    case 1: edge_a = 0; edge_b = 3; break;
                    case 2: edge_a = 1; edge_b = 2; break;
                    default: edge_a = 1; edge_b = 3; break;
                }

                bool ramp_a = neighbor_is_ramp(tx, ty, edge_a);
                bool ramp_b = neighbor_is_ramp(tx, ty, edge_b);

                // Adjacent low corner on each edge
                u32 adj_a, adj_b;
                switch (hi) {
                    case 0: adj_a = 1; adj_b = 2; break; // TL: top→TR, left→BL
                    case 1: adj_a = 0; adj_b = 3; break; // TR: top→TL, right→BR
                    case 2: adj_a = 3; adj_b = 0; break; // BL: bottom→BR, left→TL
                    default: adj_a = 2; adj_b = 1; break; // BR: bottom→BL, right→TR
                }

                // Both-ramp case takes a different decomposition than the standard
                // 1-high cliff. The standard form has a small high plateau (the
                // high triangle at z=H) extending into the tile, which leaves
                // triangular gaps along the shared edges with ramp neighbors —
                // their surfaces interpolate linearly from high to low while our
                // plateau-then-vertical-drop profile sits above the ramp on one
                // half of each edge. The spire form has no plateau: the high
                // corner is a single apex with three cliff faces (one along each
                // adjacent tile edge, one chamfer between the midpoints) fanning
                // down to a low pentagon. The full-edge walls share the
                // hi-to-adj diagonal with the ramp neighbor, so the geometry
                // closes cleanly.
                bool both_ramps = ramp_a && ramp_b;

                u32 vh = corner_v(hi, high_z);
                u32 wa_l, wb_l;  // wall low endpoints, also used by the low pentagon fan

                if (both_ramps) {
                    wa_l = mid_v(edge_a, low_z);
                    wb_l = mid_v(edge_b, low_z);
                    u32 v_adj_a = corner_v(adj_a, low_z);
                    u32 v_adj_b = corner_v(adj_b, low_z);

                    // Three cliff faces fanning from the high apex down to the
                    // low pentagon: one along each adjacent tile edge, plus
                    // the diagonal chamfer between the two midpoints. Each
                    // gets its geometric normal flipped (if needed) to face
                    // the tile interior — that's where the player views from.
                    const glm::vec3 interior{xm, ym, low_z};
                    const u32 walls[3][3] = {
                        {vh, v_adj_a, wa_l},  // edge_a face (full edge)
                        {vh, v_adj_b, wb_l},  // edge_b face (full edge)
                        {vh, wa_l, wb_l},     // chamfer
                    };
                    for (auto wall : walls) {
                        u32 v0 = wall[0], v1 = wall[1], v2 = wall[2];
                        glm::vec3 n = glm::normalize(glm::cross(
                            vertices[v1].position - vertices[v0].position,
                            vertices[v2].position - vertices[v0].position));
                        glm::vec3 centroid = (vertices[v0].position
                                            + vertices[v1].position
                                            + vertices[v2].position) / 3.0f;
                        if (glm::dot(n, interior - centroid) < 0) { std::swap(v1, v2); n = -n; }
                        u32 d0 = dup_v(v0), d1 = dup_v(v1), d2 = dup_v(v2);
                        vertices[d0].normal = vertices[d1].normal = vertices[d2].normal = n;
                        indices.push_back(d0); indices.push_back(d1); indices.push_back(d2);
                    }
                } else {
                    // Standard 1-high cliff. Corner-collapse trick when one neighbor
                    // is a ramp (yields a slope segment matching the ramp).
                    u32 wa_h, wb_h;
                    if (ramp_a) {
                        wa_h = vh;
                        wa_l = corner_v(adj_a, low_z);
                    } else {
                        wa_h = mid_v(edge_a, high_z);
                        wa_l = mid_v(edge_a, low_z);
                    }
                    if (ramp_b) {
                        wb_h = vh;
                        wb_l = corner_v(adj_b, low_z);
                    } else {
                        wb_h = mid_v(edge_b, high_z);
                        wb_l = mid_v(edge_b, low_z);
                    }

                    // High triangle (degenerate when one neighbor is a ramp; harmless).
                    add_tri(vh, wa_h, wb_h);

                    // Chamfer wall (quad), duplicated for separate normals.
                    glm::vec3 wall_n = glm::normalize(glm::vec3{
                        xm - vertices[vh].position.x,
                        ym - vertices[vh].position.y, 0.0f});
                    add_wall_quad(dup_v(wa_h), dup_v(wa_l), dup_v(wb_h), dup_v(wb_l), wall_n);
                }

                // Low surface: substitute midpoints with wall endpoints
                u32 vl[4];
                for (u32 i = 0; i < 4; ++i) vl[i] = corner_v(i, low_z);
                u32 fan[5];
                switch (hi) {
                    case 0: // TL high. edge_a=top(0), edge_b=left(2)
                        fan[0] = vl[1]; fan[1] = vl[3]; fan[2] = vl[2];
                        fan[3] = wb_l; fan[4] = wa_l; break;
                    case 1: // TR high. edge_a=top(0), edge_b=right(3)
                        fan[0] = vl[0]; fan[1] = wa_l; fan[2] = wb_l;
                        fan[3] = vl[3]; fan[4] = vl[2]; break;
                    case 2: // BL high. edge_a=bottom(1), edge_b=left(2)
                        fan[0] = vl[0]; fan[1] = vl[1]; fan[2] = vl[3];
                        fan[3] = wa_l; fan[4] = wb_l; break;
                    default: // BR high. edge_a=bottom(1), edge_b=right(3)
                        fan[0] = vl[0]; fan[1] = vl[1]; fan[2] = wb_l;
                        fan[3] = wa_l; fan[4] = vl[2]; break;
                }
                for (u32 i = 1; i + 1 < 5; ++i) {
                    add_tri(fan[0], fan[i], fan[i+1]);
                }

            } else if (high_count == 2) {
                // 2 high corners on one edge: wall bisects the tile.
                // Find which edge has both high corners.
                // Edges: top={0,1}, bottom={2,3}, left={0,2}, right={1,3}
                u32 edge_hi; // edge perpendicular to the wall, on the high side
                if (c[0] == cmax && c[1] == cmax)      { edge_hi = 0; } // top high, wall at mid-Y
                else if (c[2] == cmax && c[3] == cmax)  { edge_hi = 1; } // bottom high
                else if (c[0] == cmax && c[2] == cmax)  { edge_hi = 2; } // left high
                else if (c[1] == cmax && c[3] == cmax)  { edge_hi = 3; } // right high
                else {
                    // Diagonal pair (TL+BR or TR+BL) — render as two 1-high triangles
                    // Each high corner gets a small triangle + wall, like the 1-high case
                    // but we do it twice for each high corner.
                    u32 hi_a, hi_b;
                    if (c[0] == cmax && c[3] == cmax) { hi_a = 0; hi_b = 3; } // TL + BR
                    else { hi_a = 1; hi_b = 2; } // TR + BL

                    // Render each high corner as a small high triangle with midpoint wall
                    for (u32 pass = 0; pass < 2; ++pass) {
                        u32 hi = (pass == 0) ? hi_a : hi_b;

                        u32 ea, eb;
                        switch (hi) {
                            case 0: ea = 0; eb = 2; break;
                            case 1: ea = 0; eb = 3; break;
                            case 2: ea = 1; eb = 2; break;
                            default: ea = 1; eb = 3; break;
                        }

                        u32 vh = corner_v(hi, high_z);
                        u32 ma_h = mid_v(ea, high_z);
                        u32 mb_h = mid_v(eb, high_z);
                        add_tri(vh, ma_h, mb_h);

                        // Wall
                        u32 d_ma_h = dup_v(ma_h); u32 d_ma_l = mid_v(ea, low_z);
                        u32 d_mb_h = dup_v(mb_h); u32 d_mb_l = mid_v(eb, low_z);
                        glm::vec3 wall_n = glm::normalize(glm::vec3{
                            xm - vertices[vh].position.x,
                            ym - vertices[vh].position.y, 0.0f});
                        add_wall_quad(d_ma_h, d_ma_l, d_mb_h, d_mb_l, wall_n);
                    }

                    // Low center: 4 midpoints + 2 low corners form the remaining surface
                    u32 vl_a = corner_v((hi_a == 0) ? 1 : 0, low_z);  // the two low corners
                    u32 vl_b = corner_v((hi_a == 0) ? 2 : 3, low_z);

                    // Fan from center-ish. Two low corners + 4 midpoints form a hexagon.
                    // Triangulate from one low corner.
                    if (hi_a == 0 && hi_b == 3) {
                        // TL+BR high, TR(1) and BL(2) low
                        // Hex CCW from TR: TR, m_right, m_bot, BL, m_left, m_top
                        u32 hex[6] = {vl_a, mid_v(3, low_z), mid_v(1, low_z),
                                      vl_b, mid_v(2, low_z), mid_v(0, low_z)};
                        for (u32 i = 1; i + 1 < 6; ++i) add_tri(hex[0], hex[i], hex[i+1]);
                    } else {
                        // TR+BL high, TL(0) and BR(3) low
                        // Hex CCW from TL: TL, m_top, m_right, BR, m_bot, m_left
                        u32 hex[6] = {vl_a, mid_v(0, low_z), mid_v(3, low_z),
                                      vl_b, mid_v(1, low_z), mid_v(2, low_z)};
                        for (u32 i = 1; i + 1 < 6; ++i) add_tri(hex[0], hex[i], hex[i+1]);
                    }
                    continue;
                }

                u32 wall_edge_a, wall_edge_b;
                if (edge_hi == 0 || edge_hi == 1) { wall_edge_a = 2; wall_edge_b = 3; }
                else { wall_edge_a = 0; wall_edge_b = 1; }

                bool ramp_a = neighbor_is_ramp(tx, ty, wall_edge_a);
                bool ramp_b = neighbor_is_ramp(tx, ty, wall_edge_b);

                // Find hi/lo corners on each wall edge
                // Each wall edge has one high corner and one low corner
                auto edge_hi_lo = [&](u32 edge) -> std::pair<u32, u32> {
                    u32 c0, c1;
                    switch (edge) {
                        case 0: c0 = 0; c1 = 1; break; // top: TL, TR
                        case 1: c0 = 2; c1 = 3; break; // bottom: BL, BR
                        case 2: c0 = 0; c1 = 2; break; // left: TL, BL
                        default: c0 = 1; c1 = 3; break; // right: TR, BR
                    }
                    return (c[c0] == cmax) ? std::pair{c0, c1} : std::pair{c1, c0};
                };

                auto [a_hi_ci, a_lo_ci] = edge_hi_lo(wall_edge_a);
                auto [b_hi_ci, b_lo_ci] = edge_hi_lo(wall_edge_b);

                // Wall endpoint vertices: midpoint (normal) or corner (ramp neighbor)
                u32 wa_h, wa_l, wb_h, wb_l;
                if (ramp_a) {
                    wa_h = corner_v(a_hi_ci, high_z);
                    wa_l = corner_v(a_lo_ci, low_z);
                } else {
                    wa_h = mid_v(wall_edge_a, high_z);
                    wa_l = mid_v(wall_edge_a, low_z);
                }
                if (ramp_b) {
                    wb_h = corner_v(b_hi_ci, high_z);
                    wb_l = corner_v(b_lo_ci, low_z);
                } else {
                    wb_h = mid_v(wall_edge_b, high_z);
                    wb_l = mid_v(wall_edge_b, low_z);
                }

                // High surface
                u32 hi_corners[2];
                u32 n_hi = 0;
                for (u32 i = 0; i < 4; ++i) { if (c[i] == cmax) hi_corners[n_hi++] = i; }
                u32 vh0 = corner_v(hi_corners[0], high_z);
                u32 vh1 = corner_v(hi_corners[1], high_z);

                if (ramp_a && ramp_b) {
                    // Both ramps: high surface is just the two high corners (slope handles rest)
                    // Wall quad IS the full tile slope
                } else if (ramp_a) {
                    // wa_h coincides with one hi corner → triangle
                    add_tri(vh0, vh1, wb_h);
                } else if (ramp_b) {
                    add_tri(vh0, vh1, wa_h);
                } else {
                    add_tri(vh0, vh1, wa_h);
                    add_tri(vh1, wb_h, wa_h);
                }

                // Low surface
                u32 lo_corners[2];
                u32 n_lo = 0;
                for (u32 i = 0; i < 4; ++i) { if (c[i] != cmax) lo_corners[n_lo++] = i; }
                u32 vl0 = corner_v(lo_corners[0], low_z);
                u32 vl1 = corner_v(lo_corners[1], low_z);

                if (ramp_a && ramp_b) {
                    // Both ramps: low surface is just the two low corners
                } else if (ramp_a) {
                    add_tri(vl0, vl1, wb_l);
                } else if (ramp_b) {
                    add_tri(vl0, vl1, wa_l);
                } else {
                    add_tri(vl0, vl1, wa_l);
                    add_tri(vl1, wb_l, wa_l);
                }

                // Wall/slope quad connecting high side to low side
                glm::vec3 wall_center = (vertices[wa_h].position + vertices[wb_h].position) * 0.5f;
                glm::vec3 low_center{0, 0, 0};
                for (u32 i = 0; i < 4; ++i) { if (c[i] != cmax) {
                    low_center = glm::vec3{td.vertex_world_x((i & 1) ? tx+1 : tx),
                                           td.vertex_world_y((i & 2) ? ty+1 : ty), 0};
                    break;
                }}
                glm::vec3 wall_n = glm::normalize(glm::vec3{
                    low_center.x - wall_center.x,
                    low_center.y - wall_center.y, 0.0f});

                add_wall_quad(dup_v(wa_h), dup_v(wa_l), dup_v(wb_h), dup_v(wb_l), wall_n);

            } else if (high_count == 3) {
                // 3 high: same as 1-low. Small triangle at low, rest high.
                u32 lo = 0;
                for (u32 i = 0; i < 4; ++i) { if (c[i] != cmax) { lo = i; break; } }

                u32 edge_a, edge_b;
                switch (lo) {
                    case 0: edge_a = 0; edge_b = 2; break;
                    case 1: edge_a = 0; edge_b = 3; break;
                    case 2: edge_a = 1; edge_b = 2; break;
                    default: edge_a = 1; edge_b = 3; break;
                }

                bool ramp_a = neighbor_is_ramp(tx, ty, edge_a);
                bool ramp_b = neighbor_is_ramp(tx, ty, edge_b);

                // Adjacent high corner on each edge (the other corner besides lo)
                u32 adj_a, adj_b;
                switch (lo) {
                    case 0: adj_a = 1; adj_b = 2; break; // TL: top→TR, left→BL
                    case 1: adj_a = 0; adj_b = 3; break; // TR: top→TL, right→BR
                    case 2: adj_a = 3; adj_b = 0; break; // BL: bottom→BR, left→TL
                    default: adj_a = 2; adj_b = 1; break; // BR: bottom→BL, right→TR
                }

                // Wall endpoints per-edge (midpoint or corner if ramp)
                u32 wa_h, wa_l, wb_h, wb_l;
                if (ramp_a) {
                    wa_h = corner_v(adj_a, high_z);
                    wa_l = corner_v(lo, low_z);
                } else {
                    wa_h = mid_v(edge_a, high_z);
                    wa_l = mid_v(edge_a, low_z);
                }
                if (ramp_b) {
                    wb_h = corner_v(adj_b, high_z);
                    wb_l = corner_v(lo, low_z);
                } else {
                    wb_h = mid_v(edge_b, high_z);
                    wb_l = mid_v(edge_b, low_z);
                }

                // Low triangle (may be degenerate if ramp replaces endpoint with lo corner)
                u32 vlo = corner_v(lo, low_z);
                if (!ramp_a && !ramp_b) {
                    add_tri(vlo, wa_l, wb_l);
                }

                // Wall quad (duplicated vertices so wall normals don't blend with surface)
                if (!ramp_a || !ramp_b) {
                    glm::vec3 wall_n = glm::normalize(glm::vec3{
                        vertices[vlo].position.x - xm,
                        vertices[vlo].position.y - ym, 0.0f});
                    add_wall_quad(dup_v(wa_h), dup_v(wa_l), dup_v(wb_h), dup_v(wb_l), wall_n);
                }

                // High surface: substitute midpoints with wall endpoints
                u32 vh[4];
                for (u32 i = 0; i < 4; ++i) vh[i] = corner_v(i, high_z);
                u32 fan[5];
                switch (lo) {
                    case 0: // TL low. edge_a=top(0), edge_b=left(2)
                        fan[0] = vh[1]; fan[1] = vh[3]; fan[2] = vh[2];
                        fan[3] = wb_h; fan[4] = wa_h; break;
                    case 1: // TR low. edge_a=top(0), edge_b=right(3)
                        fan[0] = vh[0]; fan[1] = wa_h; fan[2] = wb_h;
                        fan[3] = vh[3]; fan[4] = vh[2]; break;
                    case 2: // BL low. edge_a=bottom(1), edge_b=left(2)
                        fan[0] = vh[0]; fan[1] = vh[1]; fan[2] = vh[3];
                        fan[3] = wa_h; fan[4] = wb_h; break;
                    default: // BR low. edge_a=bottom(1), edge_b=right(3)
                        fan[0] = vh[0]; fan[1] = vh[1]; fan[2] = wb_h;
                        fan[3] = wa_h; fan[4] = vh[2]; break;
                }
                for (u32 i = 1; i + 1 < 5; ++i) {
                    add_tri(fan[0], fan[i], fan[i+1]);
                }

            } else {
                // high_count == 4 but cmin != cmax shouldn't happen
                // Fall through to full quad at max
                u32 v0 = corner_v(0, high_z), v1 = corner_v(1, high_z);
                u32 v2 = corner_v(2, high_z), v3 = corner_v(3, high_z);
                add_tri(v0, v1, v2);
                add_tri(v1, v3, v2);
            }
        }
    }

    // Smooth normals: standard per-vertex averaging of all adjacent face normals.
    {
        u32 vc = static_cast<u32>(vertices.size());
        std::vector<glm::vec3> normal_accum(vc, glm::vec3{0.0f});

        for (u32 i = 0; i + 2 < static_cast<u32>(indices.size()); i += 3) {
            u32 ia = indices[i], ib = indices[i+1], ic = indices[i+2];
            glm::vec3 face_n = glm::cross(
                vertices[ib].position - vertices[ia].position,
                vertices[ic].position - vertices[ia].position);
            normal_accum[ia] += face_n;
            normal_accum[ib] += face_n;
            normal_accum[ic] += face_n;
        }

        for (u32 i = 0; i < vc; ++i) {
            f32 len = glm::length(normal_accum[i]);
            vertices[i].normal = (len > 0.001f) ? normal_accum[i] / len : glm::vec3{0, 0, 1};
        }
    }

    // Water is a tile type (splatmap layer 3 = water). No separate water geometry.

    // ── Upload to GPU ────────────────────────────────────────────────────

    TerrainMesh result;
    auto& mesh = result.gpu_mesh;

    u64 vb_size = vertices.size() * sizeof(TerrainVertex);
    u64 ib_size = indices.size() * sizeof(u32);

    {
        rhi::BufferDesc d{};
        d.size   = vb_size;
        d.usage  = rhi::BufferUsage::Vertex;
        d.memory = rhi::MemoryUsage::HostSequential;
        mesh.vertex_buffer = rhi.create_buffer(d);
    }
    if (void* dst = rhi.mapped_ptr(mesh.vertex_buffer)) {
        std::memcpy(dst, vertices.data(), vb_size);
    }

    {
        rhi::BufferDesc d{};
        d.size   = ib_size;
        d.usage  = rhi::BufferUsage::Index;
        d.memory = rhi::MemoryUsage::HostSequential;
        mesh.index_buffer = rhi.create_buffer(d);
    }
    if (void* dst = rhi.mapped_ptr(mesh.index_buffer)) {
        std::memcpy(dst, indices.data(), ib_size);
    }

    mesh.index_count  = static_cast<u32>(indices.size());
    mesh.vertex_count = static_cast<u32>(vertices.size());

    log::info("Terrain", "Built terrain mesh: {}x{} tiles, {} vertices, {} indices",
              td.tiles_x, td.tiles_y, mesh.vertex_count, mesh.index_count);

    return result;
}

void destroy_terrain_mesh(rhi::Rhi& rhi, TerrainMesh& mesh) {
    destroy_mesh(rhi, mesh.gpu_mesh);
    mesh = {};
}

} // namespace uldum::render
