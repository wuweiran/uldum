#include "render/terrain.h"
#include "map/terrain_data.h"
#include "asset/model.h"
#include "core/log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <vector>

namespace uldum::render {

TerrainMesh build_terrain_mesh(VmaAllocator allocator, const map::TerrainData& td) {
    if (!td.is_valid()) {
        log::error("Terrain", "Cannot build mesh from invalid TerrainData");
        return {};
    }

    u32 vx = td.verts_x();
    u32 vy = td.verts_y();

    std::vector<asset::Vertex> vertices(vx * vy);

    // Fill positions and texcoords from heightmap
    for (u32 iy = 0; iy < vy; ++iy) {
        for (u32 ix = 0; ix < vx; ++ix) {
            u32 idx = iy * vx + ix;
            f32 x = static_cast<f32>(ix) * td.tile_size;
            f32 y = static_cast<f32>(iy) * td.tile_size;
            f32 z = td.heightmap[idx];

            vertices[idx].position = {x, y, z};
            // UV spans 0-1 across entire terrain (for splatmap sampling)
            vertices[idx].texcoord = {
                static_cast<f32>(ix) / static_cast<f32>(td.tiles_x),
                static_cast<f32>(iy) / static_cast<f32>(td.tiles_y)
            };
        }
    }

    // Compute normals from cross products of neighbor edges
    for (u32 iy = 0; iy < vy; ++iy) {
        for (u32 ix = 0; ix < vx; ++ix) {
            u32 idx = iy * vx + ix;
            glm::vec3 p = vertices[idx].position;

            glm::vec3 left  = (ix > 0)          ? vertices[idx - 1].position  : p;
            glm::vec3 right = (ix < td.tiles_x) ? vertices[idx + 1].position  : p;
            glm::vec3 down  = (iy > 0)           ? vertices[idx - vx].position : p;
            glm::vec3 up    = (iy < td.tiles_y)  ? vertices[idx + vx].position : p;

            glm::vec3 dx = right - left;
            glm::vec3 dy = up - down;
            vertices[idx].normal = glm::normalize(glm::cross(dx, dy));
        }
    }

    // Generate triangle indices (two triangles per tile)
    std::vector<u32> indices;
    indices.reserve(td.tile_count() * 6);
    for (u32 iy = 0; iy < td.tiles_y; ++iy) {
        for (u32 ix = 0; ix < td.tiles_x; ++ix) {
            u32 tl = iy * vx + ix;
            u32 tr = tl + 1;
            u32 bl = tl + vx;
            u32 br = bl + 1;

            indices.push_back(tl);
            indices.push_back(tr);
            indices.push_back(bl);

            indices.push_back(tr);
            indices.push_back(br);
            indices.push_back(bl);
        }
    }

    asset::MeshData mesh_data;
    mesh_data.vertices = std::move(vertices);
    mesh_data.indices  = std::move(indices);

    TerrainMesh result;
    result.gpu_mesh = upload_mesh(allocator, mesh_data);

    log::info("Terrain", "Built terrain mesh: {}x{} tiles, {} vertices, {} indices",
              td.tiles_x, td.tiles_y, vx * vy, static_cast<u32>(mesh_data.indices.size()));

    return result;
}

void destroy_terrain_mesh(VmaAllocator allocator, TerrainMesh& mesh) {
    destroy_mesh(allocator, mesh.gpu_mesh);
    mesh = {};
}

} // namespace uldum::render
