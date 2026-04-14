#include "render/renderer.h"
#include "rhi/vulkan/vulkan_rhi.h"
#include "map/terrain_data.h"
#include "simulation/world.h"
#include "simulation/simulation.h"
#include "simulation/components.h"
#include "simulation/type_registry.h"
#include "asset/model.h"
#include "core/log.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace uldum::render {

static constexpr const char* TAG = "Render";

// ── Shader loading helper ──────────────────────────────────────────────────

static VkShaderModule load_shader(VkDevice device, std::string_view path) {
    std::string path_str(path);
    std::ifstream file(path_str, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        log::error(TAG, "Failed to open shader '{}'", path);
        return VK_NULL_HANDLE;
    }
    auto size = file.tellg();
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), size);

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &mod);
    return mod;
}

// ── Procedural texture generation ─────────────────────────────────────────

static std::vector<u8> generate_solid_texture(u32 size, u8 r, u8 g, u8 b) {
    std::vector<u8> pixels(size * size * 4);
    for (u32 i = 0; i < size * size; ++i) {
        // Add subtle noise for visual interest
        u8 noise = static_cast<u8>((i * 7 + (i / size) * 13) % 16);
        pixels[i * 4]     = static_cast<u8>(std::min(255, r + noise));
        pixels[i * 4 + 1] = static_cast<u8>(std::min(255, g + noise));
        pixels[i * 4 + 2] = static_cast<u8>(std::min(255, b + noise / 2));
        pixels[i * 4 + 3] = 255;
    }
    return pixels;
}

// ── Render interpolation helper ────────────────────────────────────────────

// Interpolate between previous and current transform for smooth rendering.
static glm::vec3 lerp_position(const simulation::Transform& t, f32 alpha) {
    return glm::mix(t.prev_position, t.position, alpha);
}

static f32 lerp_facing(const simulation::Transform& t, f32 alpha) {
    // Shortest-path angle interpolation
    f32 diff = t.facing - t.prev_facing;
    // Normalize to [-π, π]
    while (diff > glm::pi<f32>()) diff -= glm::two_pi<f32>();
    while (diff < -glm::pi<f32>()) diff += glm::two_pi<f32>();
    return t.prev_facing + diff * alpha;
}

// ── Terrain slope tilt helper ──────────────────────────────────────────────

// Build a rotation matrix that tilts an entity to match the terrain slope.
// terrain_normal is the surface normal at the entity position (Z-up).
static glm::mat4 slope_tilt_matrix(const glm::vec3& terrain_normal) {
    glm::vec3 up{0.0f, 0.0f, 1.0f};
    glm::vec3 n = glm::normalize(terrain_normal);
    f32 dot = glm::dot(up, n);
    if (dot > 0.999f) return glm::mat4{1.0f};  // flat terrain, no tilt

    glm::vec3 axis = glm::cross(up, n);
    f32 axis_len = glm::length(axis);
    if (axis_len < 0.001f) return glm::mat4{1.0f};
    axis /= axis_len;

    f32 angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
    return glm::rotate(glm::mat4{1.0f}, angle, axis);
}

// ── Init / Shutdown ────────────────────────────────────────────────────────

bool Renderer::init(rhi::VulkanRhi& rhi) {
    m_rhi = &rhi;

    f32 aspect = static_cast<f32>(rhi.extent().width) / static_cast<f32>(rhi.extent().height);
    m_camera.init(aspect);

    if (!create_descriptor_layouts()) return false;
    if (!create_bindless_resources()) return false;
    if (!create_shadow_resources()) return false;
    if (!create_default_texture()) return false;
    if (!create_terrain_textures()) return false;
    if (!create_mesh_pipeline()) return false;
    if (!create_skinned_mesh_pipeline()) return false;
    if (!create_particle_pipeline()) return false;
    if (!create_terrain_pipeline()) return false;
    if (!create_shadow_pipeline()) return false;
    if (!m_particles.init(rhi)) return false;
    m_effect_registry.register_defaults();
    m_effect_manager.set_particles(&m_particles);
    m_effect_manager.set_registry(&m_effect_registry);

    // Mega vertex/index buffers — all static meshes share one VB + IB (Phase 14b)
    {
        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VkBufferCreateInfo vb_ci{};
        vb_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        vb_ci.size  = MEGA_MAX_VERTICES * sizeof(asset::Vertex);
        vb_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationInfo vb_info{};
        vmaCreateBuffer(rhi.allocator(), &vb_ci, &alloc_ci,
                        &m_mega_vb, &m_mega_vb_alloc, &vb_info);
        m_mega_vb_mapped = vb_info.pMappedData;

        VkBufferCreateInfo ib_ci{};
        ib_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ib_ci.size  = MEGA_MAX_INDICES * sizeof(u32);
        ib_ci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        VmaAllocationInfo ib_info{};
        vmaCreateBuffer(rhi.allocator(), &ib_ci, &alloc_ci,
                        &m_mega_ib, &m_mega_ib_alloc, &ib_info);
        m_mega_ib_mapped = ib_info.pMappedData;

        log::info(TAG, "Mega buffers created: VB {} verts, IB {} indices",
                  MEGA_MAX_VERTICES, MEGA_MAX_INDICES);
    }

    // Instance SSBO for static mesh instancing (Phase 14b: InstanceData with material_index)
    {
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size  = MAX_STATIC_INSTANCES * sizeof(InstanceData);
        buf_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo alloc_info{};
        vmaCreateBuffer(rhi.allocator(), &buf_ci, &alloc_ci,
                        &m_instance_buffer, &m_instance_alloc, &alloc_info);
        m_instance_mapped = alloc_info.pMappedData;

        m_instance_desc_set = allocate_bone_descriptor(m_instance_buffer,
                                                        MAX_STATIC_INSTANCES * sizeof(InstanceData));
    }

    // Indirect draw command buffer
    {
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size  = MAX_STATIC_INSTANCES * sizeof(VkDrawIndexedIndirectCommand);
        buf_ci.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo alloc_info{};
        vmaCreateBuffer(rhi.allocator(), &buf_ci, &alloc_ci,
                        &m_indirect_buffer, &m_indirect_alloc, &alloc_info);
        m_indirect_mapped = alloc_info.pMappedData;
    }

    // Create a placeholder box mesh for entities without a real model.
    // Defined directly in Z-up game coordinates: base at Z=0, top at Z=2.
    asset::MeshData placeholder;
    const float s = 16.0f;   // half-width (~32 game units, WC3 collision size)
    const float h = 64.0f;  // height
    // UVs map to the default texture (white with warm tint from texture)
    placeholder.vertices = {
        // Top face (Z+)
        {{-s, -s, h}, {0,0,1}, {0,0}}, {{ s, -s, h}, {0,0,1}, {1,0}},
        {{ s,  s, h}, {0,0,1}, {1,1}}, {{-s,  s, h}, {0,0,1}, {0,1}},
        // Bottom face (Z-)
        {{-s,  s, 0}, {0,0,-1}, {0,0}}, {{ s,  s, 0}, {0,0,-1}, {1,0}},
        {{ s, -s, 0}, {0,0,-1}, {1,1}}, {{-s, -s, 0}, {0,0,-1}, {0,1}},
        // Front face (Y+)
        {{-s, s, 0}, {0,1,0}, {0,0}}, {{ s, s, 0}, {0,1,0}, {1,0}},
        {{ s, s, h}, {0,1,0}, {1,1}}, {{-s, s, h}, {0,1,0}, {0,1}},
        // Back face (Y-)
        {{ s, -s, 0}, {0,-1,0}, {0,0}}, {{-s, -s, 0}, {0,-1,0}, {1,0}},
        {{-s, -s, h}, {0,-1,0}, {1,1}}, {{ s, -s, h}, {0,-1,0}, {0,1}},
        // Right face (X+)
        {{ s, s, 0}, {1,0,0}, {0,0}}, {{ s, -s, 0}, {1,0,0}, {1,0}},
        {{ s, -s, h}, {1,0,0}, {1,1}}, {{ s,  s, h}, {1,0,0}, {0,1}},
        // Left face (X-)
        {{-s, -s, 0}, {-1,0,0}, {0,0}}, {{-s,  s, 0}, {-1,0,0}, {1,0}},
        {{-s,  s, h}, {-1,0,0}, {1,1}}, {{-s, -s, h}, {-1,0,0}, {0,1}},
    };
    // Winding convention: cross(e1, e2) must equal outward face normal (matches terrain).
    placeholder.indices = {
         0, 1, 3,  1, 2, 3,   // top    (+Z)
         4, 5, 7,  5, 6, 7,   // bottom (-Z)
         8,10, 9,  8,11,10,   // front  (+Y)
        12,14,13, 12,15,14,   // back   (-Y)
        16,18,17, 16,19,18,   // right  (+X)
        20,22,21, 20,23,22,   // left   (-X)
    };
    m_placeholder_mesh = upload_to_mega(placeholder);
    m_placeholder_mesh.native_z_up = true;

    // Create a small projectile mesh (elongated diamond shape in Z-up game coords)
    {
        asset::MeshData proj;
        const float r = 8.0f;   // radius
        const float l = 24.0f;  // half-length along Y (forward)
        proj.vertices = {
            // Tip (front, +Y)
            {{0, l, 0},  {0, 1, 0}, {0.5f, 0}},
            // Tail (back, -Y)
            {{0, -l, 0}, {0,-1, 0}, {0.5f, 1}},
            // Right (+X)
            {{r, 0, 0},  {1, 0, 0}, {1, 0.5f}},
            // Left (-X)
            {{-r, 0, 0}, {-1,0, 0}, {0, 0.5f}},
            // Top (+Z)
            {{0, 0, r},  {0, 0, 1}, {0.5f, 0.5f}},
            // Bottom (-Z)
            {{0, 0, -r}, {0, 0,-1}, {0.5f, 0.5f}},
        };
        proj.indices = {
            0,2,4,  0,4,3,  0,3,5,  0,5,2,  // front 4 faces
            1,4,2,  1,3,4,  1,5,3,  1,2,5,  // back 4 faces
        };
        m_projectile_mesh = upload_to_mega(proj);
        m_projectile_mesh.native_z_up = true;
    }

    log::info(TAG, "Renderer initialized — textured mesh + skinned + terrain pipelines ready");
    return true;
}

