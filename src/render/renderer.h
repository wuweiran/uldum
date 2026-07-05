#pragma once

#include "render/camera.h"
#include "render/gpu_mesh.h"
#include "render/gpu_texture.h"
#include "render/terrain.h"
#include "render/material.h"
#include "render/shadow.h"
#include "render/animation.h"
#include "render/particles.h"
#include "render/glow_system.h"
#include "render/effect.h"
#include "core/handle.h"
#include "asset/model.h"

#include "rhi/rhi.h"
#include "rhi/command_list.h"

#include <glm/mat4x4.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
namespace uldum::simulation { struct World; struct Transform; class Simulation; }
namespace uldum::platform { struct InputState; }
namespace uldum::map { struct TerrainData; struct Tileset; struct EnvironmentConfig; }

namespace uldum::render {

// One primitive of a model: a slice of the mega buffer plus the
// material that drives its draw state (texture binding, color
// multiplier, alpha pipeline variant, cull mode). A single-material
// model has exactly one Submesh; a multi-material model has one per
// glTF primitive.
struct Submesh {
    GpuMesh           mesh{};                    // range into the mega vert/idx buffers
    u32               texture_index   = 0;       // bindless slot for baseColorTexture, or default
    glm::vec4         base_color_factor{1, 1, 1, 1};
    asset::AlphaMode  alpha_mode      = asset::AlphaMode::Opaque;
    f32               alpha_cutoff    = 0.5f;
    bool              double_sided    = false;
    rhi::DescriptorSetHandle descriptor_set{};   // skinned pipeline: per-submesh diffuse (non-bindless)
};

// A fully loaded model: CPU data + GPU submeshes.
//
// `mesh` is the model-wide range (covers every submesh contiguously in
// the mega buffer) — kept around for frustum-cull bounding radius and
// any legacy single-draw path. Per-primitive draw state lives on
// `submeshes`, with one entry per glTF primitive.
struct LoadedModel {
    asset::ModelData     data;
    std::vector<Submesh> submeshes;
    GpuMesh              mesh{};            // merged extents (bounding radius lookup, legacy single-binding draws)
    GpuTexture           diffuse_texture{}; // first submesh's image (skinned descriptor)
    MeshMaterial         material{};        // first submesh's descriptor (skinned pipeline)
    bool                 is_skinned = false;
};

// Per-instance data for static mesh SSBO (std430 aligned). Each
// (entity × submesh) pair contributes one instance: `material_index`
// indexes the bindless texture array, `base_color_factor` multiplies
// the sample, `alpha` is the SetUnitAlpha multiplier. `alpha_cutoff`
// drives the MASK pipeline's discard test (unused in Opaque).
struct InstanceData {
    glm::mat4 model;              // 64 bytes
    glm::vec4 base_color_factor;  // 16 bytes
    u32       material_index;     //  4 bytes — bindless texture slot
    f32       alpha;              //  4 bytes — visual_alpha (1.0 = opaque)
    f32       alpha_cutoff;       //  4 bytes — MASK threshold
    u32       _pad;               //  4 bytes — align to 16
};

class Renderer {
public:
    bool init(rhi::Rhi& rhi);
    void shutdown();

    // Tear down anything scoped to a single game session — currently the
    // per-entity animation instances. Must be called from App::end_session
    // before the simulation handles get reused by the next map; otherwise
    // reused entity ids pick up stale bone state from the previous
    // session (visible as detached body parts / broken skeleton).
    void end_session();

    // Drop all per-entity animation instances (bone buffers) without touching
    // effects/particles. Called on a scene swap: the new scene recycles the old
    // entity ids, and these instances are keyed by id, so they must be dropped
    // or a new unit could inherit a dead one's bone buffer.
    void clear_animations();

    void update_camera(const platform::InputState& input, f32 dt);
    void handle_resize(f32 aspect);

    // Set the map root directory for resolving model asset paths.
    void set_map_root(std::string_view root) { m_map_root = root; }

    // Set the active terrain. Builds the GPU mesh and splatmap and
    // stores a CPU pointer for per-frame queries (entity slope tilt,
    // future terrain-aware overlays). Pass nullptr to tear down both.
    // The pointer must outlive the renderer's use of it — typically
    // the map's TerrainData lives on MapManager across the session.
    void set_terrain(const map::TerrainData* terrain);

    // Load terrain layer textures from tileset (call after set_map_root, before set_terrain).
    void load_tileset_textures(const map::Tileset& tileset);

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
    void upload_fog(rhi::CommandList& cmd);

