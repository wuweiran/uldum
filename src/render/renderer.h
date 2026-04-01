#pragma once

#include "render/camera.h"
#include "render/gpu_mesh.h"
#include "render/terrain.h"
#include "core/handle.h"
#include "asset/model.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/mat4x4.hpp>

#include <string>
#include <unordered_map>

namespace uldum::rhi { class VulkanRhi; }
namespace uldum::simulation { struct World; }
namespace uldum::platform { struct InputState; }
namespace uldum::map { struct TerrainData; }

namespace uldum::render {

class Renderer {
public:
    bool init(rhi::VulkanRhi& rhi);
    void shutdown();

    void update_camera(const platform::InputState& input, f32 dt);
    void handle_resize(f32 aspect);

    // Build (or rebuild) the terrain GPU mesh from terrain data.
    void set_terrain(const map::TerrainData& terrain);

    // Record draw commands into the given command buffer.
    // Reads Transform + Renderable components from the world.
    void draw(VkCommandBuffer cmd, VkExtent2D extent, const simulation::World& world);

    Camera& camera() { return m_camera; }

private:
    bool create_mesh_pipeline();
    GpuMesh& get_or_upload_mesh(const std::string& model_path);

    rhi::VulkanRhi* m_rhi = nullptr;
    Camera          m_camera;

    // Mesh pipeline (3D with MVP push constants)
    VkPipelineLayout m_mesh_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_mesh_pipeline        = VK_NULL_HANDLE;

    // Cached GPU meshes (model_path → GpuMesh)
    std::unordered_map<std::string, GpuMesh> m_mesh_cache;

    // Placeholder mesh for models that fail to load
    GpuMesh m_placeholder_mesh{};

    // Terrain
    TerrainMesh m_terrain{};
};

} // namespace uldum::render