void Renderer::shutdown() {
    if (!m_rhi) return;
    VkDevice device = m_rhi->device();
    VmaAllocator alloc = m_rhi->allocator();

    vkDeviceWaitIdle(device);

    m_particles.shutdown();
    destroy_terrain_mesh(alloc, m_terrain);
    for (auto& [eid, anim] : m_anim_instances) {
        // Descriptor sets freed implicitly when pools are destroyed below
        if (anim.bone_buffer) vmaDestroyBuffer(alloc, anim.bone_buffer, anim.bone_alloc);
    }
    m_anim_instances.clear();
    for (auto& [path, lm] : m_model_cache) {
        destroy_mesh(alloc, lm.mesh);
        if (lm.diffuse_texture.image) destroy_texture(*m_rhi, lm.diffuse_texture);
    }
    m_model_cache.clear();
    m_model_failed.clear();
    destroy_mesh(alloc, m_projectile_mesh);
    destroy_mesh(alloc, m_placeholder_mesh);
    for (auto& [path, mesh] : m_mesh_cache) {
        destroy_mesh(alloc, mesh);
    }
    m_mesh_cache.clear();

    // Destroy instance/indirect/mega buffers
    if (m_instance_buffer) vmaDestroyBuffer(alloc, m_instance_buffer, m_instance_alloc);
    if (m_indirect_buffer) vmaDestroyBuffer(alloc, m_indirect_buffer, m_indirect_alloc);
    if (m_mega_vb) vmaDestroyBuffer(alloc, m_mega_vb, m_mega_vb_alloc);
    if (m_mega_ib) vmaDestroyBuffer(alloc, m_mega_ib, m_mega_ib_alloc);

    // Destroy bindless resources
    if (m_bindless_set)    { /* freed when pool is destroyed */ }
    if (m_bindless_pool)   vkDestroyDescriptorPool(device, m_bindless_pool, nullptr);
    if (m_bindless_layout) vkDestroyDescriptorSetLayout(device, m_bindless_layout, nullptr);

    // Destroy fog resources
    if (m_fog_texture.image) destroy_texture(*m_rhi, m_fog_texture);
    if (m_fog_staging_buffer) {
        // No need to unmap — VMA persistent mapping freed with buffer
        vmaDestroyBuffer(alloc, m_fog_staging_buffer, m_fog_staging_alloc);
    }

    // Destroy textures
    destroy_texture(*m_rhi, m_corpse_texture);
    destroy_texture(*m_rhi, m_default_texture);
    for (u32 i = 0; i < m_terrain_material.layer_count; ++i) {
        destroy_texture(*m_rhi, m_terrain_material.layers[i]);
    }

    // Destroy shadow resources
    destroy_shadow_map(*m_rhi, m_shadow_map);
    destroy_shadow_buffer(*m_rhi, m_shadow_ubo);

    // Destroy pipelines
    if (m_particle_pipeline)              vkDestroyPipeline(device, m_particle_pipeline, nullptr);
    if (m_particle_pipeline_layout)       vkDestroyPipelineLayout(device, m_particle_pipeline_layout, nullptr);
    if (m_skinned_shadow_pipeline)        vkDestroyPipeline(device, m_skinned_shadow_pipeline, nullptr);
    if (m_skinned_shadow_pipeline_layout) vkDestroyPipelineLayout(device, m_skinned_shadow_pipeline_layout, nullptr);
    if (m_skinned_mesh_pipeline)          vkDestroyPipeline(device, m_skinned_mesh_pipeline, nullptr);
    if (m_skinned_mesh_pipeline_layout)   vkDestroyPipelineLayout(device, m_skinned_mesh_pipeline_layout, nullptr);
    if (m_terrain_shadow_pipeline)  vkDestroyPipeline(device, m_terrain_shadow_pipeline, nullptr);
    // m_terrain_shadow_pipeline_layout is shared with m_shadow_pipeline_layout, don't destroy twice
    if (m_shadow_pipeline)         vkDestroyPipeline(device, m_shadow_pipeline, nullptr);
    if (m_shadow_pipeline_layout)  vkDestroyPipelineLayout(device, m_shadow_pipeline_layout, nullptr);
    if (m_terrain_pipeline)        vkDestroyPipeline(device, m_terrain_pipeline, nullptr);
    if (m_terrain_pipeline_layout) vkDestroyPipelineLayout(device, m_terrain_pipeline_layout, nullptr);
    if (m_mesh_pipeline)           vkDestroyPipeline(device, m_mesh_pipeline, nullptr);
    if (m_mesh_pipeline_layout)    vkDestroyPipelineLayout(device, m_mesh_pipeline_layout, nullptr);

    // Destroy descriptor infrastructure
    for (auto pool : m_descriptor_pools) {
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
    }
    m_descriptor_pools.clear();
    if (m_bone_desc_layout)     vkDestroyDescriptorSetLayout(device, m_bone_desc_layout, nullptr);
    if (m_shadow_desc_layout)   vkDestroyDescriptorSetLayout(device, m_shadow_desc_layout, nullptr);
    if (m_terrain_desc_layout)  vkDestroyDescriptorSetLayout(device, m_terrain_desc_layout, nullptr);
    if (m_mesh_desc_layout)     vkDestroyDescriptorSetLayout(device, m_mesh_desc_layout, nullptr);

    m_rhi = nullptr;
    log::info(TAG, "Renderer shut down");
}

// ── Camera ─────────────────────────────────────────────────────────────────

void Renderer::update_camera(const platform::InputState& input, f32 dt) {
    m_camera.update(input, dt);
}

void Renderer::handle_resize(f32 aspect) {
    m_camera.set_aspect(aspect);
}

// Look up a clip index by name in a model's animations. Returns -1 if not found.
static i32 find_clip_by_name(const asset::ModelData& model, std::string_view name) {
    for (i32 i = 0; i < static_cast<i32>(model.animations.size()); ++i) {
        if (model.animations[i].name == name) return i;
    }
    return -1;
}

AnimationInstance& Renderer::get_or_create_anim(u32 entity_id, LoadedModel& model) {
    auto it = m_anim_instances.find(entity_id);
    if (it != m_anim_instances.end()) return it->second;

    AnimationInstance inst;
    inst.model = &model.data;

    // Bind animation states to clips by name (like WC3 model animations)
    inst.state_to_clip[static_cast<u8>(AnimState::Idle)]   = find_clip_by_name(model.data, "idle");
    inst.state_to_clip[static_cast<u8>(AnimState::Walk)]   = find_clip_by_name(model.data, "walk");
    inst.state_to_clip[static_cast<u8>(AnimState::Attack)] = find_clip_by_name(model.data, "attack");
    inst.state_to_clip[static_cast<u8>(AnimState::Spell)]  = find_clip_by_name(model.data, "spell");
    inst.state_to_clip[static_cast<u8>(AnimState::Death)]  = find_clip_by_name(model.data, "death");
    inst.state_to_clip[static_cast<u8>(AnimState::Birth)]  = find_clip_by_name(model.data, "birth");

    // Start with birth animation if available
    if (inst.state_to_clip[static_cast<u8>(AnimState::Birth)] >= 0) {
        inst.current_state = AnimState::Birth;
        inst.looping = false;
    }

    // Allocate per-entity bone buffer (persistently mapped)
    u32 bone_count = static_cast<u32>(model.data.skeleton.bones.size());
    if (bone_count > 0) {
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size  = bone_count * sizeof(glm::mat4);
        buf_ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo info{};
        vmaCreateBuffer(m_rhi->allocator(), &buf_ci, &alloc_ci,
                        &inst.bone_buffer, &inst.bone_alloc, &info);
        inst.bone_mapped = info.pMappedData;

        // Initialize to identity
        auto* bones = static_cast<glm::mat4*>(inst.bone_mapped);
        for (u32 i = 0; i < bone_count; ++i) bones[i] = glm::mat4{1.0f};

        // Allocate descriptor set
        inst.bone_descriptor = allocate_bone_descriptor(inst.bone_buffer, bone_count * sizeof(glm::mat4));
    }

    auto [inserted, _] = m_anim_instances.emplace(entity_id, std::move(inst));
    return inserted->second;
}

// Derive animation state and gameplay duration from simulation components.
struct AnimStateInfo {
    AnimState      state;
    f32            duration;       // 0 = use default clip speed
    bool           force_restart;  // true = restart animation (new attack swing)
    AttackAnimInfo attack_info;    // only used when state == Attack
    bool           has_attack_info = false;
};

static AnimStateInfo derive_anim_state(const simulation::World& world, u32 id,
                                        AnimationInstance& anim) {
    // Birth animation plays once then transitions to idle
    if (anim.current_state == AnimState::Birth && !anim.finished) {
        return {AnimState::Birth, 0, false};
    }

    if (world.dead_states.has(id)) return {AnimState::Death, 0.8f, false};

    // Look up type def once (used by spell, walk)
    const simulation::UnitTypeDef* type_def = nullptr;
    auto* hi = world.handle_infos.get(id);
    if (hi && world.types) type_def = world.types->get_unit_type(hi->type_id);

    // Spell casting — two-phase animation (ability seconds don't derive from fraction)
    auto* aset = world.ability_sets.get(id);
    if (aset && (aset->cast_state == simulation::CastState::CastPoint ||
                 aset->cast_state == simulation::CastState::Backswing)) {
        f32 cp = type_def ? type_def->cast_pt : 0.5f;
        AttackAnimInfo info;
        info.dmg_point  = cp;
        info.cast_point = aset->cast_point_secs;
        info.backswing  = aset->cast_backswing_secs;
        f32 dur = aset->cast_point_secs + aset->cast_backswing_secs;
        return {AnimState::Spell, dur, false, info, true};
    }

    auto* combat = world.combats.get(id);
    if (combat) {
        using simulation::AttackState;

        // Attack animation: two-phase speed (wind-up + backswing).
        // Play during WindUp + Backswing + Cooldown (holds last frame during Cooldown).
        if (combat->attack_state == AttackState::WindUp ||
            combat->attack_state == AttackState::Backswing ||
            combat->attack_state == AttackState::Cooldown) {

            // Detect new attack swing
            bool new_swing = false;
            if (combat->attack_state == AttackState::WindUp) {
                if (combat->attack_timer > combat->dmg_time * 0.8f) {
                    u32 swing_id = static_cast<u32>(combat->attack_timer * 1000);
                    if (swing_id != anim.attack_swing_id) {
                        anim.attack_swing_id = swing_id;
                        new_swing = true;
                    }
                }
            }

            AttackAnimInfo info;
            info.dmg_point  = combat->dmg_pt;
            info.cast_point = combat->dmg_time;
            info.backswing  = combat->backsw_time;
            f32 dur = combat->dmg_time + combat->backsw_time;
            return {AnimState::Attack, dur, new_swing, info, true};
        }

        if (combat->attack_state == AttackState::MovingToTarget) {
            auto* mov = world.movements.get(id);
            f32 ref = type_def ? type_def->walk_speed : 0;
            if (ref <= 0 && mov) ref = mov->speed;
            f32 ratio = (mov && mov->speed > 0 && ref > 0) ? mov->speed / ref : 1.0f;
            return {AnimState::Walk, -ratio, false};
        }
    }

    auto* mov = world.movements.get(id);
    if (mov && mov->moving) {
        f32 ref = type_def ? type_def->walk_speed : 0;
        if (ref <= 0) ref = mov->speed;
        f32 ratio = (mov->speed > 0 && ref > 0) ? mov->speed / ref : 1.0f;
        return {AnimState::Walk, -ratio, false};
    }

    return {AnimState::Idle, 0, false};
}

void Renderer::set_terrain(const map::TerrainData& terrain) {
    VmaAllocator alloc = m_rhi->allocator();
    destroy_terrain_mesh(alloc, m_terrain);
    m_terrain = build_terrain_mesh(alloc, terrain);

    // Splatmap weights are baked into terrain vertex attributes — no splatmap texture needed.
    // Re-allocate terrain descriptor set (layer textures only).
    if (terrain.is_valid()) {
        m_terrain_material.descriptor_set = allocate_terrain_descriptor(m_terrain_material);
    }
}

// ── Fog of war texture ────────────────────────────────────────────────────

