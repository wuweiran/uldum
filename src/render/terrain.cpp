#include "render/terrain.h"
#include "render/gpu_mesh.h"
#include "map/terrain_data.h"
#include "core/log.h"

#include <glm/geometric.hpp>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace uldum::render {

TerrainMesh build_terrain_mesh(VmaAllocator allocator, const map::TerrainData& td) {
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
                add_tri(v0, v3, v2);
                add_tri(v0, v1, v3);
                continue;
            }

            // ── Cliff wall tile: duplicate vertices below ────────────────
            auto ti = compute_tile_info(tx, ty);

            // Helper: add a corner vertex at grid position
            auto corner_v = [&](u32 ci, f32 base_z) -> u32 {
                u32 ix = (ci & 1) ? tx+1 : tx;
                u32 iy = (ci & 2) ? ty+1 : ty;
                return add_vert(td.vertex_world_x(ix),
                                td.vertex_world_y(iy),
                                base_z + td.height_at(ix, iy),
                                texcoord_at(ix, iy), ti);
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

                // Wall endpoints per-edge (midpoint or corner if ramp)
                u32 wa_h, wa_l, wb_h, wb_l;
                if (ramp_a) {
                    wa_h = corner_v(hi, high_z);
                    wa_l = corner_v(adj_a, low_z);
                } else {
                    wa_h = mid_v(edge_a, high_z);
                    wa_l = mid_v(edge_a, low_z);
                }
                if (ramp_b) {
                    wb_h = corner_v(hi, high_z);
                    wb_l = corner_v(adj_b, low_z);
                } else {
                    wb_h = mid_v(edge_b, high_z);
                    wb_l = mid_v(edge_b, low_z);
                }

                // High triangle (may be degenerate if ramp replaces endpoint with hi corner)
                u32 vh = corner_v(hi, high_z);
                if (!ramp_a || !ramp_b) {
                    add_tri(vh, wa_h, wb_h);
                }

                // Wall quad (duplicated vertices so wall normals don't blend with surface)
                if (!ramp_a || !ramp_b) {
                    glm::vec3 wall_n = glm::normalize(glm::vec3{
                        xm - vertices[vh].position.x,
                        ym - vertices[vh].position.y, 0.0f});
                    u32 d_wa_h = dup_v(wa_h); u32 d_wa_l = dup_v(wa_l);
                    u32 d_wb_h = dup_v(wb_h); u32 d_wb_l = dup_v(wb_l);
                    vertices[d_wa_h].normal = vertices[d_wa_l].normal = wall_n;
                    vertices[d_wb_h].normal = vertices[d_wb_l].normal = wall_n;

                    glm::vec3 cross = glm::cross(
                        vertices[d_wb_h].position - vertices[d_wa_h].position,
                        vertices[d_wa_l].position - vertices[d_wa_h].position);
                    if (glm::dot(cross, wall_n) >= 0) {
                        indices.push_back(d_wa_h); indices.push_back(d_wb_h); indices.push_back(d_wa_l);
                        indices.push_back(d_wb_h); indices.push_back(d_wb_l); indices.push_back(d_wa_l);
                    } else {
                        indices.push_back(d_wa_h); indices.push_back(d_wa_l); indices.push_back(d_wb_h);
                        indices.push_back(d_wb_h); indices.push_back(d_wa_l); indices.push_back(d_wb_l);
                    }
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
                u32 edge_hi, edge_lo; // edges perpendicular to the wall
                if (c[0] == cmax && c[1] == cmax)      { edge_hi = 0; edge_lo = 1; } // top high, wall at mid-Y
                else if (c[2] == cmax && c[3] == cmax)  { edge_hi = 1; edge_lo = 0; } // bottom high
                else if (c[0] == cmax && c[2] == cmax)  { edge_hi = 2; edge_lo = 3; } // left high
                else if (c[1] == cmax && c[3] == cmax)  { edge_hi = 3; edge_lo = 2; } // right high
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
                        vertices[d_ma_h].normal = vertices[d_ma_l].normal = wall_n;
                        vertices[d_mb_h].normal = vertices[d_mb_l].normal = wall_n;

                        glm::vec3 cross = glm::cross(
                            vertices[d_mb_h].position - vertices[d_ma_h].position,
                            vertices[d_ma_l].position - vertices[d_ma_h].position);
                        if (glm::dot(cross, wall_n) >= 0) {
                            indices.push_back(d_ma_h); indices.push_back(d_mb_h); indices.push_back(d_ma_l);
                            indices.push_back(d_mb_h); indices.push_back(d_mb_l); indices.push_back(d_ma_l);
                        } else {
                            indices.push_back(d_ma_h); indices.push_back(d_ma_l); indices.push_back(d_mb_h);
                            indices.push_back(d_mb_h); indices.push_back(d_ma_l); indices.push_back(d_mb_l);
                        }
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

                u32 d_wa_h = dup_v(wa_h); u32 d_wa_l = dup_v(wa_l);
                u32 d_wb_h = dup_v(wb_h); u32 d_wb_l = dup_v(wb_l);
                vertices[d_wa_h].normal = vertices[d_wa_l].normal = wall_n;
                vertices[d_wb_h].normal = vertices[d_wb_l].normal = wall_n;

                glm::vec3 cross = glm::cross(
                    vertices[d_wb_h].position - vertices[d_wa_h].position,
                    vertices[d_wa_l].position - vertices[d_wa_h].position);
                if (glm::dot(cross, wall_n) >= 0) {
                    indices.push_back(d_wa_h); indices.push_back(d_wb_h); indices.push_back(d_wa_l);
                    indices.push_back(d_wb_h); indices.push_back(d_wb_l); indices.push_back(d_wa_l);
                } else {
                    indices.push_back(d_wa_h); indices.push_back(d_wa_l); indices.push_back(d_wb_h);
                    indices.push_back(d_wb_h); indices.push_back(d_wa_l); indices.push_back(d_wb_l);
                }

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
                    u32 d_wa_h = dup_v(wa_h); u32 d_wa_l = dup_v(wa_l);
                    u32 d_wb_h = dup_v(wb_h); u32 d_wb_l = dup_v(wb_l);
                    vertices[d_wa_h].normal = vertices[d_wa_l].normal = wall_n;
                    vertices[d_wb_h].normal = vertices[d_wb_l].normal = wall_n;

                    glm::vec3 cross = glm::cross(
                        vertices[d_wb_h].position - vertices[d_wa_h].position,
                        vertices[d_wa_l].position - vertices[d_wa_h].position);
                    if (glm::dot(cross, wall_n) >= 0) {
                        indices.push_back(d_wa_h); indices.push_back(d_wb_h); indices.push_back(d_wa_l);
                        indices.push_back(d_wb_h); indices.push_back(d_wb_l); indices.push_back(d_wa_l);
                    } else {
                        indices.push_back(d_wa_h); indices.push_back(d_wa_l); indices.push_back(d_wb_h);
                        indices.push_back(d_wb_h); indices.push_back(d_wa_l); indices.push_back(d_wb_l);
                    }
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

    VkDeviceSize vb_size = vertices.size() * sizeof(TerrainVertex);
    VkDeviceSize ib_size = indices.size() * sizeof(u32);

    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buf_ci.size  = vb_size;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    vmaCreateBuffer(allocator, &buf_ci, &alloc_ci, &mesh.vertex_buffer, &mesh.vertex_alloc, nullptr);
    void* mapped = nullptr;
    vmaMapMemory(allocator, mesh.vertex_alloc, &mapped);
    std::memcpy(mapped, vertices.data(), vb_size);
    vmaUnmapMemory(allocator, mesh.vertex_alloc);

    buf_ci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    buf_ci.size  = ib_size;
    vmaCreateBuffer(allocator, &buf_ci, &alloc_ci, &mesh.index_buffer, &mesh.index_alloc, nullptr);
    vmaMapMemory(allocator, mesh.index_alloc, &mapped);
    std::memcpy(mapped, indices.data(), ib_size);
    vmaUnmapMemory(allocator, mesh.index_alloc);

    mesh.index_count  = static_cast<u32>(indices.size());
    mesh.vertex_count = static_cast<u32>(vertices.size());

    log::info("Terrain", "Built terrain mesh: {}x{} tiles, {} vertices, {} indices",
              td.tiles_x, td.tiles_y, mesh.vertex_count, mesh.index_count);

    return result;
}

void destroy_terrain_mesh(VmaAllocator allocator, TerrainMesh& mesh) {
    destroy_mesh(allocator, mesh.gpu_mesh);
    mesh = {};
}

} // namespace uldum::render
