#pragma once

#include "core/types.h"
#include "asset/model.h"
#include "rhi/handles.h"

namespace uldum::rhi { class Rhi; }

namespace uldum::render {

// A mesh uploaded to GPU memory (vertex + index buffers).
struct GpuMesh {
    rhi::BufferHandle vertex_buffer{};
    rhi::BufferHandle index_buffer{};
    u32  index_count     = 0;
    u32  vertex_count    = 0;
    u32  first_vertex    = 0;    // offset into mega buffer (Phase 14b)
    u32  first_index     = 0;    // offset into mega buffer (Phase 14b)
    f32  bounding_radius = 0.0f; // model-space bounding sphere (frustum culling)
    // Game-space pick dimensions from the model AABB at upload, honoring the
    // Y-up→Z-up flip. Multiply by instance scale for world size. Auto-fits the
    // selection cylinder to the actual model silhouette.
    f32  footprint_radius = 0.0f; // pick radius: max game-X/Y half-extent
    f32  pick_height      = 0.0f; // pick cylinder height: full game-Z extent
    bool native_z_up     = false; // true = already in Z-up game coords, skip glTF rotation
    bool is_skinned      = false;
    u32  bone_count      = 0;

    // True when this mesh owns its vertex/index buffers (created by
    // upload_mesh / upload_skinned_mesh). False for meshes that reference
    // a slice of a shared mega buffer — destroy_mesh skips freeing then.
    bool owns_buffers    = true;
};

// Upload CPU-side MeshData to GPU buffers.
GpuMesh upload_mesh(rhi::Rhi& rhi, const asset::MeshData& mesh);

// Upload skinned mesh.
GpuMesh upload_skinned_mesh(rhi::Rhi& rhi, const asset::SkinnedMeshData& mesh, u32 bone_count);

// Destroy GPU mesh buffers (no-op for mega-buffer refs).
void destroy_mesh(rhi::Rhi& rhi, GpuMesh& mesh);

} // namespace uldum::render