void Renderer::set_fog_grid(const f32* values, u32 tiles_x, u32 tiles_y) {
    if (!values || tiles_x == 0 || tiles_y == 0) {
        m_fog_enabled = false;
        return;
    }

    VkDevice device = m_rhi->device();
    VmaAllocator allocator = m_rhi->allocator();

    // (Re)create fog texture if dimensions changed
    if (tiles_x != m_fog_width || tiles_y != m_fog_height) {
        // Destroy old resources
        if (m_fog_texture.image) destroy_texture(*m_rhi, m_fog_texture);
        if (m_fog_staging_buffer) {
            if (m_fog_staging_mapped) vmaUnmapMemory(allocator, m_fog_staging_alloc);
            vmaDestroyBuffer(allocator, m_fog_staging_buffer, m_fog_staging_alloc);
            m_fog_staging_buffer = VK_NULL_HANDLE;
            m_fog_staging_mapped = nullptr;
        }

        m_fog_width = tiles_x;
        m_fog_height = tiles_y;

        // Create persistent staging buffer (mapped once)
        VkDeviceSize buf_size = static_cast<VkDeviceSize>(tiles_x) * tiles_y * 4; // RGBA
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size  = buf_size;
        buf_ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                         VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo alloc_info{};
        vmaCreateBuffer(allocator, &buf_ci, &alloc_ci,
                        &m_fog_staging_buffer, &m_fog_staging_alloc, &alloc_info);
        m_fog_staging_mapped = alloc_info.pMappedData;

        // Create GPU image (R8G8B8A8_UNORM — we expand R8 to RGBA for compatibility)
        VkImageCreateInfo img_ci{};
        img_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.imageType     = VK_IMAGE_TYPE_2D;
        img_ci.format        = VK_FORMAT_R8G8B8A8_UNORM;
        img_ci.extent        = {tiles_x, tiles_y, 1};
        img_ci.mipLevels     = 1;
        img_ci.arrayLayers   = 1;
        img_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo img_alloc_ci{};
        img_alloc_ci.usage = VMA_MEMORY_USAGE_AUTO;

        m_fog_texture = {};
        m_fog_texture.width = tiles_x;
        m_fog_texture.height = tiles_y;
        vmaCreateImage(allocator, &img_ci, &img_alloc_ci,
                       &m_fog_texture.image, &m_fog_texture.alloc, nullptr);

        // Image view
        VkImageViewCreateInfo view_ci{};
        view_ci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image    = m_fog_texture.image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format   = VK_FORMAT_R8G8B8A8_UNORM;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device, &view_ci, nullptr, &m_fog_texture.view);

        // Sampler (bilinear, clamp to edge for smooth fog borders)
        VkSamplerCreateInfo sampler_ci{};
        sampler_ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_ci.magFilter    = VK_FILTER_LINEAR;
        sampler_ci.minFilter    = VK_FILTER_LINEAR;
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_ci.maxLod       = 0.0f;
        vkCreateSampler(device, &sampler_ci, nullptr, &m_fog_texture.sampler);

        // Initial transition to SHADER_READ_ONLY (will transition to TRANSFER_DST each frame)
        VkCommandBuffer cmd = m_rhi->begin_oneshot();
        VkImageMemoryBarrier2 barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image         = m_fog_texture.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dep{};
        dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers    = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
        m_rhi->end_oneshot(cmd);

        // Re-create terrain descriptor set with fog texture bound
        if (m_terrain_material.layer_count > 0) {
            m_terrain_material.descriptor_set = allocate_terrain_descriptor(m_terrain_material);
        }
    }

    // Convert float brightness → RGBA staging buffer
    if (m_fog_staging_mapped) {
        auto* dst = static_cast<u8*>(m_fog_staging_mapped);
        u32 count = tiles_x * tiles_y;
        for (u32 i = 0; i < count; ++i) {
            u8 brightness = static_cast<u8>(std::clamp(values[i], 0.0f, 1.0f) * 255.0f);
            dst[i * 4]     = brightness;
            dst[i * 4 + 1] = brightness;
            dst[i * 4 + 2] = brightness;
            dst[i * 4 + 3] = 255;
        }
    }

    m_fog_enabled = true;
    m_fog_dirty = true;
}

void Renderer::upload_fog(VkCommandBuffer cmd) {
    if (!m_fog_dirty || !m_fog_texture.image || !m_fog_staging_buffer) return;

    // Transition SHADER_READ_ONLY → TRANSFER_DST
    VkImageMemoryBarrier2 to_transfer{};
    to_transfer.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_transfer.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_transfer.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_transfer.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_transfer.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.image         = m_fog_texture.image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &to_transfer;
    vkCmdPipelineBarrier2(cmd, &dep);

    // Copy staging → image
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {m_fog_width, m_fog_height, 1};
    vkCmdCopyBufferToImage(cmd, m_fog_staging_buffer, m_fog_texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST → SHADER_READ_ONLY
    VkImageMemoryBarrier2 to_shader{};
    to_shader.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_shader.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_shader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_shader.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_shader.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_shader.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.image         = m_fog_texture.image;
    to_shader.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    dep.pImageMemoryBarriers = &to_shader;
    vkCmdPipelineBarrier2(cmd, &dep);

    m_fog_dirty = false;
}

bool Renderer::is_fog_hidden(const simulation::World& world, u32 id, const simulation::Transform& t) const {
    if (!m_fog_enabled || !m_simulation) return false;
    const auto& fog = m_simulation->fog();
    if (!fog.enabled()) return false;

    // Friendly units are always visible
    const auto* owner = world.owners.get(id);
    if (owner && owner->player.id == m_local_player_id) return false;
    if (owner && m_simulation->is_allied(simulation::Player{m_local_player_id}, owner->player)) return false;

    // Check fog state at entity tile
    auto tile = m_terrain_data
        ? m_terrain_data->world_to_tile(t.position.x, t.position.y)
        : glm::ivec2{0, 0};
    auto vis = fog.get(simulation::Player{m_local_player_id},
                       static_cast<u32>(tile.x), static_cast<u32>(tile.y));
    return vis != simulation::Visibility::Visible;
}

// ── Descriptor set layouts + pool ─────────────────────────────────────────

bool Renderer::create_descriptor_layouts() {
    VkDevice device = m_rhi->device();

    // Mesh descriptor set layout: 1 combined image sampler at binding 0
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 1;
        ci.pBindings    = &binding;

        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_mesh_desc_layout) != VK_SUCCESS) {
            log::error(TAG, "Failed to create mesh descriptor set layout");
            return false;
        }
    }

    // Terrain descriptor set layout: 4 ground layers + 1 fog texture = 5 samplers
    {
        VkDescriptorSetLayoutBinding bindings[5]{};
        for (u32 i = 0; i < 5; ++i) {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 5;
        ci.pBindings    = bindings;

        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_terrain_desc_layout) != VK_SUCCESS) {
            log::error(TAG, "Failed to create terrain descriptor set layout");
            return false;
        }
    }

    // Shadow descriptor set layout: UBO (binding 0) + shadow map sampler (binding 1)
    {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 2;
        ci.pBindings    = bindings;

        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_shadow_desc_layout) != VK_SUCCESS) {
            log::error(TAG, "Failed to create shadow descriptor set layout");
            return false;
        }
    }

    // Bone SSBO descriptor set layout (set 2 for skinned mesh, set 0 for skinned shadow)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 1;
        ci.pBindings    = &binding;

        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &m_bone_desc_layout) != VK_SUCCESS) {
            log::error(TAG, "Failed to create bone descriptor set layout");
            return false;
        }
    }

    // Create initial descriptor pool
    if (!allocate_or_grow_pool()) {
        log::error(TAG, "Failed to create descriptor pool");
        return false;
    }

    log::info(TAG, "Descriptor layouts and pool created");
    return true;
}

VkDescriptorPool Renderer::allocate_or_grow_pool() {
    VkDevice device = m_rhi->device();

    VkDescriptorPoolSize pool_sizes[3]{};
    pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = 64;
    pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = 8;
    pool_sizes[2].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[2].descriptorCount = 256;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_ci.maxSets       = 300;
    pool_ci.poolSizeCount = 3;
    pool_ci.pPoolSizes    = pool_sizes;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool) != VK_SUCCESS) {
        log::error(TAG, "Failed to create descriptor pool");
        return VK_NULL_HANDLE;
    }

    m_descriptor_pools.push_back(pool);
    log::info(TAG, "Descriptor pool #{} created (300 sets)", m_descriptor_pools.size());
    return pool;
}

VkDescriptorSet Renderer::allocate_mesh_descriptor(const GpuTexture& diffuse) {
    VkDevice device = m_rhi->device();

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool     = m_descriptor_pools.back();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts        = &m_mesh_desc_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) {
        // Pool full — grow and retry
        if (!allocate_or_grow_pool()) return VK_NULL_HANDLE;
        alloc_info.descriptorPool = m_descriptor_pools.back();
        if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo img_info{};
    img_info.sampler     = diffuse.sampler;
    img_info.imageView   = diffuse.view;
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &img_info;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return set;
}

VkDescriptorSet Renderer::allocate_terrain_descriptor(const TerrainMaterial& mat) {
    VkDevice device = m_rhi->device();

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool     = m_descriptor_pools.back();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts        = &m_terrain_desc_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) {
        if (!allocate_or_grow_pool()) return VK_NULL_HANDLE;
        alloc_info.descriptorPool = m_descriptor_pools.back();
        if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo img_infos[5]{};
    VkWriteDescriptorSet writes[5]{};

    // Layer textures (bindings 0-3)
    for (u32 i = 0; i < 4; ++i) {
        const GpuTexture& tex = (i < mat.layer_count) ? mat.layers[i] : m_default_texture;
        img_infos[i].sampler     = tex.sampler;
        img_infos[i].imageView   = tex.view;
        img_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = set;
        writes[i].dstBinding      = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo      = &img_infos[i];
    }

    // Fog texture (binding 4) — use fog texture if available, otherwise default (fully lit)
    const GpuTexture& fog_tex = m_fog_texture.image ? m_fog_texture : m_default_texture;
    img_infos[4].sampler     = fog_tex.sampler;
    img_infos[4].imageView   = fog_tex.view;
    img_infos[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    writes[4].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet          = set;
    writes[4].dstBinding      = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].pImageInfo      = &img_infos[4];

    vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
    return set;
}

// ── Mega buffer upload (Phase 14b) ───────────────────────────────────────

GpuMesh Renderer::upload_to_mega(const asset::MeshData& mesh) {
    GpuMesh gpu{};
    if (mesh.vertices.empty()) return gpu;

    u32 vc = static_cast<u32>(mesh.vertices.size());
    u32 ic = static_cast<u32>(mesh.indices.size());

    if (m_mega_vb_used + vc > MEGA_MAX_VERTICES) {
        log::error(TAG, "Mega vertex buffer full ({}/{})", m_mega_vb_used, MEGA_MAX_VERTICES);
        return gpu;
    }
    if (m_mega_ib_used + ic > MEGA_MAX_INDICES) {
        log::error(TAG, "Mega index buffer full ({}/{})", m_mega_ib_used, MEGA_MAX_INDICES);
        return gpu;
    }

    gpu.vertex_buffer = m_mega_vb;
    gpu.index_buffer  = m_mega_ib;
    gpu.vertex_alloc  = VK_NULL_HANDLE;  // not individually allocated
    gpu.index_alloc   = VK_NULL_HANDLE;
    gpu.vertex_count  = vc;
    gpu.index_count   = ic;
    gpu.first_vertex  = m_mega_vb_used;
    gpu.first_index   = m_mega_ib_used;

    // Compute bounding sphere radius (max distance from origin in model space)
    f32 max_r2 = 0.0f;
    for (const auto& v : mesh.vertices) {
        f32 r2 = glm::dot(v.position, v.position);
        if (r2 > max_r2) max_r2 = r2;
    }
    gpu.bounding_radius = std::sqrt(max_r2);

    auto* vb_dst = static_cast<u8*>(m_mega_vb_mapped) + m_mega_vb_used * sizeof(asset::Vertex);
    std::memcpy(vb_dst, mesh.vertices.data(), vc * sizeof(asset::Vertex));
    m_mega_vb_used += vc;

    auto* ib_dst = static_cast<u8*>(m_mega_ib_mapped) + m_mega_ib_used * sizeof(u32);
    std::memcpy(ib_dst, mesh.indices.data(), ic * sizeof(u32));
    m_mega_ib_used += ic;

    return gpu;
}

