#pragma once

#include "render/camera.h"
#include "render/gpu_mesh.h"
#include "render/gpu_texture.h"
#include "render/terrain.h"
#include "render/material.h"
#include "render/shadow.h"
#include "render/animation.h"
#include "render/particles.h"
#include "render/effect.h"
#include "core/handle.h"
#include "asset/model.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/mat4x4.hpp>

#include <string>
#include <unordered_map>

namespace uldum::rhi { class VulkanRhi; }
namespace uldum::simulation { struct World; class Pathfinder; }
namespace uldum::platform { struct InputState; }
namespace uldum::map { struct TerrainData; }

namespace uldum::render {

class Renderer {
public:
    bool init(rhi::VulkanRhi& rhi);
    void shutdown();

    void update_camera(const platform::InputState& input, f32 dt);
    void handle_resize(f32 aspect);

    // Build (or rebuild) the terrain GPU mesh and splatmap from terrain data.
    void set_terrain(const map::TerrainData& terrain);

    // Set pathfinder for terrain normal sampling (used for entity slope tilt).
    void set_pathfinder(const simulation::Pathfinder* pf) { m_pathfinder = pf; }

    // Record shadow depth pass (must be called before begin_rendering).
    void draw_shadows(VkCommandBuffer cmd, const simulation::World& world);

    // Record main pass draw commands into the given command buffer.
    // Reads Transform + Renderable components from the world.
    void draw(VkCommandBuffer cmd, VkExtent2D extent, const simulation::World& world);

    Camera& camera() { return m_camera; }
    ParticleSystem& particles() { return m_particles; }
    EffectRegistry& effect_registry() { return m_effect_registry; }
    EffectManager&  effect_manager()  { return m_effect_manager; }

private:
    bool create_descriptor_layouts();
    bool create_mesh_pipeline();
    bool create_skinned_mesh_pipeline();
    bool create_particle_pipeline();
    bool create_terrain_pipeline();
    bool create_shadow_pipeline();
    bool create_shadow_resources();
    bool create_default_texture();
    bool create_terrain_textures();
    GpuMesh& get_or_upload_mesh(const std::string& model_path);
    VkDescriptorSet allocate_mesh_descriptor(const GpuTexture& diffuse);
    VkDescriptorSet allocate_terrain_descriptor(const TerrainMaterial& mat);
    VkDescriptorSet allocate_shadow_descriptor();
    VkDescriptorSet allocate_bone_descriptor(VkBuffer bone_buffer, usize size);

    void draw_shadow_pass(VkCommandBuffer cmd, const simulation::World& world);

    rhi::VulkanRhi* m_rhi = nullptr;
    const simulation::Pathfinder* m_pathfinder = nullptr;
    Camera          m_camera;

    // Descriptor infrastructure
    VkDescriptorSetLayout m_mesh_desc_layout    = VK_NULL_HANDLE;  // set 0: 1 diffuse sampler
    VkDescriptorSetLayout m_terrain_desc_layout = VK_NULL_HANDLE;  // set 0: 4 layers + 1 splatmap
    VkDescriptorSetLayout m_shadow_desc_layout  = VK_NULL_HANDLE;  // set 1: UBO + shadow map
    // Growable descriptor pool — allocates new pools on demand
    std::vector<VkDescriptorPool> m_descriptor_pools;
    VkDescriptorPool allocate_or_grow_pool();

    // Mesh pipeline (set 0 = material, set 1 = shadow)
    VkPipelineLayout m_mesh_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_mesh_pipeline        = VK_NULL_HANDLE;

    // Terrain pipeline (set 0 = terrain material, set 1 = shadow)
    VkPipelineLayout m_terrain_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_terrain_pipeline        = VK_NULL_HANDLE;

    // Skinned mesh pipeline (set 0 = material, set 1 = shadow, set 2 = bones SSBO)
    VkDescriptorSetLayout m_bone_desc_layout          = VK_NULL_HANDLE;  // set 2: 1 SSBO
    VkPipelineLayout m_skinned_mesh_pipeline_layout   = VK_NULL_HANDLE;
    VkPipeline       m_skinned_mesh_pipeline          = VK_NULL_HANDLE;
    VkPipelineLayout m_skinned_shadow_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_skinned_shadow_pipeline        = VK_NULL_HANDLE;

    // Shadow depth pass pipeline (push constant = light MVP, depth-only)
    VkPipelineLayout m_shadow_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_shadow_pipeline        = VK_NULL_HANDLE;
    VkPipelineLayout m_terrain_shadow_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_terrain_shadow_pipeline        = VK_NULL_HANDLE;

    // Shadow map resources
    ShadowMap       m_shadow_map{};
    ShadowBuffer    m_shadow_ubo{};
    VkDescriptorSet m_shadow_desc_set = VK_NULL_HANDLE;

    // Default texture for untextured meshes
    GpuTexture      m_default_texture{};
    MeshMaterial    m_default_material{};

    // Corpse material (dark gray)
    GpuTexture      m_corpse_texture{};
    MeshMaterial    m_corpse_material{};

    // Terrain material (ground layer textures + splatmap)
    TerrainMaterial m_terrain_material{};

    // Cached GPU meshes (model_path → GpuMesh)
    std::unordered_map<std::string, GpuMesh> m_mesh_cache;

    // Placeholder mesh for models that fail to load
    GpuMesh m_placeholder_mesh{};

    // Small projectile mesh
    GpuMesh m_projectile_mesh{};

    // Particle pipeline (alpha-blended, depth test on, depth write off)
    VkPipelineLayout m_particle_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_particle_pipeline        = VK_NULL_HANDLE;

    // Terrain mesh
    TerrainMesh m_terrain{};

    // Procedural test model (used for all units until real glTF models are loaded)
    asset::ModelData m_test_model;
    GpuMesh          m_test_skinned_mesh{};

    // Per-entity animation instances (entity id → AnimationInstance)
    std::unordered_map<u32, AnimationInstance> m_anim_instances;

    // Particle system + effect system
    ParticleSystem  m_particles;
    EffectRegistry  m_effect_registry;
    EffectManager   m_effect_manager;

    // Get or create animation instance for an entity
    AnimationInstance& get_or_create_anim(u32 entity_id);
};

} // namespace uldum::render