    // Record shadow depth pass (must be called before begin_rendering).
    // alpha: interpolation factor between previous and current tick (0..1).
    // `world` is non-const because the renderer advances
    // World::anim_queues as script-driven clips finish.
    void draw_shadows(rhi::CommandList& cmd, simulation::World& world, f32 alpha = 1.0f);

    // Record main pass draw commands into the given command buffer.
    // Reads Transform + Renderable components from the world.
    // alpha: interpolation factor between previous and current tick (0..1).
    // `world` is non-const for the same reason as draw_shadows.
    // `on_after_entities` (optional): invoked after the opaque unit-mesh
    // passes but before particles/glow. World decals (selection rings,
    // ability indicators) record their draws here so they depth-test
    // against unit meshes — a unit genuinely in front of an air unit's
    // ring occludes it, one behind does not. Trade: a translucent body
    // (Wind Walk) no longer shows the ring through its silhouette, which
    // is far less jarring than the ring floating over nearer ground units.
    void draw(rhi::CommandList& cmd, rhi::Extent2D extent, simulation::World& world,
              f32 alpha = 1.0f,
              const std::function<void()>& on_after_entities = {});

    Camera& camera() { return m_camera; }
    const Camera& camera() const { return m_camera; }
    ParticleSystem& particles() { return m_particles; }
    EffectRegistry& effect_registry() { return m_effect_registry; }
    EffectManager&  effect_manager()  { return m_effect_manager; }

    // Get attachment point position in model-local space for a unit. Returns {0,0,0} if not found.
    glm::vec3 get_attachment_point(u32 entity_id, std::string_view bone_name) const;

    // Resolve a clip's duration on a model (used by simulation to size
    // projectile death timers to the actual animation length).
    // Returns 0 if the model or clip is missing.
    f32 clip_duration(std::string_view model_path, std::string_view clip_name);

private:
    bool create_descriptor_layouts();
    bool create_mesh_pipeline();
    bool create_skinned_mesh_pipeline();
    bool create_particle_pipeline();
    bool create_glow_pipeline();
    bool create_terrain_pipeline();
    bool create_water_pipeline();
    bool create_shadow_pipeline();
    bool create_shadow_resources();
    bool create_default_texture();
    bool create_terrain_textures();
    LoadedModel* get_or_load_model(const std::string& model_path);
    GpuMesh& get_or_upload_mesh(const std::string& model_path);
    rhi::DescriptorSetHandle allocate_mesh_descriptor(const GpuTexture& diffuse);
    rhi::DescriptorSetHandle allocate_terrain_descriptor(const TerrainMaterial& mat);
    rhi::DescriptorSetHandle allocate_shadow_descriptor();
    rhi::DescriptorSetHandle allocate_bone_descriptor(rhi::BufferHandle bone_buffer, usize size);

    void draw_shadow_pass(rhi::CommandList& cmd, simulation::World& world, f32 alpha);

    // Returns true if an entity should be hidden by fog of war (enemy in non-visible tile).
    bool is_fog_hidden(const simulation::World& world, u32 id, const simulation::Transform& t) const;
    // Returns true if a static-remembered entity is being shown from
    // the player's *memory* (Explored tile) rather than live vision.
    // Draw paths use this to apply the kFoggedMemoryAlpha dim cue.
    bool is_in_fog_memory(const simulation::World& world, u32 id) const;

    rhi::Rhi* m_rhi = nullptr;
    const map::TerrainData* m_terrain_data = nullptr;
    Camera          m_camera;
    std::string     m_map_root;  // map root for resolving model paths

    // Descriptor infrastructure
    rhi::DescriptorSetLayoutHandle m_mesh_desc_layout{};    // set 0: 1 diffuse sampler
    rhi::DescriptorSetLayoutHandle m_terrain_desc_layout{}; // set 0: 4 layers + 1 splatmap
    rhi::DescriptorSetLayoutHandle m_shadow_desc_layout{};  // set 1: UBO + shadow map

    // Mesh pipeline (set 0 = material, set 1 = shadow).
    // _mask variant adds a fragment-shader discard for glTF alphaMode=MASK
    // primitives. Both pipelines share the same layout; only the fragment
    // shader differs. Both opt into dynamic cull-mode for doubleSided.
    rhi::PipelineLayoutHandle m_mesh_pipeline_layout{};
    rhi::PipelineHandle       m_mesh_pipeline{};
    rhi::PipelineHandle       m_mesh_mask_pipeline{};

    // Terrain pipeline (set 0 = terrain material, set 1 = shadow)
    rhi::PipelineLayoutHandle m_terrain_pipeline_layout{};
    rhi::PipelineHandle       m_terrain_pipeline{};

    // Water surface pipeline (transparent overlay, same mesh as terrain)
    rhi::PipelineLayoutHandle m_water_pipeline_layout{};
    rhi::PipelineHandle       m_water_pipeline{};

