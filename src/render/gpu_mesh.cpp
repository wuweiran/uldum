#include "render/gpu_mesh.h"
#include "core/log.h"

#include <cstring>

namespace uldum::render {

static constexpr u32 MAX_BONES = 128;

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

// Create a persistently mapped buffer (for bone matrices updated every frame)
static VkBuffer create_mapped_buffer(VmaAllocator allocator, VmaAllocation& alloc,
                                      void*& mapped, usize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo buf_ci{};
    buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_ci.size  = size;
    buf_ci.usage = usage;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                     VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo info{};
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vmaCreateBuffer(allocator, &buf_ci, &alloc_ci, &buffer, &alloc, &info) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    mapped = info.pMappedData;
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

GpuMesh upload_skinned_mesh(VmaAllocator allocator, const asset::SkinnedMeshData& mesh, u32 bone_count) {
    GpuMesh gpu{};
    gpu.is_skinned = true;
    gpu.bone_count = bone_count;

    if (mesh.vertices.empty()) return gpu;

    gpu.vertex_count = static_cast<u32>(mesh.vertices.size());
    gpu.vertex_buffer = create_buffer(allocator, gpu.vertex_alloc,
        mesh.vertices.data(), mesh.vertices.size() * sizeof(asset::SkinnedVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    if (!gpu.vertex_buffer) {
        log::error("GpuMesh", "Failed to create skinned vertex buffer");
        return {};
    }

    if (!mesh.indices.empty()) {
        gpu.index_count = static_cast<u32>(mesh.indices.size());
        gpu.index_buffer = create_buffer(allocator, gpu.index_alloc,
            mesh.indices.data(), mesh.indices.size() * sizeof(u32),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        if (!gpu.index_buffer) {
            log::error("GpuMesh", "Failed to create skinned index buffer");
            vmaDestroyBuffer(allocator, gpu.vertex_buffer, gpu.vertex_alloc);
            return {};
        }
    }

    // Create bone SSBO — persistently mapped for per-frame updates
    u32 clamped = std::min(bone_count, MAX_BONES);
    usize bone_buf_size = clamped * sizeof(glm::mat4);
    gpu.bone_buffer = create_mapped_buffer(allocator, gpu.bone_alloc,
        gpu.bone_mapped, bone_buf_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    if (!gpu.bone_buffer) {
        log::error("GpuMesh", "Failed to create bone SSBO");
        destroy_mesh(allocator, gpu);
        return {};
    }

    // Initialize to identity matrices
    auto* bones = static_cast<glm::mat4*>(gpu.bone_mapped);
    for (u32 i = 0; i < clamped; ++i) {
        bones[i] = glm::mat4{1.0f};
    }

    log::info("GpuMesh", "Uploaded skinned mesh: {} verts, {} indices, {} bones",
              gpu.vertex_count, gpu.index_count, clamped);
    return gpu;
}

void destroy_mesh(VmaAllocator allocator, GpuMesh& mesh) {
    if (mesh.bone_buffer)   vmaDestroyBuffer(allocator, mesh.bone_buffer, mesh.bone_alloc);
    if (mesh.index_buffer)  vmaDestroyBuffer(allocator, mesh.index_buffer, mesh.index_alloc);
    if (mesh.vertex_buffer) vmaDestroyBuffer(allocator, mesh.vertex_buffer, mesh.vertex_alloc);
    mesh = {};
}

} // namespace uldum::render
