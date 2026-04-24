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
#include <unordered_set>

namespace uldum::rhi { class VulkanRhi; }
namespace uldum::simulation { struct World; struct Transform; class Simulation; }
namespace uldum::platform { struct InputState; }
namespace uldum::map { struct TerrainData; struct Tileset; struct EnvironmentConfig; }

namespace uldum::render {

// A fully loaded model: CPU data + GPU resources + material.
struct LoadedModel {
    asset::ModelData data;
    GpuMesh          mesh{};             // first mesh (skinned or static)
    GpuTexture       diffuse_texture{};  // from model or default
    MeshMaterial     material{};         // per-model descriptor (skinned pipeline only)
    bool             is_skinned = false;
    u32              texture_index = 0;  // index into bindless texture array (Phase 14b)
};

// Per-instance data for static mesh SSBO (80 bytes, std430 aligned)
struct InstanceData {
    glm::mat4 model;          // 64 bytes
    u32       material_index; // 4 bytes — index into bindless texture array
    u32       _pad[3];        // 12 bytes — align to 16
};

class Renderer {
public:
    bool init(rhi::VulkanRhi& rhi);
    void shutdown();

    // Tear down anything scoped to a single game session — currently the
    // per-entity animation instances. Must be called from App::end_session
    // before the simulation handles get reused by the next map; otherwise
    // reused entity ids pick up stale bone state from the previous
    // session (visible as detached body parts / broken skeleton).
    void end_session();

    void update_camera(const platform::InputState& input, f32 dt);
    void handle_resize(f32 aspect);

    // Set the map root directory for resolving model asset paths.
    void set_map_root(std::string_view root) { m_map_root = root; }

    // Build (or rebuild) the terrain GPU mesh and splatmap from terrain data.
    void set_terrain(const map::TerrainData& terrain);

    // Load terrain layer textures from tileset (call after set_map_root, before set_terrain).
    void load_tileset_textures(const map::Tileset& tileset);

    // Set terrain data pointer for height/normal sampling (entity slope tilt).
    void set_terrain_data(const map::TerrainData* td) { m_terrain_data = td; }

    // Set environment (sun, ambient, fog, skybox) from map config.
    void set_environment(const map::EnvironmentConfig& env);

    // Add a point light for this frame. Call before draw(). Cleared each frame.
    void add_point_light(glm::vec3 position, glm::vec3 color, f32 radius, f32 intensity);

    // Set the fog of war visual grid for the local player. Called each frame before draw.
    // values: tiles_x * tiles_y floats (0.0=black .. 1.0=fully visible).
    // Pass nullptr to disable fog rendering.
    void set_fog_grid(const f32* values, u32 tiles_x, u32 tiles_y);

    // Set the simulation reference for fog visibility queries during draw.
    void set_simulation(const simulation::Simulation* sim) { m_simulation = sim; }

    // Set the local player for fog-based unit filtering.
    void set_local_player(u32 player_id) { m_local_player_id = player_id; }

    // Upload the fog texture to GPU. Must be called outside a render pass.
    void upload_fog(VkCommandBuffer cmd);

    // Record shadow depth pass (must be called before begin_rendering).
    // alpha: interpolation factor between previous and current tick (0..1).
    void draw_shadows(VkCommandBuffer cmd, const simulation::World& world, f32 alpha = 1.0f);

    // Record main pass draw commands into the given command buffer.
    // Reads Transform + Renderable components from the world.
    // alpha: interpolation factor between previous and current tick (0..1).
    void draw(VkCommandBuffer cmd, VkExtent2D extent, const simulation::World& world, f32 alpha = 1.0f);

    Camera& camera() { return m_camera; }
    const Camera& camera() const { return m_camera; }
    ParticleSystem& particles() { return m_particles; }
    EffectRegistry& effect_registry() { return m_effect_registry; }
    EffectManager&  effect_manager()  { return m_effect_manager; }

    // Get attachment point position in model-local space for a unit. Returns {0,0,0} if not found.
    glm::vec3 get_attachment_point(u32 entity_id, std::string_view bone_name) const;

private:
    bool create_descriptor_layouts();
    bool create_mesh_pipeline();
    bool create_skinned_mesh_pipeline();
    bool create_particle_pipeline();
    bool create_terrain_pipeline();
    bool create_water_pipeline();
    bool create_shadow_pipeline();
    bool create_shadow_resources();
    bool create_default_texture();
    bool create_terrain_textures();
    LoadedModel* get_or_load_model(const std::string& model_path);
    GpuMesh& get_or_upload_mesh(const std::string& model_path);
    VkDescriptorSet allocate_mesh_descriptor(const GpuTexture& diffuse);
    VkDescriptorSet allocate_terrain_descriptor(const TerrainMaterial& mat);
    VkDescriptorSet allocate_shadow_descriptor();
    VkDescriptorSet allocate_bone_descriptor(VkBuffer bone_buffer, usize size);

