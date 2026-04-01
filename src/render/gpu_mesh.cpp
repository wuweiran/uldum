#include "render/gpu_mesh.h"
#include "core/log.h"

#include <cstring>

namespace uldum::render {

static VkBuffer create_buffer(VmaAllocator allocator, VmaAllocation& alloc,
                               const void* data, usize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size  = size;
    buf_ci.usage = usage;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vmaCreateBuffer(allocator, &buf_ci, &alloc_ci, &buffer, &alloc, nullptr) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    void* mapped = nullptr;
    vmaMapMemory(allocator, alloc, &mapped);
    std::memcpy(mapped, data, size);
    vmaUnmapMemory(allocator, alloc);

    return buffer;
}

GpuMesh upload_mesh(VmaAllocator allocator, const asset::MeshData& mesh) {
    GpuMesh gpu{};

    if (mesh.vertices.empty()) return gpu;

    gpu.vertex_count = static_cast<u32>(mesh.vertices.size());
    gpu.vertex_buffer = create_buffer(allocator, gpu.vertex_alloc,
        mesh.vertices.data(), mesh.vertices.size() * sizeof(asset::Vertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    if (!gpu.vertex_buffer) {
        log::error("GpuMesh", "Failed to create vertex buffer");
        return {};
    }

    if (!mesh.indices.empty()) {
        gpu.index_count = static_cast<u32>(mesh.indices.size());
        gpu.index_buffer = create_buffer(allocator, gpu.index_alloc,
            mesh.indices.data(), mesh.indices.size() * sizeof(u32),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        if (!gpu.index_buffer) {
            log::error("GpuMesh", "Failed to create index buffer");
            vmaDestroyBuffer(allocator, gpu.vertex_buffer, gpu.vertex_alloc);
            return {};
        }
    }

    return gpu;
}

void destroy_mesh(VmaAllocator allocator, GpuMesh& mesh) {
    if (mesh.index_buffer)  vmaDestroyBuffer(allocator, mesh.index_buffer, mesh.index_alloc);
    if (mesh.vertex_buffer) vmaDestroyBuffer(allocator, mesh.vertex_buffer, mesh.vertex_alloc);
    mesh = {};
}

} // namespace uldum::render
