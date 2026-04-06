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

    auto splat_at = [&](u32 ix, u32 iy) -> glm::vec4 {
        u32 idx = iy * td.verts_x() + ix;
        return {
            static_cast<f32>(td.splatmap[0][idx]) / 255.0f,
            static_cast<f32>(td.splatmap[1][idx]) / 255.0f,
            static_cast<f32>(td.splatmap[2][idx]) / 255.0f,
            static_cast<f32>(td.splatmap[3][idx]) / 255.0f
        };
    };

    auto texcoord_at = [&](u32 ix, u32 iy) -> glm::vec2 {
        return {
            static_cast<f32>(ix) / static_cast<f32>(td.tiles_x),
            static_cast<f32>(iy) / static_cast<f32>(td.tiles_y)
        };
    };

    auto add_vert = [&](f32 x, f32 y, f32 z, glm::vec2 uv, glm::vec4 splat) -> u32 {
        u32 idx = static_cast<u32>(vertices.size());
        TerrainVertex v;
        v.position = {x, y, z};
        v.texcoord = uv;
        v.splat_weights = splat;
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

    // ── Surface tiles ────────────────────────────────────────────────────
    // Each tile has its own 4 vertices. Each corner's Z uses that corner's
    // own cliff_level * layer_height + heightmap. This means at a cliff
    // boundary, the high-side tile's corner is at high Z and the adjacent
    // low-side tile's corner at the same XY position is at low Z.
    // The sheer drop between them is filled by cliff wall quads.

    // Helper: interpolate heightmap, texcoord, splatmap at fractional grid position
    auto lerp_height = [&](f32 fx, f32 fy) -> f32 {
        u32 ix = static_cast<u32>(fx), iy = static_cast<u32>(fy);
        ix = std::min(ix, td.tiles_x - 1);
        iy = std::min(iy, td.tiles_y - 1);
        f32 lx = fx - static_cast<f32>(ix), ly = fy - static_cast<f32>(iy);
        return td.height_at(ix, iy) * (1-lx)*(1-ly) + td.height_at(ix+1, iy) * lx*(1-ly)
             + td.height_at(ix, iy+1) * (1-lx)*ly + td.height_at(ix+1, iy+1) * lx*ly;
    };
    auto lerp_splat = [&](f32 fx, f32 fy) -> glm::vec4 {
        u32 ix = static_cast<u32>(fx), iy = static_cast<u32>(fy);
        ix = std::min(ix, td.tiles_x - 1);
        iy = std::min(iy, td.tiles_y - 1);
        f32 lx = fx - static_cast<f32>(ix), ly = fy - static_cast<f32>(iy);
        return splat_at(ix, iy) * (1-lx)*(1-ly) + splat_at(ix+1, iy) * lx*(1-ly)
             + splat_at(ix, iy+1) * (1-lx)*ly + splat_at(ix+1, iy+1) * lx*ly;
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

            f32 x0 = static_cast<f32>(tx) * td.tile_size;
            f32 x1 = static_cast<f32>(tx + 1) * td.tile_size;
            f32 xm = (x0 + x1) * 0.5f;
            f32 y0 = static_cast<f32>(ty) * td.tile_size;
            f32 y1 = static_cast<f32>(ty + 1) * td.tile_size;
            f32 ym = (y0 + y1) * 0.5f;

            f32 high_z = static_cast<f32>(cmax) * td.layer_height;
            f32 low_z  = static_cast<f32>(cmin) * td.layer_height;

            u32 high_count = 0;
            for (u32 i = 0; i < 4; ++i) { if (c[i] == cmax) high_count++; }


            // Check if this tile is a ramp: different cliff levels + all corners have RAMP flag
            bool is_ramp = (cmin != cmax) &&
                (td.pathing_at(tx, ty) & map::PATHING_RAMP) &&
                (td.pathing_at(tx+1, ty) & map::PATHING_RAMP) &&
                (td.pathing_at(tx, ty+1) & map::PATHING_RAMP) &&
                (td.pathing_at(tx+1, ty+1) & map::PATHING_RAMP) &&
                (cmax - cmin == 1);  // ramps only between adjacent levels

            if (is_ramp) {
                // Ramp tile: each vertex at its own cliff level, creating a slope
                u32 v[4];
                u32 gx[4] = {tx, tx+1, tx, tx+1};
                u32 gy[4] = {ty, ty, ty+1, ty+1};
                for (u32 i = 0; i < 4; ++i) {
                    f32 x = static_cast<f32>(gx[i]) * td.tile_size;
                    f32 y = static_cast<f32>(gy[i]) * td.tile_size;
                    f32 z = static_cast<f32>(c[i]) * td.layer_height + td.height_at(gx[i], gy[i]);
                    v[i] = add_vert(x, y, z, texcoord_at(gx[i], gy[i]), splat_at(gx[i], gy[i]));
                }
                add_tri(v[0], v[1], v[2]);
                add_tri(v[1], v[3], v[2]);
                continue;  // skip cliff logic
            }

            // Helper: add a corner vertex at grid position
            auto corner_v = [&](u32 ci, f32 base_z) -> u32 {
                u32 ix = (ci & 1) ? tx+1 : tx;
                u32 iy = (ci & 2) ? ty+1 : ty;
                return add_vert(static_cast<f32>(ix) * td.tile_size,
                                static_cast<f32>(iy) * td.tile_size,
                                base_z + td.height_at(ix, iy),
                                texcoord_at(ix, iy), splat_at(ix, iy));
            };

            // Helper: add a midpoint vertex on a tile edge
            // edge 0=top(TL-TR), 1=bottom(BL-BR), 2=left(TL-BL), 3=right(TR-BR)
            auto mid_v = [&](u32 edge, f32 base_z) -> u32 {
                f32 mx, my, gfx, gfy;
                switch (edge) {
                    case 0: mx = xm; my = y0; gfx = tx + 0.5f; gfy = static_cast<f32>(ty); break;
                    case 1: mx = xm; my = y1; gfx = tx + 0.5f; gfy = static_cast<f32>(ty + 1); break;
                    case 2: mx = x0; my = ym; gfx = static_cast<f32>(tx); gfy = ty + 0.5f; break;
                    default: mx = x1; my = ym; gfx = static_cast<f32>(tx + 1); gfy = ty + 0.5f; break;
                }
                return add_vert(mx, my, base_z + lerp_height(gfx, gfy),
                                {gfx / td.tiles_x, gfy / td.tiles_y}, lerp_splat(gfx, gfy));
            };

            if (cmin == cmax) {
                // All same: full quad
                u32 v0 = corner_v(0, high_z), v1 = corner_v(1, high_z);
                u32 v2 = corner_v(2, high_z), v3 = corner_v(3, high_z);
                add_tri(v0, v1, v2);
                add_tri(v1, v3, v2);
            } else if (high_count == 1) {
                // 1 high corner: small triangle at high, rest at low.
                // Find the high corner.
                u32 hi = 0;
                for (u32 i = 0; i < 4; ++i) { if (c[i] == cmax) { hi = i; break; } }

                // Two edges touching the high corner
                // TL=0: edges top(0), left(2). TR=1: edges top(0), right(3).
                // BL=2: edges bottom(1), left(2). BR=3: edges bottom(1), right(3).
                u32 edge_a, edge_b;
                switch (hi) {
                    case 0: edge_a = 0; edge_b = 2; break;
                    case 1: edge_a = 0; edge_b = 3; break;
                    case 2: edge_a = 1; edge_b = 2; break;
                    default: edge_a = 1; edge_b = 3; break;
                }

                // High triangle: high corner + 2 midpoints at high_z
                u32 vh = corner_v(hi, high_z);
                u32 vma_h = mid_v(edge_a, high_z);
                u32 vmb_h = mid_v(edge_b, high_z);
                add_tri(vh, vma_h, vmb_h);

                // Wall: midpoint A to midpoint B, from high_z to low_z
                u32 vma_l = mid_v(edge_a, low_z);
                u32 vmb_l = mid_v(edge_b, low_z);
                // Normal toward the low side (center of tile away from high corner)
                glm::vec3 wall_n = glm::normalize(glm::vec3{
                    xm - vertices[vh].position.x,
                    ym - vertices[vh].position.y, 0.0f});
                vertices[vma_h].normal = vertices[vmb_h].normal = wall_n;
                vertices[vma_l].normal = vertices[vmb_l].normal = wall_n;

                // Wall quad
                glm::vec3 cross = glm::cross(
                    vertices[vmb_h].position - vertices[vma_h].position,
                    vertices[vma_l].position - vertices[vma_h].position);
                if (glm::dot(cross, wall_n) >= 0) {
                    indices.push_back(vma_h); indices.push_back(vmb_h); indices.push_back(vma_l);
                    indices.push_back(vmb_h); indices.push_back(vmb_l); indices.push_back(vma_l);
                } else {
                    indices.push_back(vma_h); indices.push_back(vma_l); indices.push_back(vmb_h);
                    indices.push_back(vmb_h); indices.push_back(vma_l); indices.push_back(vmb_l);
                }

                // Low surface: the rest of the tile (5 vertices: 3 low corners + 2 midpoints)
                u32 vl[4];
                for (u32 i = 0; i < 4; ++i) vl[i] = corner_v(i, low_z);
                // Low region: pentagon (3 low corners + 2 midpoints). Fan triangulation.
                // Triangulate as a fan from the opposite corner.
                // Order around the pentagon depends on which corner is high.
                // Let's list vertices CCW around the low region for each case:
                u32 fan[5];
                switch (hi) {
                    case 0: // TL high. Low region: TR, BR, BL, mid_left, mid_top
                        fan[0] = vl[1]; fan[1] = vl[3]; fan[2] = vl[2];
                        fan[3] = mid_v(2, low_z); fan[4] = mid_v(0, low_z); break;
                    case 1: // TR high. Low region: TL, mid_top, mid_right, BR, BL
                        fan[0] = vl[0]; fan[1] = mid_v(0, low_z); fan[2] = mid_v(3, low_z);
                        fan[3] = vl[3]; fan[4] = vl[2]; break;
                    case 2: // BL high. Low region: TL, TR, BR, mid_right... wait
                        // BL high, edges: bottom(1), left(2)
                        // Low region: TL, TR, BR, mid_bottom, mid_left
                        fan[0] = vl[0]; fan[1] = vl[1]; fan[2] = vl[3];
                        fan[3] = mid_v(1, low_z); fan[4] = mid_v(2, low_z); break;
                    default: // BR high, edges: bottom(1), right(3)
                        // Low region: TL, TR, mid_right, mid_bottom, BL
                        fan[0] = vl[0]; fan[1] = vl[1]; fan[2] = mid_v(3, low_z);
                        fan[3] = mid_v(1, low_z); fan[4] = vl[2]; break;
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
                    // Diagonal pair (TL+BR or TR+BL) — treat as 2 separate 1-high cases
                    // For simplicity, render full low tile
                    u32 v0 = corner_v(0, low_z), v1 = corner_v(1, low_z);
                    u32 v2 = corner_v(2, low_z), v3 = corner_v(3, low_z);
                    add_tri(v0, v1, v2);
                    add_tri(v1, v3, v2);
                    continue;
                }

                // Wall runs between midpoints of the two edges perpendicular to the high edge.
                // If top is high (edge 0), wall runs from mid-left to mid-right at y=ym.
                u32 wall_edge_a, wall_edge_b;
                if (edge_hi == 0 || edge_hi == 1) { wall_edge_a = 2; wall_edge_b = 3; }
                else { wall_edge_a = 0; wall_edge_b = 1; }

                // High half: 2 high corners + 2 midpoints
                u32 hi_corners[2];
                u32 n_hi = 0;
                for (u32 i = 0; i < 4; ++i) { if (c[i] == cmax) hi_corners[n_hi++] = i; }

                u32 vh0 = corner_v(hi_corners[0], high_z);
                u32 vh1 = corner_v(hi_corners[1], high_z);
                u32 vma_h = mid_v(wall_edge_a, high_z);
                u32 vmb_h = mid_v(wall_edge_b, high_z);
                add_tri(vh0, vh1, vma_h);
                add_tri(vh1, vmb_h, vma_h);

                // Low half: 2 low corners + 2 midpoints
                u32 lo_corners[2];
                u32 n_lo = 0;
                for (u32 i = 0; i < 4; ++i) { if (c[i] != cmax) lo_corners[n_lo++] = i; }

                u32 vl0 = corner_v(lo_corners[0], low_z);
                u32 vl1 = corner_v(lo_corners[1], low_z);
                u32 vma_l = mid_v(wall_edge_a, low_z);
                u32 vmb_l = mid_v(wall_edge_b, low_z);
                add_tri(vl0, vl1, vma_l);
                add_tri(vl1, vmb_l, vma_l);

                // Wall quad
                vma_h = mid_v(wall_edge_a, high_z);
                vmb_h = mid_v(wall_edge_b, high_z);
                vma_l = mid_v(wall_edge_a, low_z);
                vmb_l = mid_v(wall_edge_b, low_z);

                glm::vec3 wall_center = (vertices[vma_h].position + vertices[vmb_h].position) * 0.5f;
                glm::vec3 high_center{0, 0, 0};
                for (u32 i = 0; i < 4; ++i) { if (c[i] != cmax) {
                    high_center = glm::vec3{static_cast<f32>((i & 1) ? tx+1 : tx) * td.tile_size,
                                            static_cast<f32>((i & 2) ? ty+1 : ty) * td.tile_size, 0};
                    break;
                }}
                glm::vec3 wall_n = glm::normalize(glm::vec3{
                    high_center.x - wall_center.x,
                    high_center.y - wall_center.y, 0.0f});

                vertices[vma_h].normal = vertices[vmb_h].normal = wall_n;
                vertices[vma_l].normal = vertices[vmb_l].normal = wall_n;

                glm::vec3 cross = glm::cross(
                    vertices[vmb_h].position - vertices[vma_h].position,
                    vertices[vma_l].position - vertices[vma_h].position);
                if (glm::dot(cross, wall_n) >= 0) {
                    indices.push_back(vma_h); indices.push_back(vmb_h); indices.push_back(vma_l);
                    indices.push_back(vmb_h); indices.push_back(vmb_l); indices.push_back(vma_l);
                } else {
                    indices.push_back(vma_h); indices.push_back(vma_l); indices.push_back(vmb_h);
                    indices.push_back(vmb_h); indices.push_back(vma_l); indices.push_back(vmb_l);
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

                // Low triangle
                u32 vlo = corner_v(lo, low_z);
                u32 vma_l = mid_v(edge_a, low_z);
                u32 vmb_l = mid_v(edge_b, low_z);
                add_tri(vlo, vma_l, vmb_l);

                // Wall
                u32 vma_h = mid_v(edge_a, high_z);
                u32 vmb_h = mid_v(edge_b, high_z);
                glm::vec3 wall_n = glm::normalize(glm::vec3{
                    vertices[vlo].position.x - xm,
                    vertices[vlo].position.y - ym, 0.0f});
                vertices[vma_h].normal = vertices[vmb_h].normal = wall_n;
                vertices[vma_l].normal = vertices[vmb_l].normal = wall_n;

                glm::vec3 cross = glm::cross(
                    vertices[vmb_h].position - vertices[vma_h].position,
                    vertices[vma_l].position - vertices[vma_h].position);
                if (glm::dot(cross, wall_n) >= 0) {
                    indices.push_back(vma_h); indices.push_back(vmb_h); indices.push_back(vma_l);
                    indices.push_back(vmb_h); indices.push_back(vmb_l); indices.push_back(vma_l);
                } else {
                    indices.push_back(vma_h); indices.push_back(vma_l); indices.push_back(vmb_h);
                    indices.push_back(vmb_h); indices.push_back(vma_l); indices.push_back(vmb_l);
                }

                // High surface: pentagon (5 vertices: 3 high corners + 2 midpoints)
                u32 vh[4];
                for (u32 i = 0; i < 4; ++i) vh[i] = corner_v(i, high_z);
                u32 fan[5];
                switch (lo) {
                    case 0: // TL low. High: TR, BR, BL, mid_left, mid_top
                        fan[0] = vh[1]; fan[1] = vh[3]; fan[2] = vh[2];
                        fan[3] = mid_v(2, high_z); fan[4] = mid_v(0, high_z); break;
                    case 1: // TR low. High: TL, mid_top, mid_right, BR, BL
                        fan[0] = vh[0]; fan[1] = mid_v(0, high_z); fan[2] = mid_v(3, high_z);
                        fan[3] = vh[3]; fan[4] = vh[2]; break;
                    case 2: // BL low. High: TL, TR, BR, mid_bottom, mid_left
                        fan[0] = vh[0]; fan[1] = vh[1]; fan[2] = vh[3];
                        fan[3] = mid_v(1, high_z); fan[4] = mid_v(2, high_z); break;
                    default: // BR low. High: TL, TR, mid_right, mid_bottom, BL
                        fan[0] = vh[0]; fan[1] = vh[1]; fan[2] = mid_v(3, high_z);
                        fan[3] = mid_v(1, high_z); fan[4] = vh[2]; break;
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

    // Smooth normals for surface vertices.
    // Accumulate face normals per grid position, then assign back.
    // Uses a map from (vx, vy) -> accumulated normal.
    u32 surface_vert_end = static_cast<u32>(vertices.size());
    {
        std::vector<glm::vec3> normal_accum(td.verts_x() * td.verts_y(), glm::vec3{0.0f});

        // Accumulate: for each triangle, add face normal to its 3 vertex positions
        for (u32 i = 0; i + 2 < static_cast<u32>(indices.size()); i += 3) {
            u32 ia = indices[i], ib = indices[i+1], ic = indices[i+2];
            if (ia >= surface_vert_end || ib >= surface_vert_end || ic >= surface_vert_end)
                continue; // skip wall triangles
            glm::vec3 face_n = glm::cross(
                vertices[ib].position - vertices[ia].position,
                vertices[ic].position - vertices[ia].position);
            // Map each vertex back to grid position
            for (u32 vi : {ia, ib, ic}) {
                auto& p = vertices[vi].position;
                u32 gx = static_cast<u32>(std::round(p.x / td.tile_size));
                u32 gy = static_cast<u32>(std::round(p.y / td.tile_size));
                gx = std::min(gx, td.tiles_x);
                gy = std::min(gy, td.tiles_y);
                normal_accum[gy * td.verts_x() + gx] += face_n;
            }
        }

        // Normalize
        for (auto& n : normal_accum) {
            f32 len = glm::length(n);
            n = (len > 0) ? n / len : glm::vec3{0, 0, 1};
        }

        // Assign back to surface vertices
        for (u32 i = 0; i < surface_vert_end; ++i) {
            auto& p = vertices[i].position;
            u32 gx = static_cast<u32>(std::round(p.x / td.tile_size));
            u32 gy = static_cast<u32>(std::round(p.y / td.tile_size));
            gx = std::min(gx, td.tiles_x);
            gy = std::min(gy, td.tiles_y);
            vertices[i].normal = normal_accum[gy * td.verts_x() + gx];
        }
    }

    // Cliff walls are generated inline with surface tiles above (cases 1, 2, 3).

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
