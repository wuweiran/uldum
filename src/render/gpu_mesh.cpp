#include "render/gpu_mesh.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "core/log.h"

#include <cstring>

namespace uldum::render {

static rhi::BufferHandle upload_buffer(rhi::Rhi& rhi, const void* data,
                                       usize size, rhi::BufferUsage usage) {
    rhi::BufferDesc d{};
    d.size   = size;
    d.usage  = usage;
    d.memory = rhi::MemoryUsage::HostSequential;
    auto h = rhi.create_buffer(d);
    if (!h.is_valid()) return {};
    if (void* dst = rhi.mapped_ptr(h)) {
        std::memcpy(dst, data, size);
    }
    return h;
}

GpuMesh upload_mesh(rhi::Rhi& rhi, const asset::MeshData& mesh) {
    GpuMesh gpu{};
    if (mesh.vertices.empty()) return gpu;

    gpu.vertex_count  = static_cast<u32>(mesh.vertices.size());
    gpu.vertex_buffer = upload_buffer(rhi, mesh.vertices.data(),
        mesh.vertices.size() * sizeof(asset::Vertex), rhi::BufferUsage::Vertex);
    if (!gpu.vertex_buffer.is_valid()) {
        log::error("GpuMesh", "Failed to create vertex buffer");
        return {};
    }

    if (!mesh.indices.empty()) {
        gpu.index_count  = static_cast<u32>(mesh.indices.size());
        gpu.index_buffer = upload_buffer(rhi, mesh.indices.data(),
            mesh.indices.size() * sizeof(u32), rhi::BufferUsage::Index);
        if (!gpu.index_buffer.is_valid()) {
            log::error("GpuMesh", "Failed to create index buffer");
            rhi.destroy_buffer(gpu.vertex_buffer);
            return {};
        }
    }
    return gpu;
}

GpuMesh upload_skinned_mesh(rhi::Rhi& rhi, const asset::SkinnedMeshData& mesh, u32 bone_count) {
    GpuMesh gpu{};
    gpu.is_skinned = true;
    gpu.bone_count = bone_count;
    if (mesh.vertices.empty()) return gpu;

    gpu.vertex_count  = static_cast<u32>(mesh.vertices.size());
    gpu.vertex_buffer = upload_buffer(rhi, mesh.vertices.data(),
        mesh.vertices.size() * sizeof(asset::SkinnedVertex), rhi::BufferUsage::Vertex);
    if (!gpu.vertex_buffer.is_valid()) {
        log::error("GpuMesh", "Failed to create skinned vertex buffer");
        return {};
    }

    if (!mesh.indices.empty()) {
        gpu.index_count  = static_cast<u32>(mesh.indices.size());
        gpu.index_buffer = upload_buffer(rhi, mesh.indices.data(),
            mesh.indices.size() * sizeof(u32), rhi::BufferUsage::Index);
        if (!gpu.index_buffer.is_valid()) {
            log::error("GpuMesh", "Failed to create skinned index buffer");
            rhi.destroy_buffer(gpu.vertex_buffer);
            return {};
        }
    }

    log::info("GpuMesh", "Uploaded skinned mesh: {} verts, {} indices, {} bones",
              gpu.vertex_count, gpu.index_count, bone_count);
    return gpu;
}

void destroy_mesh(rhi::Rhi& rhi, GpuMesh& mesh) {
    if (mesh.owns_buffers) {
        rhi.destroy_buffer(mesh.vertex_buffer);
        rhi.destroy_buffer(mesh.index_buffer);
    }
    mesh = {};
}

} // namespace uldum::render