    // Skinned mesh pipeline (set 0 = material, set 1 = shadow, set 2 = bones SSBO)
    rhi::DescriptorSetLayoutHandle m_bone_desc_layout{};   // set 2: 1 SSBO
    rhi::PipelineLayoutHandle      m_skinned_mesh_pipeline_layout{};
    rhi::PipelineHandle            m_skinned_mesh_pipeline{};
    rhi::PipelineLayoutHandle      m_skinned_shadow_pipeline_layout{};
    rhi::PipelineHandle            m_skinned_shadow_pipeline{};

    // Skybox pipeline (set 0 = cubemap, push constant = VP without translation)
    rhi::DescriptorSetLayoutHandle m_skybox_desc_layout{};
    rhi::PipelineLayoutHandle      m_skybox_pipeline_layout{};
    rhi::PipelineHandle            m_skybox_pipeline{};
    rhi::DescriptorSetHandle       m_skybox_desc_set{};
    GpuTexture       m_skybox_cubemap{};
    GpuMesh          m_skybox_mesh{};
    bool             m_has_skybox = false;
    bool create_skybox_pipeline();
    bool create_skybox_mesh();

    // Shadow depth pass pipeline (push constant = light MVP, depth-only).
    // The _mask variant adds bindless textures (set 1) and a fragment
    // shader that samples + alpha-tests, so cut-out geometry casts the
    // correct silhouette into the shadow map.
    rhi::PipelineLayoutHandle m_shadow_pipeline_layout{};
    rhi::PipelineHandle       m_shadow_pipeline{};
    rhi::PipelineLayoutHandle m_shadow_mask_pipeline_layout{};
    rhi::PipelineHandle       m_shadow_mask_pipeline{};
    rhi::PipelineLayoutHandle m_terrain_shadow_pipeline_layout{};
    rhi::PipelineHandle       m_terrain_shadow_pipeline{};

    // Shadow map resources
    ShadowMap                m_shadow_map{};
    ShadowBuffer             m_shadow_ubo{};
    rhi::DescriptorSetHandle m_shadow_desc_set{};

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
    rhi::BufferHandle m_env_ubo_buffer{};
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
        u32 water_mask = 0;   // bitmask: which layer IDs are any water
        u32 deep_mask  = 0;   // bitmask: which layer IDs are deep water
    } m_water_params{};
    static constexpr u32 WATER_COLOR_SLOTS = 16;  // per-layer water tint slots (indexed by layer id)
    rhi::BufferHandle m_water_color_ubo{};        // vec4[16] tint per tileset layer id
    f32 m_elapsed_time = 0.0f;

    // Cached loaded models (model_path → LoadedModel)
    std::unordered_map<std::string, LoadedModel> m_model_cache;
    std::unordered_set<std::string> m_model_failed;  // paths that failed to load

    // Cached GPU meshes for special built-in meshes (projectile, etc.)
    std::unordered_map<std::string, GpuMesh> m_mesh_cache;

    // Post-init baselines for the mega buffer + bindless array. end_session
    // rolls back to these so the next map's same-path-different-bytes models
    // re-upload cleanly instead of returning stale cache entries.
    u32 m_init_mega_vb_used   = 0;
    u32 m_init_mega_ib_used   = 0;
    u32 m_init_bindless_count = 0;

    // Placeholder mesh for models that fail to load
    GpuMesh m_placeholder_mesh{};

    // Small projectile mesh
    GpuMesh m_projectile_mesh{};

    // ── GPU-driven rendering (Phase 14a/b) ──────────────────────────────
    static constexpr u32 MAX_STATIC_INSTANCES = 4096;

    // Mega vertex/index buffers — all static meshes share one VB+IB (Phase 14b)
    static constexpr u32 MEGA_MAX_VERTICES = 512 * 1024;
    static constexpr u32 MEGA_MAX_INDICES  = 2 * 1024 * 1024;
    rhi::BufferHandle m_mega_vb{};
    u32               m_mega_vb_used = 0;   // next free vertex slot
    rhi::BufferHandle m_mega_ib{};
    u32               m_mega_ib_used = 0;   // next free index slot
    GpuMesh upload_to_mega(const asset::MeshData& mesh);

