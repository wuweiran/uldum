#pragma once

#include "render/gpu_mesh.h"
#include "core/types.h"

#include <vk_mem_alloc.h>

namespace uldum::map { struct TerrainData; }

namespace uldum::render {

// GPU-side terrain mesh built from map::TerrainData.
struct TerrainMesh {
    GpuMesh gpu_mesh{};
};

// Build a GPU mesh from terrain data. Call again to rebuild after edits.
TerrainMesh build_terrain_mesh(VmaAllocator allocator, const map::TerrainData& terrain);

void destroy_terrain_mesh(VmaAllocator allocator, TerrainMesh& mesh);

} // namespace uldum::render
