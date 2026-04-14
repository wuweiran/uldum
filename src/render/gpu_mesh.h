#pragma once

#include "core/types.h"
#include "asset/model.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace uldum::render {

// A mesh uploaded to GPU memory (vertex + index buffers).
struct GpuMesh {
    VkBuffer      vertex_buffer = VK_NULL_HANDLE;
    VmaAllocation vertex_alloc  = VK_NULL_HANDLE;
    VkBuffer      index_buffer  = VK_NULL_HANDLE;
    VmaAllocation index_alloc   = VK_NULL_HANDLE;
    u32           index_count   = 0;
    u32           vertex_count  = 0;
    u32           first_vertex  = 0;    // offset into mega buffer (Phase 14b)
    u32           first_index   = 0;    // offset into mega buffer (Phase 14b)
    f32           bounding_radius = 0.0f; // model-space bounding sphere radius (for frustum culling)
    bool          native_z_up   = false; // true = already in Z-up game coords, skip glTF rotation
    bool          is_skinned    = false;
    u32           bone_count    = 0;

    // Per-instance bone SSBO (only valid if is_skinned)
    VkBuffer      bone_buffer   = VK_NULL_HANDLE;
    VmaAllocation bone_alloc    = VK_NULL_HANDLE;
    void*         bone_mapped   = nullptr; // persistently mapped for CPU updates
    VkDescriptorSet bone_descriptor = VK_NULL_HANDLE;
};

// Upload CPU-side MeshData to GPU buffers via VMA.
GpuMesh upload_mesh(VmaAllocator allocator, const asset::MeshData& mesh);

// Upload skinned mesh with bone buffer.
GpuMesh upload_skinned_mesh(VmaAllocator allocator, const asset::SkinnedMeshData& mesh, u32 bone_count);

// Destroy GPU mesh buffers.
void destroy_mesh(VmaAllocator allocator, GpuMesh& mesh);

} // namespace uldum::render