    // Bindless texture array (Phase 14b)
    static constexpr u32 MAX_BINDLESS_TEXTURES = 256;
    rhi::DescriptorSetLayoutHandle m_bindless_layout{};
    rhi::DescriptorSetHandle       m_bindless_set{};
    u32                            m_bindless_count   = 0;
    u32                            m_default_tex_idx  = 0;
    u32                            m_corpse_tex_idx   = 0;
    bool create_bindless_resources();
    // Backend-aware unit-texture registration. Vulkan path writes the
    // descriptor at array_element = next index (bindless sampler array).
    // GLES path resizes pixels to UNIT_TEX_SIZE and uploads them to the
    // next layer of m_unit_tex_array (a 2D texture array sampled in
    // mesh.frag.gles via material_index). One of `tex` / `pixels` is the
    // authoritative input per backend; the other can be defaulted.
    u32  register_unit_texture(const GpuTexture& tex, const u8* pixels, u32 width, u32 height);

#if defined(ULDUM_BACKEND_GLES)
    // GLES sampler2DArray that replaces the Vulkan bindless sampler[]
    // array. All unit textures get resized to UNIT_TEX_SIZE×UNIT_TEX_SIZE
    // and uploaded as layers; mesh.frag samples with material_index as
    // the layer coordinate. Capped at 64 layers — enough for typical
    // unit / doodad rosters without burning 64MB of VRAM.
    static constexpr u32 UNIT_TEX_SIZE   = 256;
    static constexpr u32 UNIT_TEX_LAYERS = 64;
    rhi::TextureHandle m_unit_tex_array{};
    rhi::SamplerHandle m_unit_tex_sampler{};
#endif

    // Ring-buffered by frame_index() — CPU writes frame N+1 while the GPU
    // reads frame N. Without this, layout shifts on entity culling alias
    // one mesh's instance slot onto another's.
    rhi::BufferHandle        m_instance_buffer  [rhi::MAX_FRAMES_IN_FLIGHT] = {};
    rhi::DescriptorSetHandle m_instance_desc_set[rhi::MAX_FRAMES_IN_FLIGHT] = {};

    rhi::BufferHandle m_indirect_buffer[rhi::MAX_FRAMES_IN_FLIGHT] = {};

    // Draw group: one indirect draw per (geometry × pipeline state)
    // combination. `pipeline_class` separates Opaque (0) from Mask (1)
    // because they bind different pipelines (alpha-test discard).
    // `double_sided` flips cull mode at draw time (back-cull vs no-cull).
    struct DrawGroup {
        u32 first_index;      // into mega index buffer
        u32 index_count;
        i32 vertex_offset;    // into mega vertex buffer
        u32 first_instance;   // offset into instance SSBO
        u32 instance_count;
        u8  pipeline_class;   // 0 = Opaque, 1 = Mask
        u8  double_sided;     // 0 = back-cull, 1 = no-cull
    };
    std::vector<DrawGroup>  m_draw_groups;
    u32                     m_static_instance_count = 0;

    // After build_static_draw_batches, m_draw_groups is sorted by
    // (pipeline_class, double_sided) so each combo is a contiguous slice
    // ready for one multi-draw-indirect dispatch. Partition indices map
    // to: 0 = opaque+back-cull, 1 = opaque+no-cull, 2 = mask+back-cull,
    // 3 = mask+no-cull.
    struct DrawPartition {
        u32 first = 0;
        u32 count = 0;
    };
    DrawPartition m_static_partitions[4];

    static constexpr u8 partition_index(u8 pipeline_class, u8 double_sided) {
        return static_cast<u8>((pipeline_class << 1) | double_sided);
    }

    void build_static_draw_batches(const simulation::World& world, f32 alpha);

    // Particle pipeline (alpha-blended, depth test on, depth write off)
    rhi::PipelineLayoutHandle m_particle_pipeline_layout{};
    rhi::PipelineHandle       m_particle_pipeline{};

    // Glow pipeline (additive, depth test on, depth write off) — engine
    // light visuals (volumetric Tyndall shafts). Push constant: mat4 vp + float time.
    rhi::PipelineLayoutHandle m_glow_pipeline_layout{};
    rhi::PipelineHandle       m_glow_pipeline{};

    // Fog of war texture (R8, tiles_x * tiles_y, updated per frame)
    GpuTexture  m_fog_texture{};
    rhi::BufferHandle m_fog_staging_buffer{};
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

    // Particle system + glow system + effect system
    ParticleSystem  m_particles;
    GlowSystem      m_glow;
    EffectRegistry  m_effect_registry;
    EffectManager   m_effect_manager;

    // Get or create animation instance for an entity using its model.
    // `play_birth` decides the INITIAL state on creation: a freshly-born
    // unit in the player's sight starts in Birth, anything else (revealed
    // out of fog, or flagged skip_birth) starts in Idle. Ignored once the
    // instance exists.
    AnimationInstance& get_or_create_anim(u32 entity_id, LoadedModel& model, bool play_birth);
};

} // namespace uldum::render