// ── Bindless texture array (Phase 14b) ───────────────────────────────────

bool Renderer::create_bindless_resources() {
    VkDevice device = m_rhi->device();

    // Descriptor set layout: variable-size sampler2D array
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = MAX_BINDLESS_TEXTURES;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorBindingFlags bindless_flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{};
    flags_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_ci.bindingCount  = 1;
    flags_ci.pBindingFlags = &bindless_flags;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext        = &flags_ci;
    layout_ci.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount = 1;
    layout_ci.pBindings    = &binding;

    if (vkCreateDescriptorSetLayout(device, &layout_ci, nullptr, &m_bindless_layout) != VK_SUCCESS) {
        log::error(TAG, "Failed to create bindless descriptor set layout");
        return false;
    }

    // Dedicated pool for the one bindless set
    VkDescriptorPoolSize pool_size{};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = MAX_BINDLESS_TEXTURES;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets       = 1;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes    = &pool_size;

    if (vkCreateDescriptorPool(device, &pool_ci, nullptr, &m_bindless_pool) != VK_SUCCESS) {
        log::error(TAG, "Failed to create bindless descriptor pool");
        return false;
    }

    // Allocate the bindless set with variable descriptor count
    u32 variable_count = MAX_BINDLESS_TEXTURES;
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_ci{};
    variable_ci.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    variable_ci.descriptorSetCount = 1;
    variable_ci.pDescriptorCounts  = &variable_count;

    VkDescriptorSetAllocateInfo alloc_ci{};
    alloc_ci.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_ci.pNext              = &variable_ci;
    alloc_ci.descriptorPool     = m_bindless_pool;
    alloc_ci.descriptorSetCount = 1;
    alloc_ci.pSetLayouts        = &m_bindless_layout;

    if (vkAllocateDescriptorSets(device, &alloc_ci, &m_bindless_set) != VK_SUCCESS) {
        log::error(TAG, "Failed to allocate bindless descriptor set");
        return false;
    }

    log::info(TAG, "Bindless texture array created (max {} textures)", MAX_BINDLESS_TEXTURES);
    return true;
}

u32 Renderer::register_bindless_texture(const GpuTexture& tex) {
    if (m_bindless_count >= MAX_BINDLESS_TEXTURES) {
        log::error(TAG, "Bindless texture array full ({}/{})", m_bindless_count, MAX_BINDLESS_TEXTURES);
        return 0;  // fallback to default texture
    }

    u32 idx = m_bindless_count++;

    VkDescriptorImageInfo img_info{};
    img_info.sampler     = tex.sampler;
    img_info.imageView   = tex.view;
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = m_bindless_set;
    write.dstBinding      = 0;
    write.dstArrayElement = idx;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo      = &img_info;

    vkUpdateDescriptorSets(m_rhi->device(), 1, &write, 0, nullptr);
    return idx;
}

// ── Default + terrain textures ────────────────────────────────────────────

bool Renderer::create_default_texture() {
    // 4x4 warm orange texture for placeholder meshes
    auto pixels = generate_solid_texture(4, 220, 160, 80);
    m_default_texture = upload_texture_rgba(*m_rhi, pixels.data(), 4, 4);
    if (!m_default_texture.image) return false;

    m_default_material.diffuse = m_default_texture;
    m_default_material.descriptor_set = allocate_mesh_descriptor(m_default_texture);

    // Register in bindless array
    m_default_tex_idx = register_bindless_texture(m_default_texture);

    // Corpse texture (dark gray)
    auto corpse_pixels = generate_solid_texture(4, 50, 50, 50);
    m_corpse_texture = upload_texture_rgba(*m_rhi, corpse_pixels.data(), 4, 4);
    if (!m_corpse_texture.image) return false;
    m_corpse_material.diffuse = m_corpse_texture;
    m_corpse_material.descriptor_set = allocate_mesh_descriptor(m_corpse_texture);

    // Register in bindless array
    m_corpse_tex_idx = register_bindless_texture(m_corpse_texture);

    log::info(TAG, "Default + corpse textures created (bindless idx: {}, {})",
              m_default_tex_idx, m_corpse_tex_idx);
    return true;
}

bool Renderer::create_terrain_textures() {
    // Generate procedural ground textures (32x32 each)
    constexpr u32 TEX_SIZE = 32;

    // Layer 0: grass (green)
    auto grass = generate_solid_texture(TEX_SIZE, 60, 140, 40);
    m_terrain_material.layers[0] = upload_texture_rgba(*m_rhi, grass.data(), TEX_SIZE, TEX_SIZE);

    // Layer 1: dirt (brown)
    auto dirt = generate_solid_texture(TEX_SIZE, 140, 100, 50);
    m_terrain_material.layers[1] = upload_texture_rgba(*m_rhi, dirt.data(), TEX_SIZE, TEX_SIZE);

    // Layer 2: stone (gray)
    auto stone = generate_solid_texture(TEX_SIZE, 130, 130, 120);
    m_terrain_material.layers[2] = upload_texture_rgba(*m_rhi, stone.data(), TEX_SIZE, TEX_SIZE);

    // Layer 3: water (blue) — also used for water surface quads
    auto sand = generate_solid_texture(TEX_SIZE, 30, 70, 150);
    m_terrain_material.layers[3] = upload_texture_rgba(*m_rhi, sand.data(), TEX_SIZE, TEX_SIZE);

    m_terrain_material.layer_count = 4;

    log::info(TAG, "Terrain textures created (4 layers)");
    return true;
}

// ── Pipeline creation helpers ─────────────────────────────────────────────

// Shared pipeline state (vertex input, rasterizer, depth, blend, dynamic)
struct PipelineStateConfig {
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    VkPipelineInputAssemblyStateCreateInfo input_assembly;
    VkPipelineViewportStateCreateInfo viewport_state;
    VkPipelineRasterizationStateCreateInfo rasterizer;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineDepthStencilStateCreateInfo depth_stencil;
    VkPipelineColorBlendAttachmentState blend_attachment;
    VkPipelineColorBlendStateCreateInfo color_blend;
    VkPipelineDynamicStateCreateInfo dynamic_state;
    VkDynamicState dynamic_states[2];
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attrs[5];
};

static PipelineStateConfig make_common_pipeline_state() {
    PipelineStateConfig cfg{};

    cfg.binding.binding   = 0;
    cfg.binding.stride    = sizeof(asset::Vertex);
    cfg.binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    cfg.attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, position)};
    cfg.attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(asset::Vertex, normal)};
    cfg.attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(asset::Vertex, texcoord)};

    cfg.vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    cfg.vertex_input.vertexBindingDescriptionCount   = 1;
    cfg.vertex_input.pVertexBindingDescriptions      = &cfg.binding;
    cfg.vertex_input.vertexAttributeDescriptionCount = 3;
    cfg.vertex_input.pVertexAttributeDescriptions    = cfg.attrs;

    cfg.input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    cfg.input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    cfg.viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    cfg.viewport_state.viewportCount = 1;
    cfg.viewport_state.scissorCount  = 1;

    cfg.rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    cfg.rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    cfg.rasterizer.cullMode    = VK_CULL_MODE_BACK_BIT;
    cfg.rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    cfg.rasterizer.lineWidth   = 1.0f;

    cfg.multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    cfg.multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    cfg.depth_stencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    cfg.depth_stencil.depthTestEnable  = VK_TRUE;
    cfg.depth_stencil.depthWriteEnable = VK_TRUE;
    cfg.depth_stencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    cfg.blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    cfg.color_blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cfg.color_blend.attachmentCount = 1;
    cfg.color_blend.pAttachments    = &cfg.blend_attachment;

    cfg.dynamic_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
    cfg.dynamic_states[1] = VK_DYNAMIC_STATE_SCISSOR;
    cfg.dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    cfg.dynamic_state.dynamicStateCount = 2;
    cfg.dynamic_state.pDynamicStates    = cfg.dynamic_states;

    return cfg;
}

bool Renderer::create_mesh_pipeline() {
    VkDevice device = m_rhi->device();

    VkShaderModule vert = load_shader(device, "engine/shaders/mesh.vert.spv");
    VkShaderModule frag = load_shader(device, "engine/shaders/mesh.frag.spv");
    if (!vert || !frag) {
        log::error(TAG, "Failed to load mesh shaders");
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    auto cfg = make_common_pipeline_state();

    cfg.stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    cfg.stages[0].module = vert;
    cfg.stages[0].pName  = "main";
    cfg.stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    cfg.stages[1].module = frag;
    cfg.stages[1].pName  = "main";

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset     = 0;
    push_range.size       = sizeof(glm::mat4);  // just vp

    // Set 0 = bindless textures, set 1 = shadow, set 2 = instance SSBO (Phase 14b)
    VkDescriptorSetLayout mesh_layouts[] = {m_bindless_layout, m_shadow_desc_layout, m_bone_desc_layout};
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount         = 3;
    layout_ci.pSetLayouts            = mesh_layouts;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &m_mesh_pipeline_layout) != VK_SUCCESS) {
        log::error(TAG, "Failed to create mesh pipeline layout");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkFormat color_format = m_rhi->swapchain_format();
    VkFormat depth_format = m_rhi->depth_format();
    VkPipelineRenderingCreateInfo rendering_ci{};
    rendering_ci.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.colorAttachmentCount    = 1;
    rendering_ci.pColorAttachmentFormats = &color_format;
    rendering_ci.depthAttachmentFormat   = depth_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext               = &rendering_ci;
    pipeline_ci.stageCount          = 2;
    pipeline_ci.pStages             = cfg.stages;
    pipeline_ci.pVertexInputState   = &cfg.vertex_input;
    pipeline_ci.pInputAssemblyState = &cfg.input_assembly;
    pipeline_ci.pViewportState      = &cfg.viewport_state;
    pipeline_ci.pRasterizationState = &cfg.rasterizer;
    pipeline_ci.pMultisampleState   = &cfg.multisample;
    pipeline_ci.pDepthStencilState  = &cfg.depth_stencil;
    pipeline_ci.pColorBlendState    = &cfg.color_blend;
    pipeline_ci.pDynamicState       = &cfg.dynamic_state;
    pipeline_ci.layout              = m_mesh_pipeline_layout;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &m_mesh_pipeline) != VK_SUCCESS) {
        log::error(TAG, "Failed to create mesh pipeline");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    log::info(TAG, "Mesh pipeline created (textured + shadow)");
    return true;
}

