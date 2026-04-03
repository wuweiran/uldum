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

    u32 vx = td.verts_x();
    u32 vy = td.verts_y();

    std::vector<TerrainVertex> vertices(vx * vy);

    // Fill positions, texcoords, and splatmap weights
    for (u32 iy = 0; iy < vy; ++iy) {
        for (u32 ix = 0; ix < vx; ++ix) {
            u32 idx = iy * vx + ix;
            f32 x = static_cast<f32>(ix) * td.tile_size;
            f32 y = static_cast<f32>(iy) * td.tile_size;
            f32 z = td.world_z_at(ix, iy);

            vertices[idx].position = {x, y, z};
            vertices[idx].texcoord = {
                static_cast<f32>(ix) / static_cast<f32>(td.tiles_x),
                static_cast<f32>(iy) / static_cast<f32>(td.tiles_y)
            };

            // Splatmap weights: convert u8 (0-255) to float (0-1)
            f32 w0 = static_cast<f32>(td.splatmap[0][idx]) / 255.0f;
            f32 w1 = static_cast<f32>(td.splatmap[1][idx]) / 255.0f;
            f32 w2 = static_cast<f32>(td.splatmap[2][idx]) / 255.0f;
            f32 w3 = static_cast<f32>(td.splatmap[3][idx]) / 255.0f;
            vertices[idx].splat_weights = {w0, w1, w2, w3};
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

    // Upload as raw vertex data (TerrainVertex layout, not asset::Vertex)
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
