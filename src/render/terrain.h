#pragma once

#include "render/gpu_mesh.h"
#include "core/types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <vk_mem_alloc.h>

namespace uldum::map { struct TerrainData; }

namespace uldum::render {

// Terrain vertex with blend data for tile transitions (40 bytes).
struct TerrainVertex {
    glm::vec3 position{0.0f};           // 12 — world XYZ
    glm::vec3 normal{0.0f, 0.0f, 1.0f}; // 12
    glm::vec2 texcoord{0.0f};           // 8  — UV across entire terrain (0-1)
    u32       layer_corners = 0;        // 4  — packed: c0|(c1<<8)|(c2<<16)|(c3<<24)
    u32       case_info = 0;            // 4  — reserved for future use
};

// GPU-side terrain mesh built from map::TerrainData.
struct TerrainMesh {
    GpuMesh gpu_mesh{};
};

// Build a GPU mesh from terrain data. Call again to rebuild after edits.
TerrainMesh build_terrain_mesh(VmaAllocator allocator, const map::TerrainData& terrain);

void destroy_terrain_mesh(VmaAllocator allocator, TerrainMesh& mesh);

} // namespace uldum::render