bool Renderer::create_skinned_mesh_pipeline() {
    VkDevice device = m_rhi->device();

    VkShaderModule vert = load_shader(device, "engine/shaders/skinned_mesh.vert.spv");
    VkShaderModule frag = load_shader(device, "engine/shaders/skinned_mesh.frag.spv");
    if (!vert || !frag) {
        log::error(TAG, "Failed to load skinned mesh shaders");
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    // Skinned vertex input: 5 attributes, 64-byte stride
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(asset::SkinnedVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[5]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(asset::SkinnedVertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(asset::SkinnedVertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(asset::SkinnedVertex, texcoord)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_UINT,   offsetof(asset::SkinnedVertex, bone_indices)};
    attrs[4] = {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(asset::SkinnedVertex, bone_weights)};

    auto cfg = make_common_pipeline_state();
    // Override vertex input with skinned format
    cfg.vertex_input.pVertexBindingDescriptions    = &binding;
    cfg.vertex_input.vertexAttributeDescriptionCount = 5;
    cfg.vertex_input.pVertexAttributeDescriptions  = attrs;
    cfg.stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    cfg.stages[0].module = vert;
    cfg.stages[0].pName  = "main";
    cfg.stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    cfg.stages[1].module = frag;
    cfg.stages[1].pName  = "main";

    // Layout: set 0 = material, set 1 = shadow, set 2 = bones SSBO
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset     = 0;
    push_range.size       = 2 * sizeof(glm::mat4);

    VkDescriptorSetLayout layouts[] = {m_mesh_desc_layout, m_shadow_desc_layout, m_bone_desc_layout};
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount         = 3;
    layout_ci.pSetLayouts            = layouts;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &m_skinned_mesh_pipeline_layout) != VK_SUCCESS) {
        log::error(TAG, "Failed to create skinned mesh pipeline layout");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkFormat color_format = m_rhi->swapchain_format();
    VkFormat depth_format = m_rhi->depth_format();
    VkPipelineRenderingCreateInfo rendering_ci{};
    rendering_ci.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.colorAttachmentCount    = 1;
    rendering_ci.pColorAttachmentFormats = &color_format;
    rendering_ci.depthAttachmentFormat   = depth_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext               = &rendering_ci;
    pipeline_ci.stageCount          = 2;
    pipeline_ci.pStages             = cfg.stages;
    pipeline_ci.pVertexInputState   = &cfg.vertex_input;
    pipeline_ci.pInputAssemblyState = &cfg.input_assembly;
    pipeline_ci.pViewportState      = &cfg.viewport_state;
    pipeline_ci.pRasterizationState = &cfg.rasterizer;
    pipeline_ci.pMultisampleState   = &cfg.multisample;
    pipeline_ci.pDepthStencilState  = &cfg.depth_stencil;
    pipeline_ci.pColorBlendState    = &cfg.color_blend;
    pipeline_ci.pDynamicState       = &cfg.dynamic_state;
    pipeline_ci.layout              = m_skinned_mesh_pipeline_layout;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &m_skinned_mesh_pipeline) != VK_SUCCESS) {
        log::error(TAG, "Failed to create skinned mesh pipeline");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);

    // Skinned shadow pipeline
    VkShaderModule shadow_vert = load_shader(device, "engine/shaders/skinned_shadow.vert.spv");
    if (shadow_vert) {
        // Shadow layout: set 0 = bones SSBO, push constant = light_mvp
        VkPushConstantRange shadow_push{};
        shadow_push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        shadow_push.size       = sizeof(glm::mat4);

        VkPipelineLayoutCreateInfo shadow_layout_ci{};
        shadow_layout_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        shadow_layout_ci.setLayoutCount         = 1;
        shadow_layout_ci.pSetLayouts            = &m_bone_desc_layout;
        shadow_layout_ci.pushConstantRangeCount = 1;
        shadow_layout_ci.pPushConstantRanges    = &shadow_push;

        vkCreatePipelineLayout(device, &shadow_layout_ci, nullptr, &m_skinned_shadow_pipeline_layout);

        VkPipelineShaderStageCreateInfo shadow_stage{};
        shadow_stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shadow_stage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
        shadow_stage.module = shadow_vert;
        shadow_stage.pName  = "main";

        // Shadow: depth-only, no color
        VkPipelineRenderingCreateInfo shadow_rendering{};
        shadow_rendering.sType                = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        shadow_rendering.depthAttachmentFormat = m_rhi->depth_format();

        cfg.vertex_input.pVertexBindingDescriptions    = &binding;
        cfg.vertex_input.vertexAttributeDescriptionCount = 5;
        cfg.vertex_input.pVertexAttributeDescriptions  = attrs;
        cfg.rasterizer.depthBiasEnable    = VK_TRUE;
        cfg.rasterizer.depthBiasConstantFactor = 1.25f;
        cfg.rasterizer.depthBiasSlopeFactor    = 1.75f;
        cfg.color_blend.attachmentCount = 0;

        VkGraphicsPipelineCreateInfo shadow_ci{};
        shadow_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        shadow_ci.pNext               = &shadow_rendering;
        shadow_ci.stageCount          = 1;
        shadow_ci.pStages             = &shadow_stage;
        shadow_ci.pVertexInputState   = &cfg.vertex_input;
        shadow_ci.pInputAssemblyState = &cfg.input_assembly;
        shadow_ci.pViewportState      = &cfg.viewport_state;
        shadow_ci.pRasterizationState = &cfg.rasterizer;
        shadow_ci.pMultisampleState   = &cfg.multisample;
        shadow_ci.pDepthStencilState  = &cfg.depth_stencil;
        shadow_ci.pColorBlendState    = &cfg.color_blend;
        shadow_ci.pDynamicState       = &cfg.dynamic_state;
        shadow_ci.layout              = m_skinned_shadow_pipeline_layout;

        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &shadow_ci, nullptr, &m_skinned_shadow_pipeline);
        vkDestroyShaderModule(device, shadow_vert, nullptr);
    }

    log::info(TAG, "Skinned mesh pipeline created (textured + shadow + bone SSBO)");
    return true;
}

bool Renderer::create_particle_pipeline() {
    VkDevice device = m_rhi->device();

    VkShaderModule vert = load_shader(device, "engine/shaders/particle.vert.spv");
    VkShaderModule frag = load_shader(device, "engine/shaders/particle.frag.spv");
    if (!vert || !frag) {
        log::error(TAG, "Failed to load particle shaders");
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    // Vertex input: position(vec3) + color(vec4) + texcoord(vec2)
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(ParticleVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(ParticleVertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,  offsetof(ParticleVertex, color)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,        offsetof(ParticleVertex, texcoord)};

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode    = VK_CULL_MODE_NONE;  // particles are double-sided
    rasterizer.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable  = VK_TRUE;
    depth_stencil.depthWriteEnable = VK_FALSE;  // don't write depth — particles are transparent
    depth_stencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    // Alpha blending: src_alpha * src + (1 - src_alpha) * dst
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.blendEnable         = VK_TRUE;
    blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.colorBlendOp        = VK_BLEND_OP_ADD;
    blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_att.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend_att.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments    = &blend_att;

    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates    = dynamic_states;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    // Push constant: just the VP matrix
    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.size       = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &m_particle_pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkFormat color_format = m_rhi->swapchain_format();
    VkFormat depth_format = m_rhi->depth_format();
    VkPipelineRenderingCreateInfo rendering_ci{};
    rendering_ci.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.colorAttachmentCount    = 1;
    rendering_ci.pColorAttachmentFormats = &color_format;
    rendering_ci.depthAttachmentFormat   = depth_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext               = &rendering_ci;
    pipeline_ci.stageCount          = 2;
    pipeline_ci.pStages             = stages;
    pipeline_ci.pVertexInputState   = &vertex_input;
    pipeline_ci.pInputAssemblyState = &input_assembly;
    pipeline_ci.pViewportState      = &viewport_state;
    pipeline_ci.pRasterizationState = &rasterizer;
    pipeline_ci.pMultisampleState   = &multisample;
    pipeline_ci.pDepthStencilState  = &depth_stencil;
    pipeline_ci.pColorBlendState    = &color_blend;
    pipeline_ci.pDynamicState       = &dynamic_state;
    pipeline_ci.layout              = m_particle_pipeline_layout;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &m_particle_pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    log::info(TAG, "Particle pipeline created (alpha-blended)");
    return true;
}

bool Renderer::create_terrain_pipeline() {
    VkDevice device = m_rhi->device();

    VkShaderModule vert = load_shader(device, "engine/shaders/terrain.vert.spv");
    VkShaderModule frag = load_shader(device, "engine/shaders/terrain.frag.spv");
    if (!vert || !frag) {
        log::error(TAG, "Failed to load terrain shaders");
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkVertexInputBindingDescription terrain_binding{};
    terrain_binding.binding   = 0;
    terrain_binding.stride    = sizeof(TerrainVertex);
    terrain_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription terrain_attrs[4]{};
    terrain_attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(TerrainVertex, position)};
    terrain_attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(TerrainVertex, normal)};
    terrain_attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(TerrainVertex, texcoord)};
    terrain_attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainVertex, splat_weights)};

    auto cfg = make_common_pipeline_state();
    cfg.vertex_input.pVertexBindingDescriptions      = &terrain_binding;
    cfg.vertex_input.vertexAttributeDescriptionCount  = 4;
    cfg.vertex_input.pVertexAttributeDescriptions     = terrain_attrs;

    cfg.stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    cfg.stages[0].module = vert;
    cfg.stages[0].pName  = "main";
    cfg.stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    cfg.stages[1].module = frag;
    cfg.stages[1].pName  = "main";

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_range.offset     = 0;
    push_range.size       = 2 * sizeof(glm::mat4) + sizeof(glm::vec2) + 2 * sizeof(f32);  // mvp + model + world_size + fog_enabled + pad

    VkDescriptorSetLayout terrain_layouts[] = {m_terrain_desc_layout, m_shadow_desc_layout};
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount         = 2;
    layout_ci.pSetLayouts            = terrain_layouts;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &m_terrain_pipeline_layout) != VK_SUCCESS) {
        log::error(TAG, "Failed to create terrain pipeline layout");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkFormat color_format = m_rhi->swapchain_format();
    VkFormat depth_format = m_rhi->depth_format();
    VkPipelineRenderingCreateInfo rendering_ci{};
    rendering_ci.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.colorAttachmentCount    = 1;
    rendering_ci.pColorAttachmentFormats = &color_format;
    rendering_ci.depthAttachmentFormat   = depth_format;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext               = &rendering_ci;
    pipeline_ci.stageCount          = 2;
    pipeline_ci.pStages             = cfg.stages;
    pipeline_ci.pVertexInputState   = &cfg.vertex_input;
    pipeline_ci.pInputAssemblyState = &cfg.input_assembly;
    pipeline_ci.pViewportState      = &cfg.viewport_state;
    pipeline_ci.pRasterizationState = &cfg.rasterizer;
    pipeline_ci.pMultisampleState   = &cfg.multisample;
    pipeline_ci.pDepthStencilState  = &cfg.depth_stencil;
    pipeline_ci.pColorBlendState    = &cfg.color_blend;
    pipeline_ci.pDynamicState       = &cfg.dynamic_state;
    pipeline_ci.layout              = m_terrain_pipeline_layout;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &m_terrain_pipeline) != VK_SUCCESS) {
        log::error(TAG, "Failed to create terrain pipeline");
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    log::info(TAG, "Terrain pipeline created (splatmap + shadow)");
    return true;
}

