#pragma once

#include "render/gpu_mesh.h"
#include "core/types.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <vk_mem_alloc.h>

namespace uldum::map { struct TerrainData; }

namespace uldum::render {

// Terrain vertex with splatmap weights baked in (48 bytes).
struct TerrainVertex {
    glm::vec3 position{0.0f};   // world XYZ (Z = cliff_level * layer_height + heightmap)
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec2 texcoord{0.0f};   // UV across entire terrain (0-1)
    glm::vec4 splat_weights{1.0f, 0.0f, 0.0f, 0.0f};  // blend weights for 4 texture layers
};

// GPU-side terrain mesh built from map::TerrainData.
struct TerrainMesh {
    GpuMesh gpu_mesh{};
};

// Build a GPU mesh from terrain data. Call again to rebuild after edits.
TerrainMesh build_terrain_mesh(VmaAllocator allocator, const map::TerrainData& terrain);

void destroy_terrain_mesh(VmaAllocator allocator, TerrainMesh& mesh);

} // namespace uldum::render
