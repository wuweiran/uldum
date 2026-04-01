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
    bool          native_z_up   = false; // true = already in Z-up game coords, skip glTF rotation
};

// Upload CPU-side MeshData to GPU buffers via VMA.
GpuMesh upload_mesh(VmaAllocator allocator, const asset::MeshData& mesh);

// Destroy GPU mesh buffers.
void destroy_mesh(VmaAllocator allocator, GpuMesh& mesh);

} // namespace uldum::render