// ── Model loading + mesh cache ────────────────────────────────────────────

LoadedModel* Renderer::get_or_load_model(const std::string& model_path) {
    // Check cache first
    auto it = m_model_cache.find(model_path);
    if (it != m_model_cache.end()) return &it->second;

    // Don't retry paths that already failed
    if (m_model_failed.contains(model_path)) return nullptr;

    // Resolve path: try map assets, then engine root, then absolute
    namespace fs = std::filesystem;
    std::string resolved;

    if (!m_map_root.empty()) {
        std::string map_asset = m_map_root + "/shared/assets/" + model_path;
        if (fs::exists(map_asset)) resolved = map_asset;
    }
    if (resolved.empty() && fs::exists(model_path)) {
        resolved = model_path;
    }
    if (resolved.empty()) {
        log::warn(TAG, "Model not found: '{}'", model_path);
        m_model_failed.insert(model_path);
        return nullptr;
    }

    // Load model from file
    auto result = asset::load_model(resolved);
    if (!result) {
        log::error(TAG, "Failed to load model '{}': {}", model_path, result.error());
        m_model_failed.insert(model_path);
        return nullptr;
    }

    LoadedModel lm;
    lm.data = std::move(*result);

    // Merge all meshes into one and upload
    VmaAllocator alloc = m_rhi->allocator();
    if (!lm.data.skinned_meshes.empty() && lm.data.has_skeleton()) {
        // Merge all skinned mesh primitives into one
        asset::SkinnedMeshData merged;
        for (auto& sm : lm.data.skinned_meshes) {
            u32 base_vertex = static_cast<u32>(merged.vertices.size());
            merged.vertices.insert(merged.vertices.end(), sm.vertices.begin(), sm.vertices.end());
            for (u32 idx : sm.indices) {
                merged.indices.push_back(base_vertex + idx);
            }
        }
        u32 bone_count = static_cast<u32>(lm.data.skeleton.bones.size());
        lm.mesh = upload_skinned_mesh(alloc, merged, bone_count);
        lm.is_skinned = true;
        log::info(TAG, "Loaded skinned model '{}': {} meshes merged, {} verts, {} bones, {} anims",
                  model_path, lm.data.skinned_meshes.size(), lm.mesh.vertex_count,
                  bone_count, lm.data.animations.size());
    } else if (!lm.data.meshes.empty()) {
        // Merge all static mesh primitives into one → mega buffer (Phase 14b)
        asset::MeshData merged;
        for (auto& m : lm.data.meshes) {
            u32 base_vertex = static_cast<u32>(merged.vertices.size());
            merged.vertices.insert(merged.vertices.end(), m.vertices.begin(), m.vertices.end());
            for (u32 idx : m.indices) {
                merged.indices.push_back(base_vertex + idx);
            }
        }
        lm.mesh = upload_to_mega(merged);
        lm.is_skinned = false;
        log::info(TAG, "Loaded static model '{}': {} meshes merged, {} verts (mega @{}/{})",
                  model_path, lm.data.meshes.size(), lm.mesh.vertex_count,
                  lm.mesh.first_vertex, lm.mesh.first_index);
    } else {
        log::warn(TAG, "Model '{}' has no meshes", model_path);
        m_model_failed.insert(model_path);
        return nullptr;
    }

    // Upload diffuse texture (from model or use default)
    if (!lm.data.textures.empty()) {
        lm.diffuse_texture = upload_texture_rgba(*m_rhi,
            lm.data.textures[0].pixels.data(),
            lm.data.textures[0].width,
            lm.data.textures[0].height);
        lm.material.diffuse = lm.diffuse_texture;
        lm.material.descriptor_set = allocate_mesh_descriptor(lm.diffuse_texture);
        // Register in bindless array for static pipeline
        lm.texture_index = register_bindless_texture(lm.diffuse_texture);
        log::info(TAG, "  Texture: {}x{} (bindless idx: {})",
                  lm.data.textures[0].width, lm.data.textures[0].height, lm.texture_index);
    } else {
        lm.material = m_default_material;
        lm.texture_index = m_default_tex_idx;
    }

    auto [inserted, _] = m_model_cache.emplace(model_path, std::move(lm));
    return &inserted->second;
}

glm::vec3 Renderer::get_attachment_point(u32 entity_id, std::string_view bone_name) const {
    auto it = m_anim_instances.find(entity_id);
    if (it == m_anim_instances.end()) return {0, 0, 0};
    glm::vec3 local = render::get_attachment_point(it->second, bone_name);
    // Convert from glTF Y-up to game Z-up: (X, Y, Z) → (X, -Z, Y)
    return {local.x, -local.z, local.y};
}

GpuMesh& Renderer::get_or_upload_mesh(const std::string& model_path) {
    auto it = m_mesh_cache.find(model_path);
    if (it != m_mesh_cache.end()) return it->second;

    if (model_path == "projectile") {
        m_mesh_cache[model_path] = m_projectile_mesh;
    } else if (model_path == "placeholder") {
        m_mesh_cache[model_path] = m_placeholder_mesh;
        m_mesh_cache[model_path].native_z_up = true;
    } else {
        // Try loading as a real model first
        auto* lm = get_or_load_model(model_path);
        if (lm) {
            // Return the loaded model's mesh (caller uses mesh cache for non-unit entities)
            m_mesh_cache[model_path] = lm->mesh;
            return m_mesh_cache[model_path];
        }
        log::trace(TAG, "Using placeholder mesh for '{}'", model_path);
        m_mesh_cache[model_path] = m_placeholder_mesh;
    }
    return m_mesh_cache[model_path];
}

// ── Shadow pipeline + resources ────────────────────────────────────────────

bool Renderer::create_shadow_pipeline() {
    VkDevice device = m_rhi->device();

    VkShaderModule vert = load_shader(device, "engine/shaders/shadow.vert.spv");
    VkShaderModule frag = load_shader(device, "engine/shaders/shadow.frag.spv");
    if (!vert || !frag) {
        log::error(TAG, "Failed to load shadow shaders");
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    auto cfg = make_common_pipeline_state();
    cfg.stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    cfg.stages[0].module = vert;
    cfg.stages[0].pName  = "main";
    cfg.stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cfg.stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    cfg.stages[1].module = frag;
    cfg.stages[1].pName  = "main";

    cfg.rasterizer.depthBiasEnable         = VK_TRUE;
    cfg.rasterizer.depthBiasConstantFactor = 2.0f;
    cfg.rasterizer.depthBiasSlopeFactor    = 1.5f;

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_range.offset     = 0;
    push_range.size       = sizeof(glm::mat4);

    VkDescriptorSetLayout shadow_instance_layout[] = {m_bone_desc_layout};  // set 0 = instance SSBO
    VkPipelineLayoutCreateInfo layout_ci{};
    layout_ci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_ci.setLayoutCount         = 1;
    layout_ci.pSetLayouts            = shadow_instance_layout;
    layout_ci.pushConstantRangeCount = 1;
    layout_ci.pPushConstantRanges    = &push_range;

    if (vkCreatePipelineLayout(device, &layout_ci, nullptr, &m_shadow_pipeline_layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    VkPipelineRenderingCreateInfo rendering_ci{};
    rendering_ci.sType                 = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering_ci.depthAttachmentFormat = depth_format;

    cfg.color_blend.attachmentCount = 0;
    cfg.color_blend.pAttachments    = nullptr;

    VkGraphicsPipelineCreateInfo pipeline_ci{};
    pipeline_ci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_ci.pNext               = &rendering_ci;
    pipeline_ci.stageCount          = 2;
    pipeline_ci.pStages             = cfg.stages;
    pipeline_ci.pVertexInputState   = &cfg.vertex_input;
    pipeline_ci.pInputAssemblyState = &cfg.input_assembly;
    pipeline_ci.pViewportState      = &cfg.viewport_state;
    pipeline_ci.pRasterizationState = &cfg.rasterizer;
    pipeline_ci.pMultisampleState   = &cfg.multisample;
    pipeline_ci.pDepthStencilState  = &cfg.depth_stencil;
    pipeline_ci.pColorBlendState    = &cfg.color_blend;
    pipeline_ci.pDynamicState       = &cfg.dynamic_state;
    pipeline_ci.layout              = m_shadow_pipeline_layout;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_ci, nullptr, &m_shadow_pipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);

    // Terrain shadow pipeline: same as above but with TerrainVertex layout (48-byte stride)
    VkShaderModule terrain_vert = load_shader(device, "engine/shaders/terrain_shadow.vert.spv");
    if (terrain_vert) {
        VkVertexInputBindingDescription terrain_binding{};
        terrain_binding.binding   = 0;
        terrain_binding.stride    = sizeof(TerrainVertex);
        terrain_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription terrain_attrs[4]{};
        terrain_attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(TerrainVertex, position)};
        terrain_attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(TerrainVertex, normal)};
        terrain_attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(TerrainVertex, texcoord)};
        terrain_attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TerrainVertex, splat_weights)};

        auto tcfg = make_common_pipeline_state();
        tcfg.vertex_input.pVertexBindingDescriptions      = &terrain_binding;
        tcfg.vertex_input.vertexAttributeDescriptionCount  = 4;
        tcfg.vertex_input.pVertexAttributeDescriptions     = terrain_attrs;

        tcfg.stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        tcfg.stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        tcfg.stages[0].module = terrain_vert;
        tcfg.stages[0].pName  = "main";
        tcfg.stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        tcfg.stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        tcfg.stages[1].module = load_shader(device, "engine/shaders/shadow.frag.spv");
        tcfg.stages[1].pName  = "main";

        tcfg.rasterizer.depthBiasEnable         = VK_TRUE;
        tcfg.rasterizer.depthBiasConstantFactor = 2.0f;
        tcfg.rasterizer.depthBiasSlopeFactor    = 1.5f;
        tcfg.color_blend.attachmentCount = 0;
        tcfg.color_blend.pAttachments    = nullptr;

        // Reuse shadow pipeline layout (same push constant)
        m_terrain_shadow_pipeline_layout = m_shadow_pipeline_layout;

        VkGraphicsPipelineCreateInfo tpci{};
        tpci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        tpci.pNext               = &rendering_ci;
        tpci.stageCount          = 2;
        tpci.pStages             = tcfg.stages;
        tpci.pVertexInputState   = &tcfg.vertex_input;
        tpci.pInputAssemblyState = &tcfg.input_assembly;
        tpci.pViewportState      = &tcfg.viewport_state;
        tpci.pRasterizationState = &tcfg.rasterizer;
        tpci.pMultisampleState   = &tcfg.multisample;
        tpci.pDepthStencilState  = &tcfg.depth_stencil;
        tpci.pColorBlendState    = &tcfg.color_blend;
        tpci.pDynamicState       = &tcfg.dynamic_state;
        tpci.layout              = m_terrain_shadow_pipeline_layout;

        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &tpci, nullptr, &m_terrain_shadow_pipeline);
        if (tcfg.stages[1].module) vkDestroyShaderModule(device, tcfg.stages[1].module, nullptr);
        vkDestroyShaderModule(device, terrain_vert, nullptr);
    }

    log::info(TAG, "Shadow pipelines created (depth-only)");
    return true;
}