    void draw_shadow_pass(VkCommandBuffer cmd, const simulation::World& world, f32 alpha);

    // Returns true if an entity should be hidden by fog of war (enemy in non-visible tile).
    bool is_fog_hidden(const simulation::World& world, u32 id, const simulation::Transform& t) const;

    rhi::VulkanRhi* m_rhi = nullptr;
    const map::TerrainData* m_terrain_data = nullptr;
    Camera          m_camera;
    std::string     m_map_root;  // map root for resolving model paths

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

    // Water surface pipeline (transparent overlay, same mesh as terrain)
    VkPipelineLayout m_water_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_water_pipeline        = VK_NULL_HANDLE;

    // Skinned mesh pipeline (set 0 = material, set 1 = shadow, set 2 = bones SSBO)
    VkDescriptorSetLayout m_bone_desc_layout          = VK_NULL_HANDLE;  // set 2: 1 SSBO
    VkPipelineLayout m_skinned_mesh_pipeline_layout   = VK_NULL_HANDLE;
    VkPipeline       m_skinned_mesh_pipeline          = VK_NULL_HANDLE;
    VkPipelineLayout m_skinned_shadow_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_skinned_shadow_pipeline        = VK_NULL_HANDLE;

    // Skybox pipeline (set 0 = cubemap, push constant = VP without translation)
    VkDescriptorSetLayout m_skybox_desc_layout = VK_NULL_HANDLE;
    VkPipelineLayout m_skybox_pipeline_layout  = VK_NULL_HANDLE;
    VkPipeline       m_skybox_pipeline         = VK_NULL_HANDLE;
    VkDescriptorSet  m_skybox_desc_set         = VK_NULL_HANDLE;
    GpuTexture       m_skybox_cubemap{};
    GpuMesh          m_skybox_mesh{};
    bool             m_has_skybox = false;
    bool create_skybox_pipeline();
    bool create_skybox_mesh();

    // Shadow depth pass pipeline (push constant = light MVP, depth-only)
    VkPipelineLayout m_shadow_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_shadow_pipeline        = VK_NULL_HANDLE;
    VkPipelineLayout m_terrain_shadow_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_terrain_shadow_pipeline        = VK_NULL_HANDLE;

    // Shadow map resources
    ShadowMap       m_shadow_map{};
    ShadowBuffer    m_shadow_ubo{};
    VkDescriptorSet m_shadow_desc_set = VK_NULL_HANDLE;

    // Environment lighting (UBO at set 1 binding 2, cubemap at set 1 binding 3)
    static constexpr u32 MAX_POINT_LIGHTS = 8;

    struct PointLight {
        glm::vec4 position;  // xyz = world pos, w = radius
        glm::vec4 color;     // rgb = color, a = intensity
    };

    struct EnvironmentUBO {
        glm::vec4 sun_direction{-0.4f, -0.5f, 0.8f, 1.0f};  // xyz = dir, w = intensity
        glm::vec4 sun_color{1.0f, 1.0f, 0.9f, 0.0f};       // rgb, a = unused
        glm::vec4 ambient_color{0.15f, 0.15f, 0.2f, 0.25f}; // rgb, a = intensity
        glm::vec4 fog_color{0.5f, 0.5f, 0.6f, 0.0f};       // rgb, a = density
        glm::ivec4 light_count{0, 0, 0, 0};                 // x = active point light count
        PointLight lights[MAX_POINT_LIGHTS]{};
    };
    VkBuffer      m_env_ubo_buffer = VK_NULL_HANDLE;
    VmaAllocation m_env_ubo_alloc  = VK_NULL_HANDLE;
    void*         m_env_ubo_mapped = nullptr;
    GpuTexture    m_default_cubemap{};  // 1x1 fallback when no skybox
    glm::vec3     m_sun_direction{-0.4f, -0.5f, 0.8f};  // cached for shadow pass
    EnvironmentUBO m_env_data{};  // cached, updated with point lights each frame

    // Default texture for untextured meshes
    GpuTexture      m_default_texture{};
    MeshMaterial    m_default_material{};

    // Corpse material (dark gray)
    GpuTexture      m_corpse_texture{};
    MeshMaterial    m_corpse_material{};

    // Terrain material (layer array texture + transition noise)
    TerrainMaterial m_terrain_material{};
    GpuTexture      m_transition_noise{};  // single noise texture for curve perturbation
    GpuTexture      m_water_normal{};      // tileable water normal map for ripples
    bool create_transition_noise();
    bool create_water_normal();

    // Water surface rendering data (computed from tileset)
    struct WaterParams {
        glm::vec3 shallow_color{0.18f, 0.45f, 0.55f};
        glm::vec3 deep_color{0.05f, 0.12f, 0.25f};
        u32 water_mask = 0;   // bitmask: which layer IDs are any water
        u32 deep_mask  = 0;   // bitmask: which layer IDs are deep water
    } m_water_params{};
    f32 m_elapsed_time = 0.0f;