bool Renderer::create_shadow_resources() {
    if (!create_shadow_map(*m_rhi, m_shadow_map)) return false;
    if (!create_shadow_buffer(*m_rhi, m_shadow_ubo)) return false;
    m_shadow_desc_set = allocate_shadow_descriptor();
    return m_shadow_desc_set != VK_NULL_HANDLE;
}

VkDescriptorSet Renderer::allocate_shadow_descriptor() {
    VkDevice device = m_rhi->device();

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool     = m_descriptor_pools.back();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts        = &m_shadow_desc_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) {
        if (!allocate_or_grow_pool()) return VK_NULL_HANDLE;
        alloc_info.descriptorPool = m_descriptor_pools.back();
        if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    }

    VkDescriptorBufferInfo buf_info{};
    buf_info.buffer = m_shadow_ubo.buffer;
    buf_info.offset = 0;
    buf_info.range  = sizeof(ShadowUBO);

    VkDescriptorImageInfo img_info{};
    img_info.sampler     = m_shadow_map.sampler;
    img_info.imageView   = m_shadow_map.depth_view;
    img_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = set;
    writes[0].dstBinding      = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo     = &buf_info;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = set;
    writes[1].dstBinding      = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo      = &img_info;

    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    log::info(TAG, "Shadow descriptor set allocated");
    return set;
}

VkDescriptorSet Renderer::allocate_bone_descriptor(VkBuffer bone_buffer, usize size) {
    VkDevice device = m_rhi->device();

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool     = m_descriptor_pools.back();
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts        = &m_bone_desc_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) {
        if (!allocate_or_grow_pool()) return VK_NULL_HANDLE;
        alloc_info.descriptorPool = m_descriptor_pools.back();
        if (vkAllocateDescriptorSets(device, &alloc_info, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    }

    VkDescriptorBufferInfo buf_info{};
    buf_info.buffer = bone_buffer;
    buf_info.offset = 0;
    buf_info.range  = size;

    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set;
    write.dstBinding      = 0;
    write.descriptorCount = 1;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo     = &buf_info;

    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return set;
}

// ── Shadow depth pass ─────────────────────────────────────────────────────

void Renderer::draw_shadow_pass(VkCommandBuffer cmd, const simulation::World& world, f32 alpha) {
    if (!m_shadow_pipeline) return;

    glm::vec3 light_dir{0.3f, -0.5f, 0.8f};
    glm::vec3 scene_center{4096.0f, 4096.0f, 160.0f};
    f32 scene_radius = 5120.0f;
    glm::mat4 light_vp = compute_light_vp(light_dir, scene_center, scene_radius);

    ShadowUBO ubo{light_vp};
    std::memcpy(m_shadow_ubo.mapped, &ubo, sizeof(ubo));

    VkImageMemoryBarrier2 to_depth{};
    to_depth.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_depth.srcStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_depth.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_depth.dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    to_depth.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_depth.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    to_depth.newLayout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    to_depth.image         = m_shadow_map.depth_image;
    to_depth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &to_depth;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView   = m_shadow_map.depth_view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType            = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea       = {{0, 0}, {m_shadow_map.size, m_shadow_map.size}};
    rendering.layerCount       = 1;
    rendering.pDepthAttachment = &depth_attachment;

    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadow_pipeline);

    VkViewport vp{};
    vp.width    = static_cast<float>(m_shadow_map.size);
    vp.height   = static_cast<float>(m_shadow_map.size);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{{0, 0}, {m_shadow_map.size, m_shadow_map.size}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Terrain shadow (uses terrain shadow pipeline with TerrainVertex stride)
    if (m_terrain_shadow_pipeline && m_terrain.gpu_mesh.vertex_buffer) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_terrain_shadow_pipeline);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        glm::mat4 light_mvp = light_vp;
        vkCmdPushConstants(cmd, m_terrain_shadow_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &light_mvp);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_terrain.gpu_mesh.vertex_buffer, &offset);
        vkCmdBindIndexBuffer(cmd, m_terrain.gpu_mesh.index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m_terrain.gpu_mesh.index_count, 1, 0, 0, 0);
    }

    // Entities — skinned units use skinned shadow pipeline, others use regular
    auto& transforms = world.transforms;
    auto& renderables = world.renderables;


    bool has_skinned_shadow = m_skinned_shadow_pipeline != VK_NULL_HANDLE;

    // Pass A: skinned units
    if (has_skinned_shadow) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinned_shadow_pipeline);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        for (u32 i = 0; i < renderables.count(); ++i) {
            u32 id = renderables.ids()[i];
            const auto& renderable = renderables.data()[i];
            if (!renderable.visible) continue;

            auto* lm = get_or_load_model(renderable.model_path);
            if (!lm || !lm->is_skinned) continue;

            const auto* transform = transforms.get(id);
            if (!transform) continue;
            if (is_fog_hidden(world, id, *transform)) continue;

            auto it = m_anim_instances.find(id);
            if (it == m_anim_instances.end() || !it->second.bone_descriptor) continue;

            // Upload bones (animation already evaluated in draw())
            auto& anim = it->second;
            if (!anim.bone_matrices.empty() && anim.bone_mapped) {
                std::memcpy(anim.bone_mapped, anim.bone_matrices.data(),
                            anim.bone_matrices.size() * sizeof(glm::mat4));
            }

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    m_skinned_shadow_pipeline_layout, 0, 1,
                                    &anim.bone_descriptor, 0, nullptr);

            glm::vec3 vis_pos = lerp_position(*transform, alpha);
            f32 vis_facing = lerp_facing(*transform, alpha);
            glm::mat4 model = glm::translate(glm::mat4{1.0f}, vis_pos);
            if (m_terrain_data) {
                glm::vec3 normal = map::sample_normal(*m_terrain_data,vis_pos.x, vis_pos.y);
                model = model * slope_tilt_matrix(normal);
            }
            model = glm::rotate(model, vis_facing + glm::half_pi<f32>(), glm::vec3{0.0f, 0.0f, 1.0f});
            model = glm::scale(model, glm::vec3{transform->scale});
            // glTF Y-up → game Z-up
            model = model * glm::rotate(glm::mat4{1.0f}, glm::radians(90.0f), glm::vec3{1.0f, 0.0f, 0.0f});

            glm::mat4 light_mvp = light_vp * model;
            vkCmdPushConstants(cmd, m_skinned_shadow_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(glm::mat4), &light_mvp);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &lm->mesh.vertex_buffer, &offset);
            vkCmdBindIndexBuffer(cmd, lm->mesh.index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, lm->mesh.index_count, 1, 0, 0, 0);
        }
    }

    // Pass B: non-skinned entities via indirect draw (Phase 14a)
    // Build instance batches (reused by main pass)
    build_static_draw_batches(world, alpha);

    if (m_shadow_pipeline && !m_draw_groups.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadow_pipeline);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdPushConstants(cmd, m_shadow_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &light_vp);

        // Bind mega VB/IB + instance SSBO once
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_mega_vb, &offset);
        vkCmdBindIndexBuffer(cmd, m_mega_ib, 0, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_shadow_pipeline_layout, 0, 1, &m_instance_desc_set, 0, nullptr);

        // Multi-draw indirect for all static mesh shadows
        u32 draw_count = static_cast<u32>(m_draw_groups.size());
        vkCmdDrawIndexedIndirect(cmd, m_indirect_buffer, 0, draw_count,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }

    vkCmdEndRendering(cmd);

    // Transition shadow map for sampling
    VkImageMemoryBarrier2 to_read{};
    to_read.sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_read.srcStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    to_read.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    to_read.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_read.oldLayout     = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    to_read.newLayout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    to_read.image         = m_shadow_map.depth_image;
    to_read.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    dep.pImageMemoryBarriers = &to_read;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// ── Draw ───────────────────────────────────────────────────────────────────

void Renderer::draw_shadows(VkCommandBuffer cmd, const simulation::World& world, f32 alpha) {
    draw_shadow_pass(cmd, world, alpha);
}

// ── Instance batching for static meshes (Phase 14a) ──────────────────────

void Renderer::build_static_draw_batches(const simulation::World& world, f32 alpha) {
    m_draw_groups.clear();
    m_static_instance_count = 0;

    auto& renderables = world.renderables;
    auto& transforms = world.transforms;


    bool has_skinned = m_skinned_mesh_pipeline != VK_NULL_HANDLE;
    auto frustum = m_camera.frustum();

    // Key: mesh geometry identity (offsets into mega buffer) → group index
    struct GroupKey {
        u32 first_index; u32 index_count; i32 vertex_offset;
        bool operator==(const GroupKey& o) const {
            return first_index == o.first_index && index_count == o.index_count
                && vertex_offset == o.vertex_offset;
        }
    };
    struct GroupKeyHash {
        size_t operator()(const GroupKey& k) const {
            size_t h = std::hash<u32>{}(k.first_index);
            h ^= std::hash<u32>{}(k.index_count) * 2654435761u;
            h ^= std::hash<i32>{}(k.vertex_offset) * 40503u;
            return h;
        }
    };
    std::unordered_map<GroupKey, u32, GroupKeyHash> group_map;

    // CPU staging for instance data (model matrix + material index)
    std::vector<InstanceData> instances;
    instances.reserve(256);

    for (u32 i = 0; i < renderables.count(); ++i) {
        u32 id = renderables.ids()[i];
        const auto& renderable = renderables.data()[i];
        if (!renderable.visible) continue;

        // Skip skinned entities (drawn separately via skinned pipeline)
        if (has_skinned) {
            auto* lm = get_or_load_model(renderable.model_path);
            if (lm && lm->is_skinned) continue;
        }

        const auto* transform = transforms.get(id);
        if (!transform) continue;
        if (is_fog_hidden(world, id, *transform)) continue;

        GpuMesh& mesh = get_or_upload_mesh(renderable.model_path);
        if (!mesh.vertex_buffer || !mesh.index_buffer) continue;

        // Frustum cull: skip entities whose bounding sphere is entirely off-screen
        {
            glm::vec3 pos = lerp_position(*transform, alpha);
            f32 radius = mesh.bounding_radius * transform->scale;
            if (!frustum.is_sphere_visible(pos, radius)) continue;
        }

        // Resolve bindless texture index
        bool is_corpse = world.dead_states.has(id);
        auto* lm_static = get_or_load_model(renderable.model_path);
        u32 tex_idx;
        if (is_corpse && !(lm_static && lm_static->diffuse_texture.image != VK_NULL_HANDLE)) {
            tex_idx = m_corpse_tex_idx;
        } else if (lm_static) {
            tex_idx = lm_static->texture_index;
        } else {
            tex_idx = m_default_tex_idx;
        }

        // Compute model matrix
        glm::vec3 vis_pos = lerp_position(*transform, alpha);
        f32 vis_facing = lerp_facing(*transform, alpha);
        glm::mat4 model = glm::translate(glm::mat4{1.0f}, vis_pos);
        if (m_terrain_data) {
            glm::vec3 normal = map::sample_normal(*m_terrain_data, vis_pos.x, vis_pos.y);
            model = model * slope_tilt_matrix(normal);
        }
        f32 facing = vis_facing;
        if (!mesh.native_z_up) facing += glm::half_pi<f32>();
        model = glm::rotate(model, facing, glm::vec3{0.0f, 0.0f, 1.0f});
        model = glm::scale(model, glm::vec3{transform->scale});
        if (!mesh.native_z_up) {
            model = model * glm::rotate(glm::mat4{1.0f}, glm::radians(90.0f), glm::vec3{1.0f, 0.0f, 0.0f});
        }

        // Find or create draw group keyed by mesh geometry (Phase 14b)
        GroupKey key{mesh.first_index, mesh.index_count,
                     static_cast<i32>(mesh.first_vertex)};
        auto git = group_map.find(key);
        if (git == group_map.end()) {
            u32 gi = static_cast<u32>(m_draw_groups.size());
            group_map[key] = gi;
            DrawGroup dg{};
            dg.first_index    = mesh.first_index;
            dg.index_count    = mesh.index_count;
            dg.vertex_offset  = static_cast<i32>(mesh.first_vertex);
            dg.first_instance = static_cast<u32>(instances.size());
            dg.instance_count = 0;
            m_draw_groups.push_back(dg);
            git = group_map.find(key);
        }

        m_draw_groups[git->second].instance_count++;
        InstanceData inst{};
        inst.model = model;
        inst.material_index = tex_idx;
        instances.push_back(inst);
    }

    m_static_instance_count = static_cast<u32>(instances.size());

    // Upload instance data to GPU
    if (m_static_instance_count > 0 && m_instance_mapped) {
        u32 upload_count = std::min(m_static_instance_count, MAX_STATIC_INSTANCES);
        std::memcpy(m_instance_mapped, instances.data(), upload_count * sizeof(InstanceData));
    }

    // Build indirect commands and upload
    if (!m_draw_groups.empty() && m_indirect_mapped) {
        auto* cmds = static_cast<VkDrawIndexedIndirectCommand*>(m_indirect_mapped);
        for (u32 i = 0; i < m_draw_groups.size(); ++i) {
            auto& dg = m_draw_groups[i];
            cmds[i].indexCount    = dg.index_count;
            cmds[i].instanceCount = dg.instance_count;
            cmds[i].firstIndex    = dg.first_index;
            cmds[i].vertexOffset  = dg.vertex_offset;
            cmds[i].firstInstance = dg.first_instance;
        }
    }
}