    // Cached loaded models (model_path → LoadedModel)
    std::unordered_map<std::string, LoadedModel> m_model_cache;
    std::unordered_set<std::string> m_model_failed;  // paths that failed to load

    // Cached GPU meshes for special built-in meshes (projectile, etc.)
    std::unordered_map<std::string, GpuMesh> m_mesh_cache;

    // Placeholder mesh for models that fail to load
    GpuMesh m_placeholder_mesh{};

    // Small projectile mesh
    GpuMesh m_projectile_mesh{};

    // ── GPU-driven rendering (Phase 14a/b) ──────────────────────────────
    static constexpr u32 MAX_STATIC_INSTANCES = 4096;

    // Mega vertex/index buffers — all static meshes share one VB+IB (Phase 14b)
    static constexpr u32 MEGA_MAX_VERTICES = 512 * 1024;
    static constexpr u32 MEGA_MAX_INDICES  = 2 * 1024 * 1024;
    VkBuffer        m_mega_vb        = VK_NULL_HANDLE;
    VmaAllocation   m_mega_vb_alloc  = VK_NULL_HANDLE;
    void*           m_mega_vb_mapped = nullptr;
    u32             m_mega_vb_used   = 0;   // next free vertex slot
    VkBuffer        m_mega_ib        = VK_NULL_HANDLE;
    VmaAllocation   m_mega_ib_alloc  = VK_NULL_HANDLE;
    void*           m_mega_ib_mapped = nullptr;
    u32             m_mega_ib_used   = 0;   // next free index slot
    GpuMesh upload_to_mega(const asset::MeshData& mesh);

    // Bindless texture array (Phase 14b)
    static constexpr u32 MAX_BINDLESS_TEXTURES = 256;
    VkDescriptorSetLayout m_bindless_layout  = VK_NULL_HANDLE;
    VkDescriptorPool      m_bindless_pool    = VK_NULL_HANDLE;
    VkDescriptorSet       m_bindless_set     = VK_NULL_HANDLE;
    u32                   m_bindless_count    = 0;
    u32                   m_default_tex_idx  = 0;
    u32                   m_corpse_tex_idx   = 0;
    bool create_bindless_resources();
    u32  register_bindless_texture(const GpuTexture& tex);

    // Per-frame instance SSBO (InstanceData for all static-mesh entities)
    VkBuffer        m_instance_buffer   = VK_NULL_HANDLE;
    VmaAllocation   m_instance_alloc    = VK_NULL_HANDLE;
    void*           m_instance_mapped   = nullptr;
    VkDescriptorSet m_instance_desc_set = VK_NULL_HANDLE;

    // Per-frame indirect draw command buffer
    VkBuffer        m_indirect_buffer = VK_NULL_HANDLE;
    VmaAllocation   m_indirect_alloc  = VK_NULL_HANDLE;
    void*           m_indirect_mapped = nullptr;

    // Draw group: one indirect draw per unique mesh geometry
    struct DrawGroup {
        u32 first_index;      // into mega index buffer
        u32 index_count;
        i32 vertex_offset;    // into mega vertex buffer
        u32 first_instance;   // offset into instance SSBO
        u32 instance_count;
    };
    std::vector<DrawGroup>  m_draw_groups;
    u32                     m_static_instance_count = 0;

    void build_static_draw_batches(const simulation::World& world, f32 alpha);

    // Particle pipeline (alpha-blended, depth test on, depth write off)
    VkPipelineLayout m_particle_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_particle_pipeline        = VK_NULL_HANDLE;

    // Fog of war texture (R8, tiles_x * tiles_y, updated per frame)
    GpuTexture  m_fog_texture{};
    VkBuffer    m_fog_staging_buffer = VK_NULL_HANDLE;
    VmaAllocation m_fog_staging_alloc = VK_NULL_HANDLE;
    void*       m_fog_staging_mapped = nullptr;
    u32         m_fog_width = 0;
    u32         m_fog_height = 0;
    bool        m_fog_enabled = false;
    bool        m_fog_dirty = false;

    // Simulation reference for fog queries
    const simulation::Simulation* m_simulation = nullptr;
    u32 m_local_player_id = 0;

    // Terrain mesh
    TerrainMesh m_terrain{};

    // Per-entity animation instances (entity id → AnimationInstance)
    std::unordered_map<u32, AnimationInstance> m_anim_instances;

    // Particle system + effect system
    ParticleSystem  m_particles;
    EffectRegistry  m_effect_registry;
    EffectManager   m_effect_manager;

    // Get or create animation instance for an entity using its model.
    AnimationInstance& get_or_create_anim(u32 entity_id, LoadedModel& model);
};

} // namespace uldum::render