void Renderer::draw(VkCommandBuffer cmd, VkExtent2D extent, const simulation::World& world, f32 alpha) {
    if (extent.width == 0 || extent.height == 0) return;

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{{0, 0}, extent};
    glm::mat4 vp = m_camera.view_projection();

    // ── Draw terrain with splatmap pipeline ──────────────────────────────
    if (m_terrain_pipeline && m_terrain.gpu_mesh.vertex_buffer && m_terrain_material.descriptor_set) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_terrain_pipeline);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkDescriptorSet terrain_sets[] = {m_terrain_material.descriptor_set, m_shadow_desc_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_terrain_pipeline_layout, 0, 2, terrain_sets, 0, nullptr);

        glm::mat4 terrain_model{1.0f};
        glm::mat4 terrain_mvp = vp * terrain_model;
        struct {
            glm::mat4 mvp;
            glm::mat4 model;
            glm::vec2 world_size;
            f32 fog_enabled;
            f32 _pad;
        } terrain_push{};
        terrain_push.mvp = terrain_mvp;
        terrain_push.model = terrain_model;
        terrain_push.world_size = m_terrain_data
            ? glm::vec2{m_terrain_data->world_width(), m_terrain_data->world_height()}
            : glm::vec2{1.0f};
        terrain_push.fog_enabled = m_fog_enabled ? 1.0f : 0.0f;
        vkCmdPushConstants(cmd, m_terrain_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(terrain_push), &terrain_push);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_terrain.gpu_mesh.vertex_buffer, &offset);
        vkCmdBindIndexBuffer(cmd, m_terrain.gpu_mesh.index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m_terrain.gpu_mesh.index_count, 1, 0, 0, 0);
    }

    // ── Draw entities ────────────────────────────────────────────────────
    if (!m_mesh_pipeline) return;

    auto& transforms = world.transforms;
    auto& renderables = world.renderables;


    // Determine frame dt for animation (approximate from last frame)
    static auto last_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    f32 frame_dt = std::chrono::duration<f32>(now - last_time).count();
    frame_dt = std::min(frame_dt, 0.1f);  // clamp to avoid huge jumps
    last_time = now;

    bool has_skinned_pipeline = m_skinned_mesh_pipeline != VK_NULL_HANDLE;

    // Clean up animation instances for entities that no longer exist in the world
    {
        std::vector<u32> stale;
        for (auto& [eid, anim] : m_anim_instances) {
            if (!world.handle_infos.has(eid)) {
                stale.push_back(eid);
            }
        }
        if (!stale.empty()) {
            // Wait for GPU to finish using the bone buffers before destroying
            vkDeviceWaitIdle(m_rhi->device());
        }
        for (u32 eid : stale) {
            auto it = m_anim_instances.find(eid);
            if (it != m_anim_instances.end()) {
                if (it->second.bone_buffer) {
                    vmaDestroyBuffer(m_rhi->allocator(), it->second.bone_buffer, it->second.bone_alloc);
                }
                m_anim_instances.erase(it);
            }
        }
    }

    // Update animations for all skinned entities
    if (has_skinned_pipeline) {
        for (u32 i = 0; i < renderables.count(); ++i) {
            u32 id = renderables.ids()[i];
            const auto& renderable = renderables.data()[i];

            auto* lm = get_or_load_model(renderable.model_path);
            if (!lm || !lm->is_skinned) continue;

            auto& anim = get_or_create_anim(id, *lm);

            // Skip birth animation for revealed entities (not newly created)
            if (renderable.skip_birth && anim.current_state == AnimState::Birth) {
                anim.current_state = AnimState::Idle;
                anim.looping = true;
                anim.time = 0;
            }

            auto anim_info = derive_anim_state(world, id, anim);
            set_anim_state(anim, anim_info.state, anim_info.duration, anim_info.force_restart,
                           anim_info.has_attack_info ? &anim_info.attack_info : nullptr);
            update_animation(anim, frame_dt);
            evaluate_animation(anim);
        }
    }

    // Pass 1: Draw skinned units with skinned pipeline
    if (has_skinned_pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinned_mesh_pipeline);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        for (u32 i = 0; i < renderables.count(); ++i) {
            u32 id = renderables.ids()[i];
            const auto& renderable = renderables.data()[i];
            if (!renderable.visible) continue;

            auto* lm = get_or_load_model(renderable.model_path);
            if (!lm || !lm->is_skinned) continue;

            const auto* transform = transforms.get(id);
            if (!transform) continue;

            // Skip enemies hidden by fog of war
            if (is_fog_hidden(world, id, *transform)) continue;

            auto it = m_anim_instances.find(id);
            if (it == m_anim_instances.end()) continue;
            auto& anim = it->second;
            if (!anim.bone_descriptor) continue;

            // Upload this entity's bone matrices to its own SSBO
            if (!anim.bone_matrices.empty() && anim.bone_mapped) {
                std::memcpy(anim.bone_mapped, anim.bone_matrices.data(),
                            anim.bone_matrices.size() * sizeof(glm::mat4));
            }

            // Use corpse material only for placeholder models (no real texture)
            bool is_corpse = world.dead_states.has(id);
            bool has_own_texture = lm->diffuse_texture.image != VK_NULL_HANDLE;
            auto& mat = (is_corpse && !has_own_texture) ? m_corpse_material : lm->material;
            if (mat.descriptor_set && m_shadow_desc_set) {
                VkDescriptorSet sets[] = {mat.descriptor_set, m_shadow_desc_set,
                                          anim.bone_descriptor};
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_skinned_mesh_pipeline_layout, 0, 3, sets, 0, nullptr);
            }

            glm::vec3 vis_pos = lerp_position(*transform, alpha);
            f32 vis_facing = lerp_facing(*transform, alpha);
            glm::mat4 model = glm::translate(glm::mat4{1.0f}, vis_pos);
            if (m_terrain_data) {
                glm::vec3 normal = map::sample_normal(*m_terrain_data,vis_pos.x, vis_pos.y);
                model = model * slope_tilt_matrix(normal);
            }
            model = glm::rotate(model, vis_facing + glm::half_pi<f32>(), glm::vec3{0.0f, 0.0f, 1.0f});
            model = glm::scale(model, glm::vec3{transform->scale});
            // glTF Y-up → game Z-up
            model = model * glm::rotate(glm::mat4{1.0f}, glm::radians(90.0f), glm::vec3{1.0f, 0.0f, 0.0f});

            glm::mat4 mvp = vp * model;
            struct { glm::mat4 mvp; glm::mat4 model; } push{mvp, model};
            vkCmdPushConstants(cmd, m_skinned_mesh_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(push), &push);

            VkDeviceSize vb_offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &lm->mesh.vertex_buffer, &vb_offset);
            vkCmdBindIndexBuffer(cmd, lm->mesh.index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, lm->mesh.index_count, 1, 0, 0, 0);
        }
    }

    // Pass 2: Draw non-skinned entities via mega buffer + bindless + indirect draw (Phase 14b)
    if (m_mesh_pipeline && !m_draw_groups.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_mesh_pipeline);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Push vp matrix (same for all instances)
        vkCmdPushConstants(cmd, m_mesh_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &vp);

        // Bind mega vertex/index buffers once (all static meshes share them)
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_mega_vb, &offset);
        vkCmdBindIndexBuffer(cmd, m_mega_ib, 0, VK_INDEX_TYPE_UINT32);

        // Bind all descriptor sets once: bindless textures + shadow + instance SSBO
        VkDescriptorSet sets[] = {m_bindless_set, m_shadow_desc_set, m_instance_desc_set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_mesh_pipeline_layout, 0, 3, sets, 0, nullptr);

        // Multi-draw indirect: one command per unique mesh geometry
        u32 draw_count = static_cast<u32>(m_draw_groups.size());
        vkCmdDrawIndexedIndirect(cmd, m_indirect_buffer, 0, draw_count,
                                 sizeof(VkDrawIndexedIndirectCommand));
    }

    // ── Pass 3: Particles (alpha-blended, drawn last) ────────────────────
    if (m_particle_pipeline) {
        // Compute camera right/up from view matrix for billboarding
        glm::mat4 view = m_camera.view_matrix();
        glm::vec3 cam_right{view[0][0], view[1][0], view[2][0]};
        glm::vec3 cam_up{view[0][1], view[1][1], view[2][1]};

        // Update effect instances (follow units, continuous emission, expiry)
        m_effect_manager.update(frame_dt, [](simulation::Unit u, void* ctx) -> glm::vec3 {
            auto* w = static_cast<const simulation::World*>(ctx);
            auto* t = w->transforms.get(u.id);
            return t ? t->position : glm::vec3{0};
        }, const_cast<void*>(static_cast<const void*>(&world)));

        m_particles.update(frame_dt);
        m_particles.upload(cam_right, cam_up);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_particle_pipeline);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        glm::mat4 particle_vp = vp;
        vkCmdPushConstants(cmd, m_particle_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(glm::mat4), &particle_vp);

        m_particles.draw(cmd);
    }
}

} // namespace uldum::render
