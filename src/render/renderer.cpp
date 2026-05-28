#include "render/renderer.h"
#include "rhi/rhi.h"
#include "map/terrain_data.h"
#include "map/map.h"
#include "asset/asset.h"
#include "asset/texture.h"
#include "simulation/world.h"
#include "simulation/simulation.h"
#include "simulation/components.h"
#include "simulation/type_registry.h"
#include "asset/model.h"
#include "core/log.h"

#if defined(ULDUM_BACKEND_GLES)
#  include <stb_image_resize2.h>
#endif

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace uldum::render {

static constexpr const char* TAG = "Render";

// Multiplier applied to a unit's visual_alpha when it carries
// UNIT_STATUS_INVISIBLE AND is being rendered (which only happens for
// owner / ally / true-sight viewers — anything that can't see invisible
// is already culled by Vision::is_unit_visible_to). Gives every
// invisibility ability a consistent "ghost" appearance to its allies
// without each map having to declare visual_alpha_mult on every
// invisibility buff. Map abilities can layer their own
// visual_alpha_mult modifier on top to dim further or to override
// with visual_alpha_mult: 0 if they want a different visual treatment.
static constexpr f32 kInvisibleGhostAlpha = 0.5f;

// Multiplier on a static-remembered entity (tree / doodad / building)
// rendered from the player's *memory* of an Explored tile rather than
// live vision. Reads as "scouted but no longer lit."
static constexpr f32 kFoggedMemoryAlpha = 0.6f;

static f32 effective_visual_alpha(const simulation::World& world, u32 id,
                                  const simulation::Renderable& renderable) {
    f32 a = renderable.visual_alpha;
    auto* sf = world.status_flags.get(id);
    if (sf && (sf->flags & simulation::status::Invisible)) {
        a *= kInvisibleGhostAlpha;
    }
    return a;
}

// ── Shader loading helper ──────────────────────────────────────────────────

static rhi::ShaderModuleHandle load_shader(rhi::Rhi& rhi, std::string_view path) {
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) {
        log::error(TAG, "Shader load without AssetManager: '{}'", path);
        return {};
    }
#if defined(ULDUM_BACKEND_GLES)
    // GLES backend reads tagged GLSL ES blobs, not SPIR-V. The Android
    // Gradle build emits both `.spv` (Vulkan) and `.glsl` (GLES) into
    // engine.uldpak; consumer code names the .spv path uniformly, and we
    // swap the extension here.
    std::string resolved(path);
    if (resolved.size() >= 4 && resolved.substr(resolved.size() - 4) == ".spv") {
        resolved.replace(resolved.size() - 4, 4, ".glsl");
    }
    auto bytes = mgr->read_file_bytes(resolved);
#else
    auto bytes = mgr->read_file_bytes(path);
#endif
    if (bytes.empty()) {
        log::error(TAG, "Shader not found in any package: '{}'", path);
        return {};
    }
    return rhi.create_shader_module(bytes);
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

// ── Render interpolation helpers ───────────────────────────────────────────
// Thin wrappers around Transform's interp_position / interp_facing methods.
// Kept here so existing call sites (which are many in renderer.cpp) don't
// need to change shape. The canonical implementation lives in components.h.
static glm::vec3 lerp_position(const simulation::Transform& t, f32 alpha) { return t.interp_position(alpha); }
static f32       lerp_facing  (const simulation::Transform& t, f32 alpha) { return t.interp_facing(alpha); }

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

bool Renderer::init(rhi::Rhi& rhi) {
    m_rhi = &rhi;

    f32 aspect = static_cast<f32>(rhi.extent().width) / static_cast<f32>(rhi.extent().height);
    m_camera.init(aspect);

    if (!create_descriptor_layouts()) return false;
    if (!create_bindless_resources()) return false;
    if (!create_default_texture()) return false;
    if (!create_shadow_resources()) return false;
    if (!create_terrain_textures()) return false;
    if (!create_transition_noise()) return false;
    if (!create_water_normal()) return false;
    // Mesh / skinned-mesh pipelines are non-fatal: the bindless static-mesh
    // path uses `GL_EXT_nonuniform_qualifier`, which spirv-cross can't
    // translate to GLSL ES, so on the GLES backend these pipelines stay
    // invalid and unit rendering is skipped. The rest of the renderer
    // (terrain, water, particles, HUD, overlays) still comes up so we can
    // see the world. Vulkan path always succeeds and warning never fires.
    if (!create_mesh_pipeline()) {
        log::warn(TAG, "mesh pipeline unavailable — static unit meshes won't render");
    }
    if (!create_skinned_mesh_pipeline()) {
        log::warn(TAG, "skinned-mesh pipeline unavailable — animated units won't render");
    }
    if (!m_particles.init(rhi)) return false;  // must be before particle pipeline (creates desc layout)
    if (!create_particle_pipeline()) return false;
    if (!create_terrain_pipeline()) return false;
    if (!create_water_pipeline()) return false;
    if (!create_skybox_pipeline()) return false;
    if (!create_skybox_mesh()) return false;
    if (!create_shadow_pipeline()) return false;
    m_effect_registry.register_defaults();
    m_effect_manager.set_particles(&m_particles);
    m_effect_manager.set_registry(&m_effect_registry);

    // Mega vertex/index buffers — all static meshes share one VB + IB (Phase 14b)
    {
        {
            rhi::BufferDesc d{};
            d.size   = MEGA_MAX_VERTICES * sizeof(asset::Vertex);
            d.usage  = rhi::BufferUsage::Vertex;
            d.memory = rhi::MemoryUsage::HostSequential;
            m_mega_vb = rhi.create_buffer(d);
        }
        {
            rhi::BufferDesc d{};
            d.size   = MEGA_MAX_INDICES * sizeof(u32);
            d.usage  = rhi::BufferUsage::Index;
            d.memory = rhi::MemoryUsage::HostSequential;
            m_mega_ib = rhi.create_buffer(d);
        }

        log::info(TAG, "Mega buffers created: VB {} verts, IB {} indices",
                  MEGA_MAX_VERTICES, MEGA_MAX_INDICES);
    }

    // Instance SSBO + indirect buffer, one of each per frame-in-flight.
    for (u32 f = 0; f < rhi::MAX_FRAMES_IN_FLIGHT; ++f) {
        {
            rhi::BufferDesc d{};
            d.size   = MAX_STATIC_INSTANCES * sizeof(InstanceData);
            d.usage  = rhi::BufferUsage::Storage;
            d.memory = rhi::MemoryUsage::HostSequential;
            m_instance_buffer[f] = rhi.create_buffer(d);

            m_instance_desc_set[f] = allocate_bone_descriptor(
                m_instance_buffer[f],
                MAX_STATIC_INSTANCES * sizeof(InstanceData));
        }

        {
            rhi::BufferDesc d{};
            d.size   = MAX_STATIC_INSTANCES * sizeof(rhi::DrawIndexedIndirectCommand);
            d.usage  = rhi::BufferUsage::Indirect;
            d.memory = rhi::MemoryUsage::HostSequential;
            m_indirect_buffer[f] = rhi.create_buffer(d);
        }
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

    m_init_mega_vb_used   = m_mega_vb_used;
    m_init_mega_ib_used   = m_mega_ib_used;
    m_init_bindless_count = m_bindless_count;

    log::info(TAG, "Renderer initialized — textured mesh + skinned + terrain pipelines ready");
    return true;
}

void Renderer::end_session() {
    if (!m_rhi) return;
    // GPU must be idle before freeing any bone buffer still in flight.
    m_rhi->wait_idle();
    for (auto& [eid, anim] : m_anim_instances) {
        m_rhi->destroy_buffer(anim.bone_buffer);
        // Descriptor sets will be freed when the pool is destroyed in
        // shutdown(). They're cheap to leak per session for the small
        // # of units we run; revisit when sessions are very long-lived.
    }
    m_anim_instances.clear();
    // Drop every persistent effect + in-flight particle so the next
    // session doesn't start with the previous map's emitters still
    // running.
    m_effect_manager.clear();

    // Roll back model + mesh caches. Same-relative-path / different-bytes
    // models from different maps (test_map vs action_test, etc.) would
    // otherwise return the previous session's bytes from the cache.
    // Skinned meshes own separate VB/IB/bone allocations (not slices of
    // the mega buffer) — destroy_mesh frees those; for static meshes it
    // no-ops because alloc is null.
    for (auto& [path, lm] : m_model_cache) {
        destroy_mesh(*m_rhi, lm.mesh);
        if (lm.diffuse_texture.texture.is_valid()) destroy_texture(*m_rhi, lm.diffuse_texture);
    }
    m_model_cache.clear();
    m_model_failed.clear();
    m_mesh_cache.clear();
    m_mega_vb_used   = m_init_mega_vb_used;
    m_mega_ib_used   = m_init_mega_ib_used;
    m_bindless_count = m_init_bindless_count;
}

void Renderer::shutdown() {
    if (!m_rhi) return;
    m_rhi->wait_idle();

    m_particles.shutdown();
    destroy_terrain_mesh(*m_rhi, m_terrain);
    for (auto& [eid, anim] : m_anim_instances) {
        // Descriptor sets freed implicitly when pools are destroyed below
        m_rhi->destroy_buffer(anim.bone_buffer);
    }
    m_anim_instances.clear();
    for (auto& [path, lm] : m_model_cache) {
        destroy_mesh(*m_rhi, lm.mesh);
        if (lm.diffuse_texture.texture.is_valid()) destroy_texture(*m_rhi, lm.diffuse_texture);
    }
    m_model_cache.clear();
    m_model_failed.clear();
    destroy_mesh(*m_rhi, m_projectile_mesh);
    destroy_mesh(*m_rhi, m_placeholder_mesh);
    for (auto& [path, mesh] : m_mesh_cache) {
        destroy_mesh(*m_rhi, mesh);
    }
    m_mesh_cache.clear();

    // Destroy instance/indirect/mega buffers
    for (u32 f = 0; f < rhi::MAX_FRAMES_IN_FLIGHT; ++f) {
        m_rhi->destroy_buffer(m_instance_buffer[f]);
        m_rhi->destroy_buffer(m_indirect_buffer[f]);
        m_instance_buffer[f] = {};
        m_indirect_buffer[f] = {};
        if (m_instance_desc_set[f].is_valid()) m_rhi->free_descriptor_set(m_instance_desc_set[f]);
        m_instance_desc_set[f] = {};
    }
    m_rhi->destroy_buffer(m_mega_vb);
    m_rhi->destroy_buffer(m_mega_ib);

    // Destroy bindless resources
    if (m_bindless_set.is_valid()) m_rhi->free_descriptor_set(m_bindless_set);
    m_rhi->destroy_descriptor_set_layout(m_bindless_layout);
#if defined(ULDUM_BACKEND_GLES)
    if (m_unit_tex_sampler.is_valid()) m_rhi->destroy_sampler(m_unit_tex_sampler);
    if (m_unit_tex_array.is_valid())   m_rhi->destroy_texture(m_unit_tex_array);
#endif

    // Destroy fog resources
    if (m_fog_texture.texture.is_valid()) destroy_texture(*m_rhi, m_fog_texture);
    m_rhi->destroy_buffer(m_fog_staging_buffer);

    // Destroy textures
    destroy_texture(*m_rhi, m_corpse_texture);
    destroy_texture(*m_rhi, m_default_texture);
    if (m_terrain_material.layer_array.texture.is_valid()) destroy_texture(*m_rhi, m_terrain_material.layer_array);
    if (m_terrain_material.normal_array.texture.is_valid()) destroy_texture(*m_rhi, m_terrain_material.normal_array);
    if (m_transition_noise.texture.is_valid()) destroy_texture(*m_rhi, m_transition_noise);
    if (m_water_normal.texture.is_valid()) destroy_texture(*m_rhi, m_water_normal);

    // Destroy shadow resources
    destroy_shadow_map(*m_rhi, m_shadow_map);
    destroy_shadow_buffer(*m_rhi, m_shadow_ubo);
    m_rhi->destroy_buffer(m_env_ubo_buffer);
    m_env_ubo_buffer = {};
    if (m_default_cubemap.texture.is_valid()) destroy_texture(*m_rhi, m_default_cubemap);

    // Destroy pipelines
    m_rhi->destroy_pipeline(m_particle_pipeline);
    m_rhi->destroy_pipeline_layout(m_particle_pipeline_layout);
    m_rhi->destroy_pipeline(m_skinned_shadow_pipeline);
    m_rhi->destroy_pipeline_layout(m_skinned_shadow_pipeline_layout);
    m_rhi->destroy_pipeline(m_skinned_mesh_pipeline);
    m_rhi->destroy_pipeline_layout(m_skinned_mesh_pipeline_layout);
    m_rhi->destroy_pipeline(m_terrain_shadow_pipeline);
    // m_terrain_shadow_pipeline_layout is shared with m_shadow_pipeline_layout, don't destroy twice
    m_rhi->destroy_pipeline(m_shadow_pipeline);
    m_rhi->destroy_pipeline_layout(m_shadow_pipeline_layout);
    m_rhi->destroy_pipeline(m_water_pipeline);
    m_rhi->destroy_pipeline_layout(m_water_pipeline_layout);
    m_rhi->destroy_pipeline(m_skybox_pipeline);
    m_rhi->destroy_pipeline_layout(m_skybox_pipeline_layout);
    m_rhi->destroy_descriptor_set_layout(m_skybox_desc_layout);
    if (m_skybox_cubemap.texture.is_valid())    destroy_texture(*m_rhi, m_skybox_cubemap);
    destroy_mesh(*m_rhi, m_skybox_mesh);
    m_rhi->destroy_pipeline(m_terrain_pipeline);
    m_rhi->destroy_pipeline_layout(m_terrain_pipeline_layout);
    m_rhi->destroy_pipeline(m_mesh_pipeline);
    m_rhi->destroy_pipeline_layout(m_mesh_pipeline_layout);

    // Destroy descriptor infrastructure
    m_rhi->destroy_descriptor_set_layout(m_bone_desc_layout);
    m_rhi->destroy_descriptor_set_layout(m_shadow_desc_layout);
    m_rhi->destroy_descriptor_set_layout(m_terrain_desc_layout);
    m_rhi->destroy_descriptor_set_layout(m_mesh_desc_layout);

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
        rhi::BufferDesc d{};
        d.size   = bone_count * sizeof(glm::mat4);
        d.usage  = rhi::BufferUsage::Storage;
        d.memory = rhi::MemoryUsage::HostSequential;
        inst.bone_buffer = m_rhi->create_buffer(d);

        // Initialize to identity
        auto* bones = static_cast<glm::mat4*>(m_rhi->mapped_ptr(inst.bone_buffer));
        for (u32 i = 0; i < bone_count; ++i) bones[i] = glm::mat4{1.0f};

        // Allocate descriptor set
        inst.bone_descriptor = allocate_bone_descriptor(
            inst.bone_buffer,
            bone_count * sizeof(glm::mat4));
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

// Pick the animation state for a unit this frame. Priority order:
//
//   Death > Spell > Attack > Walk > Birth > Idle
//
// Each arm fetches only the components it needs. Birth sits below
// the active states (Spell/Attack/Walk) so that an emerge clip
// gets interrupted the moment the unit starts doing something —
// equivalent to the explicit "Birth when not busy" gate, and lets
// the lower-priority arms own the early-return path that already
// consults their own components.
static AnimStateInfo derive_anim_state(const simulation::World& world, u32 id,
                                       AnimationInstance& anim) {
    using simulation::CastState;
    using simulation::AttackState;

    if (world.dead_states.has(id)) return {AnimState::Death, 0.8f, false};

    auto get_type_def = [&]() -> const simulation::UnitTypeDef* {
        auto* hi = world.handle_infos.get(id);
        return (hi && world.types) ? world.types->get_unit_type(hi->type_id) : nullptr;
    };

    // Spell — cast pump in Foreswing / Channeling / Backswing.
    if (auto* aset = world.ability_sets.get(id);
        aset && (aset->cast_state == CastState::Foreswing  ||
                 aset->cast_state == CastState::Channeling ||
                 aset->cast_state == CastState::Backswing)) {
        auto* type_def = get_type_def();
        f32 cp = type_def ? type_def->cast_pt : 0.5f;
        AttackAnimInfo info;
        info.dmg_point  = cp;
        info.cast_point = aset->foreswing_secs;
        info.backswing  = aset->cast_backswing_secs;
        // Channel duration adds straight through; whatever animation the
        // unit's spell clip plays simply holds while the channel runs.
        f32 dur = aset->foreswing_secs + aset->channel_secs + aset->cast_backswing_secs;
        return {AnimState::Spell, dur, false, info, true};
    }

    // Attack — combat in WindUp/Backswing/Cooldown. Holds the last
    // frame during Cooldown so the silhouette reads as "follow-through".
    auto* combat = world.combats.get(id);
    if (combat && (combat->attack_state == AttackState::WindUp     ||
                   combat->attack_state == AttackState::Backswing  ||
                   combat->attack_state == AttackState::Cooldown)) {
        // Detect a fresh swing (wind-up nearing damage point, swing_id
        // changed since last frame) so the renderer retriggers the
        // attack clip from frame 0.
        bool new_swing = false;
        if (combat->attack_state == AttackState::WindUp &&
            combat->attack_timer > combat->dmg_time * 0.8f) {
            u32 swing_id = static_cast<u32>(combat->attack_timer * 1000);
            if (swing_id != anim.attack_swing_id) {
                anim.attack_swing_id = swing_id;
                new_swing = true;
            }
        }
        AttackAnimInfo info;
        info.dmg_point  = combat->dmg_pt;
        info.cast_point = combat->dmg_time;
        info.backswing  = combat->backsw_time;
        f32 dur = combat->dmg_time + combat->backsw_time;
        return {AnimState::Attack, dur, new_swing, info, true};
    }

    // Walk — either driven by an active Move (`mov->moving`) or by
    // the combat-approach phase before WindUp.
    auto* mov = world.movements.get(id);
    bool approach = combat && combat->attack_state == AttackState::MovingToTarget;
    if ((mov && mov->moving) || approach) {
        auto* type_def = get_type_def();
        f32 ref = type_def ? type_def->walk_speed : 0;
        if (ref <= 0 && mov) ref = mov->speed;
        f32 ratio = (mov && mov->speed > 0 && ref > 0) ? mov->speed / ref : 1.0f;
        return {AnimState::Walk, -ratio, false};
    }

    // Birth — only relevant when nothing else is happening. Once
    // interrupted (we returned Spell/Attack/Walk above on a previous
    // frame), `anim.current_state` is no longer Birth, so this never
    // reactivates.
    if (anim.current_state == AnimState::Birth && !anim.finished) {
        return {AnimState::Birth, 0, false};
    }

    return {AnimState::Idle, 0, false};
}

void Renderer::set_environment(const map::EnvironmentConfig& env) {
    void* env_mapped = m_rhi->mapped_ptr(m_env_ubo_buffer);
    if (!env_mapped) return;

    m_sun_direction = glm::normalize(env.sun_direction);
    m_env_data = {};
    m_env_data.sun_direction = glm::vec4(m_sun_direction, env.sun_intensity);
    m_env_data.sun_color     = glm::vec4(env.sun_color, 0.0f);
    m_env_data.ambient_color = glm::vec4(env.ambient_color, env.ambient_intensity);
    m_env_data.fog_color     = glm::vec4(env.fog_color, 0.0f);
    std::memcpy(env_mapped, &m_env_data, sizeof(m_env_data));

    // Load skybox cubemap if specified
    m_has_skybox = false;
    if (env.has_skybox() && !m_map_root.empty()) {
        if (m_skybox_cubemap.texture.is_valid()) destroy_texture(*m_rhi, m_skybox_cubemap);

        // Vulkan cubemap layers: +X, -X, +Y, -Y, +Z, -Z
        // Game coords: X=right, Y=forward, Z=up
        std::string paths[6] = {
            m_map_root + "/" + env.skybox_right,   // +X
            m_map_root + "/" + env.skybox_left,    // -X
            m_map_root + "/" + env.skybox_front,   // +Y (forward)
            m_map_root + "/" + env.skybox_back,    // -Y (backward)
            m_map_root + "/" + env.skybox_top,     // +Z (up = sky)
            m_map_root + "/" + env.skybox_bottom,  // -Z (down = ground)
        };

        // Load all 6 faces
        const u8* face_data[6] = {};
        std::vector<std::vector<u8>> face_pixels(6);
        u32 face_w = 0, face_h = 0;
        bool all_loaded = true;

        auto* mgr = asset::AssetManager::instance();
        for (u32 i = 0; i < 6; ++i) {
            auto bytes = mgr ? mgr->read_file_bytes(paths[i]) : std::vector<u8>{};
            if (bytes.empty()) {
                log::warn(TAG, "Failed to load skybox face '{}' — skipping skybox", paths[i]);
                all_loaded = false;
                break;
            }
            auto result = asset::load_texture_from_memory(bytes.data(), static_cast<u32>(bytes.size()));
            if (!result || result->channels != 4) {
                log::warn(TAG, "Failed to load skybox face '{}' — skipping skybox", paths[i]);
                all_loaded = false;
                break;
            }
            if (i == 0) { face_w = result->width; face_h = result->height; }
            if (result->width != face_w || result->height != face_h) {
                log::warn(TAG, "Skybox face '{}' size mismatch — skipping skybox", paths[i]);
                all_loaded = false;
                break;
            }
            face_pixels[i] = std::move(result->pixels);
            face_data[i] = face_pixels[i].data();
        }

        if (all_loaded && face_w > 0) {
            m_skybox_cubemap = upload_texture_cubemap(*m_rhi, face_data, face_w, face_h);
            if (m_skybox_cubemap.texture.is_valid()) {
                if (m_skybox_desc_set.is_valid()) m_rhi->free_descriptor_set(m_skybox_desc_set);
                m_skybox_desc_set = m_rhi->allocate_descriptor_set(m_skybox_desc_layout);
                if (m_skybox_desc_set.is_valid()) {
                    rhi::WriteDescriptor w{};
                    w.binding = 0;
                    w.type    = rhi::DescriptorType::CombinedImageSampler;
                    w.texture = m_skybox_cubemap.texture;
                    w.sampler = m_skybox_cubemap.sampler;
                    m_rhi->update_descriptor_set(m_skybox_desc_set, std::span{&w, 1});
                    m_has_skybox = true;
                    log::info(TAG, "Skybox loaded ({}x{})", face_w, face_h);
                }
            }
        }
    }

    // Update default cubemap for water reflection (uses skybox or fog color)
    // Re-allocate shadow descriptor set so binding 3 uses the skybox cubemap
    m_shadow_desc_set = allocate_shadow_descriptor();

    log::info(TAG, "Environment set — sun dir ({:.2f},{:.2f},{:.2f}), intensity {:.1f}, skybox: {}",
              env.sun_direction.x, env.sun_direction.y, env.sun_direction.z, env.sun_intensity,
              m_has_skybox ? "yes" : "no");
}

void Renderer::add_point_light(glm::vec3 position, glm::vec3 color, f32 radius, f32 intensity) {
    u32 count = static_cast<u32>(m_env_data.light_count.x);
    if (count >= MAX_POINT_LIGHTS) return;
    m_env_data.lights[count].position = glm::vec4(position, radius);
    m_env_data.lights[count].color    = glm::vec4(color, intensity);
    m_env_data.light_count.x = static_cast<i32>(count + 1);
}

void Renderer::set_terrain(const map::TerrainData* terrain) {
    // The terrain mesh is bound by every frame's draw cmds; the previous
    // frame may still be in flight when callers swap or clear terrain.
    m_rhi->wait_idle();
    destroy_terrain_mesh(*m_rhi, m_terrain);

    if (!terrain) {
        // Teardown: free GPU mesh + drop CPU data pointer. Used by
        // App::end_session / leave_lobby so the renderer doesn't keep
        // a stale reference into a destroyed map.
        m_terrain_data = nullptr;
        return;
    }

    m_terrain = build_terrain_mesh(*m_rhi, *terrain);
    m_terrain_data = terrain;

    // Re-allocate terrain descriptor set.
    if (terrain->is_valid()) {
        m_terrain_material.descriptor_set = allocate_terrain_descriptor(m_terrain_material);
    }
}

// ── Fog of war texture ────────────────────────────────────────────────────

void Renderer::set_fog_grid(const f32* values, u32 tiles_x, u32 tiles_y) {
    if (!values || tiles_x == 0 || tiles_y == 0) {
        m_fog_enabled = false;
        return;
    }

    // (Re)create fog texture if dimensions changed
    if (tiles_x != m_fog_width || tiles_y != m_fog_height) {
        // Destroy old resources
        if (m_fog_texture.texture.is_valid()) destroy_texture(*m_rhi, m_fog_texture);
        m_rhi->destroy_buffer(m_fog_staging_buffer);
        m_fog_staging_buffer = {};

        m_fog_width = tiles_x;
        m_fog_height = tiles_y;

        // Create persistent staging buffer (mapped once)
        {
            rhi::BufferDesc d{};
            d.size   = static_cast<u64>(tiles_x) * tiles_y * 4; // RGBA
            d.usage  = rhi::BufferUsage::TransferSrc;
            d.memory = rhi::MemoryUsage::HostSequential;
            m_fog_staging_buffer = m_rhi->create_buffer(d);
        }

        // Create GPU image (R8G8B8A8_UNORM — we expand R8 to RGBA for compatibility)
        m_fog_texture = {};
        m_fog_texture.width = tiles_x;
        m_fog_texture.height = tiles_y;
        {
            rhi::TextureDesc td{};
            td.width  = tiles_x;
            td.height = tiles_y;
            td.format = rhi::TextureFormat::R8G8B8A8_UNORM;
            td.usage  = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
            m_fog_texture.texture = m_rhi->create_texture(td);
        }

        // Sampler (bilinear, clamp to edge for smooth fog borders)
        {
            rhi::SamplerDesc sd{};
            sd.address_u   = rhi::AddressMode::ClampToEdge;
            sd.address_v   = rhi::AddressMode::ClampToEdge;
            sd.address_w   = rhi::AddressMode::ClampToEdge;
            sd.mipmap_mode = rhi::MipmapMode::Nearest;
            sd.max_lod     = 0.0f;
            m_fog_texture.sampler = m_rhi->create_sampler(sd);
        }

        // Initial transition to SHADER_READ_ONLY (will transition to TRANSFER_DST each frame)
        rhi::CommandList cmd = m_rhi->begin_oneshot();
        rhi::ImageBarrier b{};
        b.image      = m_fog_texture.texture;
        b.src_stage  = rhi::PipelineStage::TopOfPipe;
        b.dst_stage  = rhi::PipelineStage::FragmentShader;
        b.dst_access = rhi::AccessFlag::ShaderRead;
        b.old_layout = rhi::ImageLayout::Undefined;
        b.new_layout = rhi::ImageLayout::ShaderReadOnlyOptimal;
        cmd.image_barrier(b);
        m_rhi->end_oneshot(cmd);

        // Re-create terrain descriptor set with fog texture bound
        if (m_terrain_material.layer_count > 0) {
            m_terrain_material.descriptor_set = allocate_terrain_descriptor(m_terrain_material);
        }
    }

    // Convert float brightness → RGBA staging buffer
    if (auto* dst = static_cast<u8*>(m_rhi->mapped_ptr(m_fog_staging_buffer))) {
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

void Renderer::upload_fog(rhi::CommandList& cmd) {
    if (!m_fog_dirty || !m_fog_texture.texture.is_valid() || !m_fog_staging_buffer.is_valid()) return;

    rhi::ImageBarrier to_transfer{};
    to_transfer.image       = m_fog_texture.texture;
    to_transfer.src_stage   = rhi::PipelineStage::FragmentShader;
    to_transfer.src_access  = rhi::AccessFlag::ShaderRead;
    to_transfer.dst_stage   = rhi::PipelineStage::Transfer;
    to_transfer.dst_access  = rhi::AccessFlag::TransferWrite;
    to_transfer.old_layout  = rhi::ImageLayout::ShaderReadOnlyOptimal;
    to_transfer.new_layout  = rhi::ImageLayout::TransferDstOptimal;
    cmd.image_barrier(to_transfer);

    rhi::BufferImageCopy region{};
    region.image_extent_w = m_fog_width;
    region.image_extent_h = m_fog_height;
    cmd.copy_buffer_to_image(m_fog_staging_buffer, m_fog_texture.texture, std::span{&region, 1});

    rhi::ImageBarrier to_shader{};
    to_shader.image      = m_fog_texture.texture;
    to_shader.src_stage  = rhi::PipelineStage::Transfer;
    to_shader.src_access = rhi::AccessFlag::TransferWrite;
    to_shader.dst_stage  = rhi::PipelineStage::FragmentShader;
    to_shader.dst_access = rhi::AccessFlag::ShaderRead;
    to_shader.old_layout = rhi::ImageLayout::TransferDstOptimal;
    to_shader.new_layout = rhi::ImageLayout::ShaderReadOnlyOptimal;
    cmd.image_barrier(to_shader);

    m_fog_dirty = false;
}

bool Renderer::is_in_fog_memory(const simulation::World& world, u32 id) const {
    if (!m_simulation) return false;
    if (!simulation::is_static_remembered_entity(world, id)) return false;
    // Remembered entity is in fog memory iff its tile is Explored
    // (which the renderer reaches because is_fog_hidden returned false)
    // but NOT currently visible.
    return !m_simulation->vision().is_unit_visible_to(
        world, *m_simulation, id, simulation::Player{m_local_player_id},
        /*remembered_ok=*/false);
}

bool Renderer::is_fog_hidden(const simulation::World& world, u32 id, const simulation::Transform& t) const {
    // Client-side cull is *defense in depth*. The server-side network
    // snapshot path is the primary line against cheating — the client
    // shouldn't even receive data for units it can't see (network.cpp
    // uses the same is_unit_visible_to). Single source of truth keeps
    // the two paths in lockstep.
    (void)t;  // Vision::is_unit_visible_to reads transform itself
    if (!m_simulation) return false;
    const bool remembered_ok = simulation::is_static_remembered_entity(world, id);
    return !m_simulation->vision().is_unit_visible_to(
        world, *m_simulation, id, simulation::Player{m_local_player_id},
        remembered_ok);
}

// ── Descriptor set layouts + pool ─────────────────────────────────────────

bool Renderer::create_descriptor_layouts() {
    // Mesh descriptor set layout: 1 combined image sampler at binding 0
    {
        rhi::DescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.type    = rhi::DescriptorType::CombinedImageSampler;
        b.count   = 1;
        b.stages  = rhi::ShaderStage::Fragment;
        rhi::DescriptorSetLayoutDesc desc{};
        desc.bindings = std::span{&b, 1};
        m_mesh_desc_layout = m_rhi->create_descriptor_set_layout(desc);
        if (!m_mesh_desc_layout.is_valid()) {
            log::error(TAG, "Failed to create mesh descriptor set layout");
            return false;
        }
    }

    // Terrain descriptor set layout: layers + fog + noise + normals + water
    {
        rhi::DescriptorSetLayoutBinding bindings[5]{};
        for (u32 i = 0; i < 5; ++i) {
            bindings[i].binding = i;
            bindings[i].type    = rhi::DescriptorType::CombinedImageSampler;
            bindings[i].count   = 1;
            bindings[i].stages  = rhi::ShaderStage::Fragment;
        }
        rhi::DescriptorSetLayoutDesc desc{};
        desc.bindings = std::span{bindings, 5};
        m_terrain_desc_layout = m_rhi->create_descriptor_set_layout(desc);
        if (!m_terrain_desc_layout.is_valid()) {
            log::error(TAG, "Failed to create terrain descriptor set layout");
            return false;
        }
    }

    // Global lighting descriptor set layout (set 1):
    //   binding 0: shadow UBO (mat4 light_vp)
    //   binding 1: shadow map sampler
    //   binding 2: environment UBO (sun, ambient, fog)
    //   binding 3: environment cubemap (skybox / fallback)
    {
        rhi::DescriptorSetLayoutBinding bindings[4]{};
        bindings[0].binding = 0; bindings[0].type = rhi::DescriptorType::UniformBuffer;
        bindings[1].binding = 1; bindings[1].type = rhi::DescriptorType::CombinedImageSampler;
        bindings[2].binding = 2; bindings[2].type = rhi::DescriptorType::UniformBuffer;
        bindings[3].binding = 3; bindings[3].type = rhi::DescriptorType::CombinedImageSampler;
        for (auto& b : bindings) { b.count = 1; b.stages = rhi::ShaderStage::Fragment; }
        rhi::DescriptorSetLayoutDesc desc{};
        desc.bindings = std::span{bindings, 4};
        m_shadow_desc_layout = m_rhi->create_descriptor_set_layout(desc);
        if (!m_shadow_desc_layout.is_valid()) {
            log::error(TAG, "Failed to create shadow descriptor set layout");
            return false;
        }
    }

    // Bone SSBO descriptor set layout (set 2 for skinned mesh, set 0 for skinned shadow)
    {
        rhi::DescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.type    = rhi::DescriptorType::StorageBuffer;
        b.count   = 1;
        b.stages  = rhi::ShaderStage::Vertex;
        rhi::DescriptorSetLayoutDesc desc{};
        desc.bindings = std::span{&b, 1};
        m_bone_desc_layout = m_rhi->create_descriptor_set_layout(desc);
        if (!m_bone_desc_layout.is_valid()) {
            log::error(TAG, "Failed to create bone descriptor set layout");
            return false;
        }
    }

    log::info(TAG, "Descriptor layouts created");
    return true;
}

rhi::DescriptorSetHandle Renderer::allocate_mesh_descriptor(const GpuTexture& diffuse) {
    rhi::DescriptorSetHandle set = m_rhi->allocate_descriptor_set(m_mesh_desc_layout);
    if (!set.is_valid()) return {};

    rhi::WriteDescriptor w{};
    w.binding = 0;
    w.type    = rhi::DescriptorType::CombinedImageSampler;
    w.texture = diffuse.texture;
    w.sampler = diffuse.sampler;
    m_rhi->update_descriptor_set(set, std::span{&w, 1});
    return set;
}

rhi::DescriptorSetHandle Renderer::allocate_terrain_descriptor(const TerrainMaterial& mat) {
    rhi::DescriptorSetHandle set = m_rhi->allocate_descriptor_set(m_terrain_desc_layout);
    if (!set.is_valid()) return {};

    const GpuTexture& layer_tex  = mat.layer_array.texture.is_valid() ? mat.layer_array : m_default_texture;
    const GpuTexture& fog_tex    = m_fog_texture.texture.is_valid() ? m_fog_texture : m_default_texture;
    const GpuTexture& mask_tex   = m_transition_noise.texture.is_valid() ? m_transition_noise : m_default_texture;
    const GpuTexture& norm_tex   = mat.normal_array.texture.is_valid() ? mat.normal_array : m_default_texture;
    const GpuTexture& water_norm = m_water_normal.texture.is_valid() ? m_water_normal : m_default_texture;

    rhi::WriteDescriptor ws[5]{};
    for (auto& w : ws) { w.type = rhi::DescriptorType::CombinedImageSampler; }
    ws[0].binding = 0; ws[0].texture = layer_tex.texture;  ws[0].sampler = layer_tex.sampler;
    ws[1].binding = 1; ws[1].texture = fog_tex.texture;    ws[1].sampler = fog_tex.sampler;
    ws[2].binding = 2; ws[2].texture = mask_tex.texture;   ws[2].sampler = mask_tex.sampler;
    ws[3].binding = 3; ws[3].texture = norm_tex.texture;   ws[3].sampler = norm_tex.sampler;
    ws[4].binding = 4; ws[4].texture = water_norm.texture; ws[4].sampler = water_norm.sampler;
    m_rhi->update_descriptor_set(set, std::span{ws, 5});
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
    gpu.owns_buffers  = false;  // slice of the shared mega buffer
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

    auto* vb_dst = static_cast<u8*>(m_rhi->mapped_ptr(m_mega_vb))
                 + m_mega_vb_used * sizeof(asset::Vertex);
    std::memcpy(vb_dst, mesh.vertices.data(), vc * sizeof(asset::Vertex));
    m_mega_vb_used += vc;

    auto* ib_dst = static_cast<u8*>(m_rhi->mapped_ptr(m_mega_ib))
                 + m_mega_ib_used * sizeof(u32);
    std::memcpy(ib_dst, mesh.indices.data(), ic * sizeof(u32));
    m_mega_ib_used += ic;

    return gpu;
}

// ── Bindless texture array (Phase 14b) ───────────────────────────────────

bool Renderer::create_bindless_resources() {
#if defined(ULDUM_BACKEND_GLES)
    // GLES has no bindless sampler[] array (EXT_bindless_texture is rare on
    // Android). Use a single sampler2DArray of fixed-size layers instead.
    rhi::DescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.type    = rhi::DescriptorType::CombinedImageSampler;
    b.count   = 1;
    b.stages  = rhi::ShaderStage::Fragment;
    rhi::DescriptorSetLayoutDesc desc{};
    desc.bindings = std::span{&b, 1};
    m_bindless_layout = m_rhi->create_descriptor_set_layout(desc);
    if (!m_bindless_layout.is_valid()) {
        log::error(TAG, "Failed to create unit-texture-array descriptor set layout");
        return false;
    }
    m_bindless_set = m_rhi->allocate_descriptor_set(m_bindless_layout);
    if (!m_bindless_set.is_valid()) {
        log::error(TAG, "Failed to allocate unit-texture-array descriptor set");
        return false;
    }

    rhi::TextureDesc td{};
    td.width        = UNIT_TEX_SIZE;
    td.height       = UNIT_TEX_SIZE;
    td.array_layers = UNIT_TEX_LAYERS;
    td.format       = rhi::TextureFormat::R8G8B8A8_SRGB;
    td.usage        = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    m_unit_tex_array = m_rhi->create_texture(td);
    if (!m_unit_tex_array.is_valid()) {
        log::error(TAG, "Failed to create unit texture array ({}x{}x{} SRGB)",
                   UNIT_TEX_SIZE, UNIT_TEX_SIZE, UNIT_TEX_LAYERS);
        return false;
    }

    rhi::SamplerDesc sd{};
    sd.address_u = rhi::AddressMode::ClampToEdge;
    sd.address_v = rhi::AddressMode::ClampToEdge;
    sd.address_w = rhi::AddressMode::ClampToEdge;
    sd.max_lod   = 0.0f;
    m_unit_tex_sampler = m_rhi->create_sampler(sd);
    if (!m_unit_tex_sampler.is_valid()) {
        log::error(TAG, "Failed to create unit texture array sampler");
        return false;
    }

    rhi::WriteDescriptor w{};
    w.binding       = 0;
    w.array_element = 0;
    w.type          = rhi::DescriptorType::CombinedImageSampler;
    w.texture       = m_unit_tex_array;
    w.sampler       = m_unit_tex_sampler;
    m_rhi->update_descriptor_set(m_bindless_set, std::span{&w, 1});

    log::info(TAG, "Unit texture array created ({}x{} x {} layers)",
              UNIT_TEX_SIZE, UNIT_TEX_SIZE, UNIT_TEX_LAYERS);
    return true;
#else
    // Vulkan: descriptor set layout: variable-size sampler2D array
    rhi::DescriptorSetLayoutBinding b{};
    b.binding             = 0;
    b.type                = rhi::DescriptorType::CombinedImageSampler;
    b.count               = MAX_BINDLESS_TEXTURES;
    b.stages              = rhi::ShaderStage::Fragment;
    b.partially_bound     = true;
    b.variable_count      = true;
    b.update_after_bind   = true;

    rhi::DescriptorSetLayoutDesc desc{};
    desc.bindings = std::span{&b, 1};

    m_bindless_layout = m_rhi->create_descriptor_set_layout(desc);
    if (!m_bindless_layout.is_valid()) {
        log::error(TAG, "Failed to create bindless descriptor set layout");
        return false;
    }

    m_bindless_set = m_rhi->allocate_descriptor_set(m_bindless_layout, MAX_BINDLESS_TEXTURES);
    if (!m_bindless_set.is_valid()) {
        log::error(TAG, "Failed to allocate bindless descriptor set");
        return false;
    }

    log::info(TAG, "Bindless texture array created (max {} textures)", MAX_BINDLESS_TEXTURES);
    return true;
#endif
}

u32 Renderer::register_unit_texture(const GpuTexture& tex, const u8* pixels, u32 width, u32 height) {
#if defined(ULDUM_BACKEND_GLES)
    (void)tex;
    if (m_bindless_count >= UNIT_TEX_LAYERS) {
        log::error(TAG, "Unit texture array full ({}/{})", m_bindless_count, UNIT_TEX_LAYERS);
        return 0;
    }
    if (!pixels || width == 0 || height == 0) {
        log::error(TAG, "register_unit_texture: missing pixel data");
        return 0;
    }
    u32 idx = m_bindless_count++;

    // Resize to UNIT_TEX_SIZE × UNIT_TEX_SIZE so the layer fits the array.
    // stb_image_resize2 is a single-call bilinear/Mitchell resize. Source
    // is RGBA8 SRGB.
    std::vector<u8> resized;
    const u8* upload_pixels = pixels;
    if (width != UNIT_TEX_SIZE || height != UNIT_TEX_SIZE) {
        resized.resize(static_cast<usize>(UNIT_TEX_SIZE) * UNIT_TEX_SIZE * 4);
        stbir_resize_uint8_srgb(pixels,        static_cast<int>(width),         static_cast<int>(height),         0,
                                 resized.data(), static_cast<int>(UNIT_TEX_SIZE), static_cast<int>(UNIT_TEX_SIZE), 0,
                                 STBIR_RGBA);
        upload_pixels = resized.data();
    }

    // Stage via a one-shot upload buffer + copy_buffer_to_image.
    const u64 layer_bytes = static_cast<u64>(UNIT_TEX_SIZE) * UNIT_TEX_SIZE * 4;
    rhi::BufferDesc bd{};
    bd.size   = layer_bytes;
    bd.usage  = rhi::BufferUsage::TransferSrc;
    bd.memory = rhi::MemoryUsage::HostSequential;
    auto staging = m_rhi->create_buffer(bd);
    if (!staging.is_valid()) {
        log::error(TAG, "register_unit_texture: staging buffer alloc failed");
        return 0;
    }
    std::memcpy(m_rhi->mapped_ptr(staging), upload_pixels, layer_bytes);

    rhi::CommandList cmd = m_rhi->begin_oneshot();
    rhi::ImageBarrier to_xfer{};
    to_xfer.image       = m_unit_tex_array;
    to_xfer.src_stage   = rhi::PipelineStage::TopOfPipe;
    to_xfer.dst_stage   = rhi::PipelineStage::Transfer;
    to_xfer.dst_access  = rhi::AccessFlag::TransferWrite;
    to_xfer.old_layout  = rhi::ImageLayout::Undefined;
    to_xfer.new_layout  = rhi::ImageLayout::TransferDstOptimal;
    to_xfer.base_layer  = idx;
    to_xfer.layer_count = 1;
    cmd.image_barrier(to_xfer);

    rhi::BufferImageCopy region{};
    region.image_extent_w   = UNIT_TEX_SIZE;
    region.image_extent_h   = UNIT_TEX_SIZE;
    region.base_array_layer = idx;
    region.layer_count      = 1;
    cmd.copy_buffer_to_image(staging, m_unit_tex_array, std::span{&region, 1});

    rhi::ImageBarrier to_shader{};
    to_shader.image       = m_unit_tex_array;
    to_shader.src_stage   = rhi::PipelineStage::Transfer;
    to_shader.src_access  = rhi::AccessFlag::TransferWrite;
    to_shader.dst_stage   = rhi::PipelineStage::FragmentShader;
    to_shader.dst_access  = rhi::AccessFlag::ShaderRead;
    to_shader.old_layout  = rhi::ImageLayout::TransferDstOptimal;
    to_shader.new_layout  = rhi::ImageLayout::ShaderReadOnlyOptimal;
    to_shader.base_layer  = idx;
    to_shader.layer_count = 1;
    cmd.image_barrier(to_shader);
    m_rhi->end_oneshot(cmd);

    m_rhi->destroy_buffer(staging);
    return idx;
#else
    (void)pixels; (void)width; (void)height;
    if (m_bindless_count >= MAX_BINDLESS_TEXTURES) {
        log::error(TAG, "Bindless texture array full ({}/{})", m_bindless_count, MAX_BINDLESS_TEXTURES);
        return 0;
    }
    u32 idx = m_bindless_count++;
    rhi::WriteDescriptor w{};
    w.binding       = 0;
    w.array_element = idx;
    w.type          = rhi::DescriptorType::CombinedImageSampler;
    w.texture       = tex.texture;
    w.sampler       = tex.sampler;
    m_rhi->update_descriptor_set(m_bindless_set, std::span{&w, 1});
    return idx;
#endif
}

// ── Default + terrain textures ────────────────────────────────────────────

bool Renderer::create_default_texture() {
    // 4x4 warm orange texture for placeholder meshes
    auto pixels = generate_solid_texture(4, 220, 160, 80);
    m_default_texture = upload_texture_rgba(*m_rhi, pixels.data(), 4, 4);
    if (!m_default_texture.texture.is_valid()) return false;

    m_default_material.diffuse = m_default_texture;
    m_default_material.descriptor_set = allocate_mesh_descriptor(m_default_texture);

    // Register in bindless (Vulkan) / unit-texture-array (GLES)
    m_default_tex_idx = register_unit_texture(m_default_texture, pixels.data(), 4, 4);

    // Corpse texture (dark gray)
    auto corpse_pixels = generate_solid_texture(4, 50, 50, 50);
    m_corpse_texture = upload_texture_rgba(*m_rhi, corpse_pixels.data(), 4, 4);
    if (!m_corpse_texture.texture.is_valid()) return false;
    m_corpse_material.diffuse = m_corpse_texture;
    m_corpse_material.descriptor_set = allocate_mesh_descriptor(m_corpse_texture);

    m_corpse_tex_idx = register_unit_texture(m_corpse_texture, corpse_pixels.data(), 4, 4);

    log::info(TAG, "Default + corpse textures created (bindless idx: {}, {})",
              m_default_tex_idx, m_corpse_tex_idx);
    return true;
}

bool Renderer::create_terrain_textures() {
    // Create a default 1-layer terrain array (used before any map tileset is loaded)
    constexpr u32 TEX_SIZE = 32;
    auto fallback = generate_solid_texture(TEX_SIZE, 100, 100, 100);
    const u8* layer_data[] = {fallback.data()};
    m_terrain_material.layer_array = upload_texture_array(*m_rhi, layer_data, 1, TEX_SIZE, TEX_SIZE);
    m_terrain_material.layer_count = 1;
    log::info(TAG, "Default terrain texture created (1 layer)");
    return m_terrain_material.layer_array.texture.is_valid();
}

// Simple hash noise for procedural texture generation
static f32 hash_noise(u32 x, u32 y) {
    u32 h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return static_cast<f32>(h & 0xFFFF) / 65535.0f;
}

bool Renderer::create_transition_noise() {
    constexpr u32 SIZE = 128;
    std::vector<u8> pixels(SIZE * SIZE * 4);

    for (u32 y = 0; y < SIZE; ++y) {
        for (u32 x = 0; x < SIZE; ++x) {
            // Multi-octave noise for organic variation
            f32 v = hash_noise(x, y) * 0.4f
                  + hash_noise(x / 3, y / 3) * 0.35f
                  + hash_noise(x / 7, y / 7) * 0.25f;
            v = std::clamp(v, 0.0f, 1.0f);
            u8 b = static_cast<u8>(v * 255.0f);
            u32 i = (y * SIZE + x) * 4;
            pixels[i] = b; pixels[i+1] = b; pixels[i+2] = b; pixels[i+3] = 255;
        }
    }

    m_transition_noise = upload_texture_rgba(*m_rhi, pixels.data(), SIZE, SIZE);
    if (!m_transition_noise.texture.is_valid()) {
        log::error(TAG, "Failed to create transition noise texture");
        return false;
    }
    log::info(TAG, "Transition noise texture created ({}x{})", SIZE, SIZE);
    return true;
}

bool Renderer::create_water_normal() {
    constexpr u32 SIZE = 128;

    // Generate tileable height field from multi-octave hash noise
    std::vector<f32> heights(SIZE * SIZE);
    for (u32 y = 0; y < SIZE; ++y) {
        for (u32 x = 0; x < SIZE; ++x) {
            // Wrapping coordinates for tileability
            f32 v = hash_noise(x % SIZE, y % SIZE) * 0.5f
                  + hash_noise((x * 2) % SIZE, (y * 2) % SIZE) * 0.3f
                  + hash_noise((x * 4) % SIZE, (y * 4) % SIZE) * 0.2f;
            heights[y * SIZE + x] = v;
        }
    }

    // Convert height field to normal map via finite differences
    std::vector<u8> pixels(SIZE * SIZE * 4);
    constexpr f32 strength = 1.5f;
    for (u32 y = 0; y < SIZE; ++y) {
        for (u32 x = 0; x < SIZE; ++x) {
            f32 hL = heights[y * SIZE + (x + SIZE - 1) % SIZE];
            f32 hR = heights[y * SIZE + (x + 1) % SIZE];
            f32 hD = heights[((y + SIZE - 1) % SIZE) * SIZE + x];
            f32 hU = heights[((y + 1) % SIZE) * SIZE + x];
            f32 dx = (hL - hR) * strength;
            f32 dy = (hD - hU) * strength;
            f32 dz = 1.0f;
            f32 len = std::sqrt(dx * dx + dy * dy + dz * dz);
            dx /= len; dy /= len; dz /= len;
            u32 i = (y * SIZE + x) * 4;
            pixels[i]     = static_cast<u8>(std::clamp((dx * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            pixels[i + 1] = static_cast<u8>(std::clamp((dy * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            pixels[i + 2] = static_cast<u8>(std::clamp((dz * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            pixels[i + 3] = 255;
        }
    }

    m_water_normal = upload_texture_rgba(*m_rhi, pixels.data(), SIZE, SIZE);
    if (!m_water_normal.texture.is_valid()) {
        log::error(TAG, "Failed to create water normal map");
        return false;
    }
    log::info(TAG, "Water normal map created ({}x{})", SIZE, SIZE);
    return true;
}

void Renderer::load_tileset_textures(const map::Tileset& tileset) {
    m_rhi->wait_idle();
    if (m_terrain_material.layer_array.texture.is_valid()) destroy_texture(*m_rhi, m_terrain_material.layer_array);
    if (m_terrain_material.normal_array.texture.is_valid()) destroy_texture(*m_rhi, m_terrain_material.normal_array);
    m_terrain_material.has_normals = false;

    if (tileset.layers.empty()) {
        create_terrain_textures();
        return;
    }

    // Procedural fallback colors per layer (used when diffuse texture is missing)
    static const struct { u8 r, g, b; } fallback_colors[] = {
        {60, 140, 40},   // 0: grass green
        {140, 100, 50},  // 1: dirt brown
        {130, 130, 120}, // 2: stone gray
        {30, 70, 150},   // 3: water blue
        {180, 170, 130}, // 4: sand beige
        {200, 200, 200}, // 5: snow white
        {80, 60, 40},    // 6: mud dark brown
        {100, 100, 100}, // 7: default gray
    };

    constexpr u32 TEX_SIZE = 256;
    u32 layer_count = static_cast<u32>(tileset.layers.size());
    std::vector<std::vector<u8>> layer_pixels(layer_count);
    std::vector<const u8*> layer_ptrs(layer_count);

    for (u32 i = 0; i < layer_count; ++i) {
        auto& tl = tileset.layers[i];

        // Try loading diffuse texture from map package
        bool loaded = false;
        if (!tl.diffuse_path.empty()) {
            std::string abs_path = m_map_root + "/" + tl.diffuse_path;
            auto* mgr = asset::AssetManager::instance();
            auto bytes = mgr ? mgr->read_file_bytes(abs_path) : std::vector<u8>{};
            auto tex_result = bytes.empty()
                ? std::expected<asset::TextureData, std::string>(std::unexpect, "not in package")
                : asset::load_texture_from_memory(bytes.data(), static_cast<u32>(bytes.size()));
            if (tex_result) {
                auto& tex_data = *tex_result;
                // Resize to TEX_SIZE if needed (simple: just use as-is if same size,
                // or generate fallback if different — proper resize is a future improvement)
                if (tex_data.width == TEX_SIZE && tex_data.height == TEX_SIZE && tex_data.channels == 4) {
                    layer_pixels[i] = std::move(tex_data.pixels);
                    loaded = true;
                    log::info(TAG, "Loaded terrain texture '{}' for layer {} ({})",
                              tl.diffuse_path, tl.id, tl.name);
                } else {
                    // Convert to RGBA and use (even if size differs — array requires uniform size)
                    // For now, fall back to procedural if size doesn't match
                    log::warn(TAG, "Terrain texture '{}' size {}x{} != {}x{}, using fallback",
                              tl.diffuse_path, tex_data.width, tex_data.height, TEX_SIZE, TEX_SIZE);
                }
            }
        }

        if (!loaded) {
            // Water layers: use tileset color as fallback
            if (tl.type == map::LayerType::WaterShallow || tl.type == map::LayerType::WaterDeep) {
                u8 r = static_cast<u8>(std::clamp(tl.water_color.x * 255.0f, 0.0f, 255.0f));
                u8 g = static_cast<u8>(std::clamp(tl.water_color.y * 255.0f, 0.0f, 255.0f));
                u8 b = static_cast<u8>(std::clamp(tl.water_color.z * 255.0f, 0.0f, 255.0f));
                layer_pixels[i] = generate_solid_texture(TEX_SIZE, r, g, b);
            } else {
                u32 ci = std::min(i, static_cast<u32>(std::size(fallback_colors) - 1));
                layer_pixels[i] = generate_solid_texture(TEX_SIZE, fallback_colors[ci].r,
                                                         fallback_colors[ci].g, fallback_colors[ci].b);
            }
        }

        layer_ptrs[i] = layer_pixels[i].data();
    }

    m_terrain_material.layer_array = upload_texture_array(*m_rhi, layer_ptrs.data(),
                                                          layer_count, TEX_SIZE, TEX_SIZE);
    m_terrain_material.layer_count = layer_count;

    // Load normal maps (optional — layers without normals get a flat default)
    {
        // Flat normal: (0.5, 0.5, 1.0) in tangent space = straight up
        std::vector<u8> flat_normal(TEX_SIZE * TEX_SIZE * 4);
        for (u32 p = 0; p < TEX_SIZE * TEX_SIZE; ++p) {
            flat_normal[p * 4]     = 128;  // X = 0
            flat_normal[p * 4 + 1] = 128;  // Y = 0
            flat_normal[p * 4 + 2] = 255;  // Z = 1 (up)
            flat_normal[p * 4 + 3] = 255;
        }

        std::vector<std::vector<u8>> normal_pixels(layer_count);
        std::vector<const u8*> normal_ptrs(layer_count);
        bool any_loaded = false;

        for (u32 i = 0; i < layer_count; ++i) {
            auto& tl = tileset.layers[i];
            bool loaded = false;

            if (!tl.normal_path.empty()) {
                std::string abs_path = m_map_root + "/" + tl.normal_path;
                auto* mgr = asset::AssetManager::instance();
                auto bytes = mgr ? mgr->read_file_bytes(abs_path) : std::vector<u8>{};
                auto tex_result = bytes.empty()
                    ? std::expected<asset::TextureData, std::string>(std::unexpect, "not in package")
                    : asset::load_texture_from_memory(bytes.data(), static_cast<u32>(bytes.size()));
                if (tex_result) {
                    auto& tex_data = *tex_result;
                    if (tex_data.width == TEX_SIZE && tex_data.height == TEX_SIZE && tex_data.channels == 4) {
                        normal_pixels[i] = std::move(tex_data.pixels);
                        loaded = true;
                        any_loaded = true;
                        log::info(TAG, "Loaded normal map '{}' for layer {} ({})",
                                  tl.normal_path, tl.id, tl.name);
                    } else {
                        log::warn(TAG, "Normal map '{}' size {}x{} != {}x{}, using flat",
                                  tl.normal_path, tex_data.width, tex_data.height, TEX_SIZE, TEX_SIZE);
                    }
                }
            }

            if (!loaded) {
                normal_pixels[i] = flat_normal;
            }
            normal_ptrs[i] = normal_pixels[i].data();
        }

        if (any_loaded) {
            m_terrain_material.normal_array = upload_texture_array(*m_rhi, normal_ptrs.data(),
                                                                    layer_count, TEX_SIZE, TEX_SIZE, false);
            m_terrain_material.has_normals = true;
            log::info(TAG, "Terrain normal maps loaded");
        }
    }

    // Compute water layer masks for water surface rendering
    m_water_params = {};
    for (u32 i = 0; i < layer_count; ++i) {
        auto& tl = tileset.layers[i];
        if (tl.type == map::LayerType::WaterShallow) {
            m_water_params.water_mask |= (1u << tl.id);
            m_water_params.shallow_color = tl.water_color;
        } else if (tl.type == map::LayerType::WaterDeep) {
            m_water_params.water_mask |= (1u << tl.id);
            m_water_params.deep_mask  |= (1u << tl.id);
            m_water_params.deep_color = tl.water_color;
        }
    }

    log::info(TAG, "Tileset '{}' textures loaded — {} layers (water_mask=0x{:X})",
              tileset.name, layer_count, m_water_params.water_mask);
}

// ── Pipeline creation helpers ─────────────────────────────────────────────


bool Renderer::create_mesh_pipeline() {
    auto vert_h = load_shader(*m_rhi, "engine/shaders/mesh.vert.spv");
    auto frag_h = load_shader(*m_rhi, "engine/shaders/mesh.frag.spv");
    if (!vert_h.is_valid() || !frag_h.is_valid()) {
        log::error(TAG, "Failed to load mesh shaders");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::PushConstantRange pc{};
    pc.stages = rhi::ShaderStage::Vertex;
    pc.size   = sizeof(glm::mat4);  // just vp

    rhi::DescriptorSetLayoutHandle set_layouts[] = { m_bindless_layout, m_shadow_desc_layout, m_bone_desc_layout };
    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.set_layouts    = std::span{set_layouts, 3};
    pl_desc.push_constants = std::span{&pc, 1};
    m_mesh_pipeline_layout = m_rhi->create_pipeline_layout(pl_desc);
    if (!m_mesh_pipeline_layout.is_valid()) {
        log::error(TAG, "Failed to create mesh pipeline layout");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::ShaderStageDesc stages[2]{};
    stages[0].stage = rhi::ShaderStage::Vertex;   stages[0].module = vert_h;
    stages[1].stage = rhi::ShaderStage::Fragment; stages[1].module = frag_h;

    rhi::VertexBindingDesc binding{ 0, sizeof(asset::Vertex), false };
    rhi::VertexAttributeDesc attrs[3]{
        { 0, 0, offsetof(asset::Vertex, position), rhi::TextureFormat::R32G32B32_SFLOAT },
        { 1, 0, offsetof(asset::Vertex, normal),   rhi::TextureFormat::R32G32B32_SFLOAT },
        { 2, 0, offsetof(asset::Vertex, texcoord), rhi::TextureFormat::R32G32_SFLOAT },
    };
    rhi::VertexInputDesc vi{};
    vi.bindings   = std::span{&binding, 1};
    vi.attributes = std::span{attrs, 3};

    rhi::RasterizerState rs{};
    rs.cull_mode  = rhi::CullMode::Back;
    rs.front_face = rhi::FrontFace::CounterClockwise;

    rhi::DepthStencilState ds{};
    ds.depth_test_enable  = true;
    ds.depth_write_enable = true;
    ds.depth_compare      = rhi::CompareOp::Less;

    // Alpha blending so InstanceData.alpha (driven by visual_alpha)
    // shows up on screen. Opaque instances (alpha=1) match the pre-blend
    // pipeline; translucent ones blend.
    rhi::BlendAttachmentState ba{};
    ba.blend_enable     = true;
    ba.src_color_factor = rhi::BlendFactor::SrcAlpha;
    ba.dst_color_factor = rhi::BlendFactor::OneMinusSrcAlpha;
    ba.src_alpha_factor = rhi::BlendFactor::One;
    ba.dst_alpha_factor = rhi::BlendFactor::Zero;

    rhi::MultisampleState ms{};
    ms.sample_count = static_cast<u32>(m_rhi->msaa_samples());

    rhi::TextureFormat color_fmt = m_rhi->swapchain_format();
    rhi::TextureFormat depth_fmt = m_rhi->depth_format();

    rhi::GraphicsPipelineDesc desc{};
    desc.layout            = m_mesh_pipeline_layout;
    desc.stages            = std::span{stages, 2};
    desc.vertex_input      = vi;
    desc.topology          = rhi::PrimitiveTopology::TriangleList;
    desc.rasterizer        = rs;
    desc.depth_stencil     = ds;
    desc.blend_attachments = std::span{&ba, 1};
    desc.multisample       = ms;
    desc.render.color_formats = std::span{&color_fmt, 1};
    desc.render.depth_format  = depth_fmt;

    m_mesh_pipeline = m_rhi->create_graphics_pipeline(desc);
    m_rhi->destroy_shader_module(vert_h);
    m_rhi->destroy_shader_module(frag_h);
    if (!m_mesh_pipeline.is_valid()) {
        log::error(TAG, "Failed to create mesh pipeline");
        return false;
    }
    log::info(TAG, "Mesh pipeline created (textured + shadow)");
    return true;
}

bool Renderer::create_skinned_mesh_pipeline() {
    auto vert_h = load_shader(*m_rhi, "engine/shaders/skinned_mesh.vert.spv");
    auto frag_h = load_shader(*m_rhi, "engine/shaders/skinned_mesh.frag.spv");
    if (!vert_h.is_valid() || !frag_h.is_valid()) {
        log::error(TAG, "Failed to load skinned mesh shaders");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    // Push constants: vertex = mat4 mvp + mat4 model, fragment = vec4 visual (alpha)
    rhi::PushConstantRange pcs[2]{};
    pcs[0].stages = rhi::ShaderStage::Vertex;
    pcs[0].offset = 0;
    pcs[0].size   = 2 * sizeof(glm::mat4);
    pcs[1].stages = rhi::ShaderStage::Fragment;
    pcs[1].offset = 2 * sizeof(glm::mat4);
    pcs[1].size   = sizeof(glm::vec4);

    rhi::DescriptorSetLayoutHandle set_layouts[] = { m_mesh_desc_layout, m_shadow_desc_layout, m_bone_desc_layout };
    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.set_layouts    = std::span{set_layouts, 3};
    pl_desc.push_constants = std::span{pcs, 2};
    m_skinned_mesh_pipeline_layout = m_rhi->create_pipeline_layout(pl_desc);
    if (!m_skinned_mesh_pipeline_layout.is_valid()) {
        log::error(TAG, "Failed to create skinned mesh pipeline layout");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::ShaderStageDesc stages[2]{};
    stages[0].stage = rhi::ShaderStage::Vertex;   stages[0].module = vert_h;
    stages[1].stage = rhi::ShaderStage::Fragment; stages[1].module = frag_h;

    rhi::VertexBindingDesc binding{ 0, sizeof(asset::SkinnedVertex), false };
    rhi::VertexAttributeDesc attrs[5]{
        { 0, 0, offsetof(asset::SkinnedVertex, position),     rhi::TextureFormat::R32G32B32_SFLOAT },
        { 1, 0, offsetof(asset::SkinnedVertex, normal),       rhi::TextureFormat::R32G32B32_SFLOAT },
        { 2, 0, offsetof(asset::SkinnedVertex, texcoord),     rhi::TextureFormat::R32G32_SFLOAT },
        { 3, 0, offsetof(asset::SkinnedVertex, bone_indices), rhi::TextureFormat::R32G32B32A32_UINT },
        { 4, 0, offsetof(asset::SkinnedVertex, bone_weights), rhi::TextureFormat::R32G32B32A32_SFLOAT },
    };
    rhi::VertexInputDesc vi{};
    vi.bindings   = std::span{&binding, 1};
    vi.attributes = std::span{attrs, 5};

    rhi::RasterizerState rs{};
    rs.cull_mode  = rhi::CullMode::Back;
    rs.front_face = rhi::FrontFace::CounterClockwise;

    rhi::DepthStencilState ds{};
    ds.depth_test_enable  = true;
    ds.depth_write_enable = true;
    ds.depth_compare      = rhi::CompareOp::Less;

    // Alpha blending for SetUnitAlpha / ghost-rendering. Depth write stays
    // on — fine for mostly-opaque humanoid silhouettes; switch to dithered
    // alpha if we ever need clean see-through ghosts.
    rhi::BlendAttachmentState ba{};
    ba.blend_enable     = true;
    ba.src_color_factor = rhi::BlendFactor::SrcAlpha;
    ba.dst_color_factor = rhi::BlendFactor::OneMinusSrcAlpha;
    ba.src_alpha_factor = rhi::BlendFactor::One;
    ba.dst_alpha_factor = rhi::BlendFactor::Zero;

    rhi::MultisampleState ms{};
    ms.sample_count = static_cast<u32>(m_rhi->msaa_samples());

    rhi::TextureFormat color_fmt = m_rhi->swapchain_format();
    rhi::TextureFormat depth_fmt = m_rhi->depth_format();

    rhi::GraphicsPipelineDesc desc{};
    desc.layout            = m_skinned_mesh_pipeline_layout;
    desc.stages            = std::span{stages, 2};
    desc.vertex_input      = vi;
    desc.topology          = rhi::PrimitiveTopology::TriangleList;
    desc.rasterizer        = rs;
    desc.depth_stencil     = ds;
    desc.blend_attachments = std::span{&ba, 1};
    desc.multisample       = ms;
    desc.render.color_formats = std::span{&color_fmt, 1};
    desc.render.depth_format  = depth_fmt;

    m_skinned_mesh_pipeline = m_rhi->create_graphics_pipeline(desc);
    m_rhi->destroy_shader_module(vert_h);
    m_rhi->destroy_shader_module(frag_h);
    if (!m_skinned_mesh_pipeline.is_valid()) {
        log::error(TAG, "Failed to create skinned mesh pipeline");
        return false;
    }

    // Skinned shadow pipeline — depth-only, set 0 = bones SSBO, push = mat4 light_mvp
    auto shadow_vert_h = load_shader(*m_rhi, "engine/shaders/skinned_shadow.vert.spv");
    if (shadow_vert_h.is_valid()) {
        rhi::PushConstantRange spc{};
        spc.stages = rhi::ShaderStage::Vertex;
        spc.size   = sizeof(glm::mat4);

        rhi::PipelineLayoutDesc spl{};
        spl.set_layouts    = std::span{&m_bone_desc_layout, 1};
        spl.push_constants = std::span{&spc, 1};
        m_skinned_shadow_pipeline_layout = m_rhi->create_pipeline_layout(spl);

        rhi::ShaderStageDesc shadow_stages[1]{};
        shadow_stages[0].stage  = rhi::ShaderStage::Vertex;
        shadow_stages[0].module = shadow_vert_h;

        rhi::RasterizerState shadow_rs = rs;
        shadow_rs.depth_bias_enable          = true;
        shadow_rs.depth_bias_constant_factor = 1.25f;
        shadow_rs.depth_bias_slope_factor    = 1.75f;

        rhi::MultisampleState shadow_ms{};
        shadow_ms.sample_count = 1;  // shadow is 1x

        rhi::GraphicsPipelineDesc sdesc{};
        sdesc.layout            = m_skinned_shadow_pipeline_layout;
        sdesc.stages            = std::span{shadow_stages, 1};
        sdesc.vertex_input      = vi;
        sdesc.topology          = rhi::PrimitiveTopology::TriangleList;
        sdesc.rasterizer        = shadow_rs;
        sdesc.depth_stencil     = ds;
        sdesc.multisample       = shadow_ms;
        sdesc.render.depth_format = depth_fmt;
        // no color attachments → blend_attachments empty

        m_skinned_shadow_pipeline = m_rhi->create_graphics_pipeline(sdesc);
        m_rhi->destroy_shader_module(shadow_vert_h);
    }

    log::info(TAG, "Skinned mesh pipeline created (textured + shadow + bone SSBO)");
    return true;
}

bool Renderer::create_particle_pipeline() {
    auto vert_h = load_shader(*m_rhi, "engine/shaders/particle.vert.spv");
    auto frag_h = load_shader(*m_rhi, "engine/shaders/particle.frag.spv");
    if (!vert_h.is_valid() || !frag_h.is_valid()) {
        log::error(TAG, "Failed to load particle shaders");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::PushConstantRange pc{};
    pc.stages = rhi::ShaderStage::Vertex;
    pc.size   = sizeof(glm::mat4);

    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.push_constants = std::span{&pc, 1};
    m_particle_pipeline_layout = m_rhi->create_pipeline_layout(pl_desc);
    if (!m_particle_pipeline_layout.is_valid()) {
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::ShaderStageDesc stages[2]{};
    stages[0].stage = rhi::ShaderStage::Vertex;   stages[0].module = vert_h;
    stages[1].stage = rhi::ShaderStage::Fragment; stages[1].module = frag_h;

    rhi::VertexBindingDesc binding{ 0, sizeof(ParticleVertex), false };
    rhi::VertexAttributeDesc attrs[4]{
        { 0, 0, offsetof(ParticleVertex, position),   rhi::TextureFormat::R32G32B32_SFLOAT },
        { 1, 0, offsetof(ParticleVertex, color),      rhi::TextureFormat::R32G32B32A32_SFLOAT },
        { 2, 0, offsetof(ParticleVertex, texcoord),   rhi::TextureFormat::R32G32_SFLOAT },
        { 3, 0, offsetof(ParticleVertex, texture_id), rhi::TextureFormat::R32_UINT },
    };
    rhi::VertexInputDesc vi{};
    vi.bindings   = std::span{&binding, 1};
    vi.attributes = std::span{attrs, 4};

    rhi::RasterizerState rs{};
    rs.cull_mode = rhi::CullMode::None;  // particles are double-sided

    rhi::DepthStencilState ds{};
    ds.depth_test_enable  = true;
    ds.depth_write_enable = false;  // particles are transparent
    ds.depth_compare      = rhi::CompareOp::Less;

    rhi::BlendAttachmentState ba{};
    ba.blend_enable     = true;
    ba.src_color_factor = rhi::BlendFactor::SrcAlpha;
    ba.dst_color_factor = rhi::BlendFactor::OneMinusSrcAlpha;
    ba.src_alpha_factor = rhi::BlendFactor::One;
    ba.dst_alpha_factor = rhi::BlendFactor::Zero;

    rhi::MultisampleState ms{};
    ms.sample_count = static_cast<u32>(m_rhi->msaa_samples());

    rhi::TextureFormat color_fmt = m_rhi->swapchain_format();
    rhi::TextureFormat depth_fmt = m_rhi->depth_format();

    rhi::GraphicsPipelineDesc desc{};
    desc.layout            = m_particle_pipeline_layout;
    desc.stages            = std::span{stages, 2};
    desc.vertex_input      = vi;
    desc.rasterizer        = rs;
    desc.depth_stencil     = ds;
    desc.blend_attachments = std::span{&ba, 1};
    desc.multisample       = ms;
    desc.render.color_formats = std::span{&color_fmt, 1};
    desc.render.depth_format  = depth_fmt;

    m_particle_pipeline = m_rhi->create_graphics_pipeline(desc);
    m_rhi->destroy_shader_module(vert_h);
    m_rhi->destroy_shader_module(frag_h);
    if (!m_particle_pipeline.is_valid()) return false;

    log::info(TAG, "Particle pipeline created (alpha-blended)");
    return true;
}

bool Renderer::create_terrain_pipeline() {
    auto vert_h = load_shader(*m_rhi, "engine/shaders/terrain.vert.spv");
    auto frag_h = load_shader(*m_rhi, "engine/shaders/terrain.frag.spv");
    if (!vert_h.is_valid() || !frag_h.is_valid()) {
        log::error(TAG, "Failed to load terrain shaders");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::PushConstantRange pc{};
    pc.stages = rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment;
    pc.size   = 2 * sizeof(glm::mat4) + sizeof(glm::vec2) + 2 * sizeof(f32);

    rhi::DescriptorSetLayoutHandle set_layouts[] = { m_terrain_desc_layout, m_shadow_desc_layout };
    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.set_layouts    = std::span{set_layouts, 2};
    pl_desc.push_constants = std::span{&pc, 1};
    m_terrain_pipeline_layout = m_rhi->create_pipeline_layout(pl_desc);
    if (!m_terrain_pipeline_layout.is_valid()) {
        log::error(TAG, "Failed to create terrain pipeline layout");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::ShaderStageDesc stages[2]{};
    stages[0].stage = rhi::ShaderStage::Vertex;   stages[0].module = vert_h;
    stages[1].stage = rhi::ShaderStage::Fragment; stages[1].module = frag_h;

    rhi::VertexBindingDesc binding{ 0, sizeof(TerrainVertex), false };
    rhi::VertexAttributeDesc attrs[5]{
        { 0, 0, offsetof(TerrainVertex, position),      rhi::TextureFormat::R32G32B32_SFLOAT },
        { 1, 0, offsetof(TerrainVertex, normal),        rhi::TextureFormat::R32G32B32_SFLOAT },
        { 2, 0, offsetof(TerrainVertex, texcoord),      rhi::TextureFormat::R32G32_SFLOAT },
        { 3, 0, offsetof(TerrainVertex, layer_corners), rhi::TextureFormat::R32_UINT },
        { 4, 0, offsetof(TerrainVertex, case_info),     rhi::TextureFormat::R32_UINT },
    };
    rhi::VertexInputDesc vi{};
    vi.bindings   = std::span{&binding, 1};
    vi.attributes = std::span{attrs, 5};

    rhi::RasterizerState rs{};
    rs.cull_mode  = rhi::CullMode::Back;
    rs.front_face = rhi::FrontFace::CounterClockwise;

    rhi::DepthStencilState ds{};
    ds.depth_test_enable  = true;
    ds.depth_write_enable = true;
    ds.depth_compare      = rhi::CompareOp::Less;

    rhi::BlendAttachmentState ba{};  // opaque

    rhi::MultisampleState ms{};
    ms.sample_count = static_cast<u32>(m_rhi->msaa_samples());

    rhi::TextureFormat color_fmt = m_rhi->swapchain_format();
    rhi::TextureFormat depth_fmt = m_rhi->depth_format();

    rhi::GraphicsPipelineDesc desc{};
    desc.layout            = m_terrain_pipeline_layout;
    desc.stages            = std::span{stages, 2};
    desc.vertex_input      = vi;
    desc.rasterizer        = rs;
    desc.depth_stencil     = ds;
    desc.blend_attachments = std::span{&ba, 1};
    desc.multisample       = ms;
    desc.render.color_formats = std::span{&color_fmt, 1};
    desc.render.depth_format  = depth_fmt;

    m_terrain_pipeline = m_rhi->create_graphics_pipeline(desc);
    m_rhi->destroy_shader_module(vert_h);
    m_rhi->destroy_shader_module(frag_h);
    if (!m_terrain_pipeline.is_valid()) return false;

    log::info(TAG, "Terrain pipeline created (splatmap + shadow)");
    return true;
}

bool Renderer::create_water_pipeline() {
    auto vert_h = load_shader(*m_rhi, "engine/shaders/water.vert.spv");
    auto frag_h = load_shader(*m_rhi, "engine/shaders/water.frag.spv");
    if (!vert_h.is_valid() || !frag_h.is_valid()) {
        log::error(TAG, "Failed to load water shaders");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    // Water push constants: terrain base (144) + water params (40) + pad (8) + camera_pos (16) = 208
    rhi::PushConstantRange pc{};
    pc.stages = rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment;
    pc.size   = 2 * sizeof(glm::mat4) + sizeof(glm::vec2) + 2 * sizeof(f32)
              + 2 * sizeof(glm::vec4) + 2 * sizeof(u32)
              + 2 * sizeof(u32) + sizeof(glm::vec4);

    rhi::DescriptorSetLayoutHandle set_layouts[] = { m_terrain_desc_layout, m_shadow_desc_layout };
    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.set_layouts    = std::span{set_layouts, 2};
    pl_desc.push_constants = std::span{&pc, 1};
    m_water_pipeline_layout = m_rhi->create_pipeline_layout(pl_desc);
    if (!m_water_pipeline_layout.is_valid()) {
        log::error(TAG, "Failed to create water pipeline layout");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::ShaderStageDesc stages[2]{};
    stages[0].stage = rhi::ShaderStage::Vertex;   stages[0].module = vert_h;
    stages[1].stage = rhi::ShaderStage::Fragment; stages[1].module = frag_h;

    // Same vertex layout as terrain
    rhi::VertexBindingDesc binding{ 0, sizeof(TerrainVertex), false };
    rhi::VertexAttributeDesc attrs[5]{
        { 0, 0, offsetof(TerrainVertex, position),      rhi::TextureFormat::R32G32B32_SFLOAT },
        { 1, 0, offsetof(TerrainVertex, normal),        rhi::TextureFormat::R32G32B32_SFLOAT },
        { 2, 0, offsetof(TerrainVertex, texcoord),      rhi::TextureFormat::R32G32_SFLOAT },
        { 3, 0, offsetof(TerrainVertex, layer_corners), rhi::TextureFormat::R32_UINT },
        { 4, 0, offsetof(TerrainVertex, case_info),     rhi::TextureFormat::R32_UINT },
    };
    rhi::VertexInputDesc vi{};
    vi.bindings   = std::span{&binding, 1};
    vi.attributes = std::span{attrs, 5};

    rhi::RasterizerState rs{};
    rs.cull_mode  = rhi::CullMode::Back;
    rs.front_face = rhi::FrontFace::CounterClockwise;

    rhi::DepthStencilState ds{};
    ds.depth_test_enable  = true;
    ds.depth_write_enable = false;  // don't occlude things behind water
    ds.depth_compare      = rhi::CompareOp::Less;

    rhi::BlendAttachmentState ba{};
    ba.blend_enable     = true;
    ba.src_color_factor = rhi::BlendFactor::SrcAlpha;
    ba.dst_color_factor = rhi::BlendFactor::OneMinusSrcAlpha;
    ba.src_alpha_factor = rhi::BlendFactor::One;
    ba.dst_alpha_factor = rhi::BlendFactor::OneMinusSrcAlpha;

    rhi::MultisampleState ms{};
    ms.sample_count = static_cast<u32>(m_rhi->msaa_samples());

    rhi::TextureFormat color_fmt = m_rhi->swapchain_format();
    rhi::TextureFormat depth_fmt = m_rhi->depth_format();

    rhi::GraphicsPipelineDesc desc{};
    desc.layout            = m_water_pipeline_layout;
    desc.stages            = std::span{stages, 2};
    desc.vertex_input      = vi;
    desc.rasterizer        = rs;
    desc.depth_stencil     = ds;
    desc.blend_attachments = std::span{&ba, 1};
    desc.multisample       = ms;
    desc.render.color_formats = std::span{&color_fmt, 1};
    desc.render.depth_format  = depth_fmt;

    m_water_pipeline = m_rhi->create_graphics_pipeline(desc);
    m_rhi->destroy_shader_module(vert_h);
    m_rhi->destroy_shader_module(frag_h);
    if (!m_water_pipeline.is_valid()) return false;

    log::info(TAG, "Water surface pipeline created");
    return true;
}

bool Renderer::create_skybox_mesh() {
    // Unit cube: 8 corners, 36 indices (12 triangles)
    static const float verts[] = {
        -1, -1, -1,   1, -1, -1,   1,  1, -1,  -1,  1, -1,
        -1, -1,  1,   1, -1,  1,   1,  1,  1,  -1,  1,  1,
    };
    static const u32 idx[] = {
        0,1,2, 2,3,0,  // -Z
        4,6,5, 6,4,7,  // +Z
        0,4,5, 5,1,0,  // -Y
        2,6,7, 7,3,2,  // +Y
        0,3,7, 7,4,0,  // -X
        1,5,6, 6,2,1,  // +X
    };

    {
        rhi::BufferDesc d{};
        d.size   = sizeof(verts);
        d.usage  = rhi::BufferUsage::Vertex;
        d.memory = rhi::MemoryUsage::HostSequential;
        m_skybox_mesh.vertex_buffer = m_rhi->create_buffer(d);
        if (void* dst = m_rhi->mapped_ptr(m_skybox_mesh.vertex_buffer)) {
            std::memcpy(dst, verts, sizeof(verts));
        }
    }
    {
        rhi::BufferDesc d{};
        d.size   = sizeof(idx);
        d.usage  = rhi::BufferUsage::Index;
        d.memory = rhi::MemoryUsage::HostSequential;
        m_skybox_mesh.index_buffer = m_rhi->create_buffer(d);
        if (void* dst = m_rhi->mapped_ptr(m_skybox_mesh.index_buffer)) {
            std::memcpy(dst, idx, sizeof(idx));
        }
    }

    m_skybox_mesh.index_count = 36;
    m_skybox_mesh.vertex_count = 8;
    return true;
}

bool Renderer::create_skybox_pipeline() {
    auto vert_h = load_shader(*m_rhi, "engine/shaders/skybox.vert.spv");
    auto frag_h = load_shader(*m_rhi, "engine/shaders/skybox.frag.spv");
    if (!vert_h.is_valid() || !frag_h.is_valid()) {
        log::error(TAG, "Failed to load skybox shaders");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    {
        rhi::DescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.type    = rhi::DescriptorType::CombinedImageSampler;
        b.count   = 1;
        b.stages  = rhi::ShaderStage::Fragment;
        rhi::DescriptorSetLayoutDesc d{};
        d.bindings = std::span{&b, 1};
        m_skybox_desc_layout = m_rhi->create_descriptor_set_layout(d);
        if (!m_skybox_desc_layout.is_valid()) {
            log::error(TAG, "Failed to create skybox descriptor set layout");
            m_rhi->destroy_shader_module(vert_h);
            m_rhi->destroy_shader_module(frag_h);
            return false;
        }
    }

    rhi::PushConstantRange pc{};
    pc.stages = rhi::ShaderStage::Vertex;
    pc.size   = sizeof(glm::mat4);

    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.set_layouts    = std::span{&m_skybox_desc_layout, 1};
    pl_desc.push_constants = std::span{&pc, 1};
    m_skybox_pipeline_layout = m_rhi->create_pipeline_layout(pl_desc);
    if (!m_skybox_pipeline_layout.is_valid()) {
        log::error(TAG, "Failed to create skybox pipeline layout");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::ShaderStageDesc stages[2]{};
    stages[0].stage = rhi::ShaderStage::Vertex;   stages[0].module = vert_h;
    stages[1].stage = rhi::ShaderStage::Fragment; stages[1].module = frag_h;

    // Vertex input: vec3 position only
    rhi::VertexBindingDesc binding{ 0, sizeof(float) * 3, false };
    rhi::VertexAttributeDesc attr{ 0, 0, 0, rhi::TextureFormat::R32G32B32_SFLOAT };
    rhi::VertexInputDesc vi{};
    vi.bindings   = std::span{&binding, 1};
    vi.attributes = std::span{&attr, 1};

    rhi::RasterizerState rs{};
    rs.cull_mode = rhi::CullMode::None;  // inside cube — render all faces

    rhi::DepthStencilState ds{};
    ds.depth_test_enable  = true;
    ds.depth_write_enable = true;
    ds.depth_compare      = rhi::CompareOp::LessEqual;  // z=1 far plane, terrain overwrites with LESS

    rhi::BlendAttachmentState ba{};  // opaque

    rhi::MultisampleState ms{};
    ms.sample_count = static_cast<u32>(m_rhi->msaa_samples());

    rhi::TextureFormat color_fmt = m_rhi->swapchain_format();
    rhi::TextureFormat depth_fmt = m_rhi->depth_format();

    rhi::GraphicsPipelineDesc desc{};
    desc.layout            = m_skybox_pipeline_layout;
    desc.stages            = std::span{stages, 2};
    desc.vertex_input      = vi;
    desc.rasterizer        = rs;
    desc.depth_stencil     = ds;
    desc.blend_attachments = std::span{&ba, 1};
    desc.multisample       = ms;
    desc.render.color_formats = std::span{&color_fmt, 1};
    desc.render.depth_format  = depth_fmt;

    m_skybox_pipeline = m_rhi->create_graphics_pipeline(desc);
    m_rhi->destroy_shader_module(vert_h);
    m_rhi->destroy_shader_module(frag_h);
    if (!m_skybox_pipeline.is_valid()) return false;

    log::info(TAG, "Skybox pipeline created");
    return true;
}

f32 Renderer::clip_duration(std::string_view model_path, std::string_view clip_name) {
    auto* lm = get_or_load_model(std::string(model_path));
    if (!lm) return 0.0f;
    i32 idx = find_clip_by_name(lm->data, clip_name);
    if (idx < 0 || idx >= (i32)lm->data.animations.size()) return 0.0f;
    return lm->data.animations[idx].duration;
}

// ── Model loading + mesh cache ────────────────────────────────────────────

LoadedModel* Renderer::get_or_load_model(const std::string& model_path) {
    // Check cache first
    auto it = m_model_cache.find(model_path);
    if (it != m_model_cache.end()) return &it->second;

    // Don't retry paths that already failed
    if (m_model_failed.contains(model_path)) return nullptr;

    // Path is what the author put in JSON — passed straight to the
    // AssetManager. Mount prefixes do the lookup; no path massaging
    // so the engine doesn't impose a folder convention on author
    // content.
    auto* mgr = asset::AssetManager::instance();
    if (!mgr) {
        log::warn(TAG, "Model load without AssetManager: '{}'", model_path);
        m_model_failed.insert(model_path);
        return nullptr;
    }

    auto bytes = mgr->read_file_bytes(model_path);
    if (bytes.empty()) {
        log::warn(TAG, "Model not found: '{}'", model_path);
        m_model_failed.insert(model_path);
        return nullptr;
    }

    auto result = asset::load_model_from_memory(bytes.data(), static_cast<u32>(bytes.size()), model_path);
    if (!result) {
        log::error(TAG, "Failed to load model '{}': {}", model_path, result.error());
        m_model_failed.insert(model_path);
        return nullptr;
    }

    LoadedModel lm;
    lm.data = std::move(*result);

    // Merge all meshes into one and upload
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
        lm.mesh = upload_skinned_mesh(*m_rhi, merged, bone_count);
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
        // Register in bindless (Vulkan) / unit-texture-array (GLES) for the
        // static mesh pipeline.
        lm.texture_index = register_unit_texture(lm.diffuse_texture,
                                                 lm.data.textures[0].pixels.data(),
                                                 lm.data.textures[0].width,
                                                 lm.data.textures[0].height);
        log::info(TAG, "  Texture: {}x{} (unit array idx: {})",
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
    // glTF Y-up → game Z-up
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
    auto vert_h = load_shader(*m_rhi, "engine/shaders/shadow.vert.spv");
    auto frag_h = load_shader(*m_rhi, "engine/shaders/shadow.frag.spv");
    if (!vert_h.is_valid() || !frag_h.is_valid()) {
        log::error(TAG, "Failed to load shadow shaders");
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::PushConstantRange pc{};
    pc.stages = rhi::ShaderStage::Vertex;
    pc.size   = sizeof(glm::mat4);

    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.set_layouts    = std::span{&m_bone_desc_layout, 1};
    pl_desc.push_constants = std::span{&pc, 1};
    m_shadow_pipeline_layout = m_rhi->create_pipeline_layout(pl_desc);
    if (!m_shadow_pipeline_layout.is_valid()) {
        m_rhi->destroy_shader_module(vert_h);
        m_rhi->destroy_shader_module(frag_h);
        return false;
    }

    rhi::ShaderStageDesc stages[2]{};
    stages[0].stage = rhi::ShaderStage::Vertex;   stages[0].module = vert_h;
    stages[1].stage = rhi::ShaderStage::Fragment; stages[1].module = frag_h;

    rhi::VertexBindingDesc binding{ 0, sizeof(asset::Vertex), false };
    rhi::VertexAttributeDesc attrs[3]{
        { 0, 0, offsetof(asset::Vertex, position), rhi::TextureFormat::R32G32B32_SFLOAT },
        { 1, 0, offsetof(asset::Vertex, normal),   rhi::TextureFormat::R32G32B32_SFLOAT },
        { 2, 0, offsetof(asset::Vertex, texcoord), rhi::TextureFormat::R32G32_SFLOAT },
    };
    rhi::VertexInputDesc vi{};
    vi.bindings   = std::span{&binding, 1};
    vi.attributes = std::span{attrs, 3};

    rhi::RasterizerState rs{};
    rs.cull_mode                 = rhi::CullMode::Back;
    rs.front_face                = rhi::FrontFace::CounterClockwise;
    rs.depth_bias_enable         = true;
    rs.depth_bias_constant_factor = 2.0f;
    rs.depth_bias_slope_factor    = 1.5f;

    rhi::DepthStencilState ds{};
    ds.depth_test_enable  = true;
    ds.depth_write_enable = true;
    ds.depth_compare      = rhi::CompareOp::Less;

    rhi::MultisampleState ms{};
    ms.sample_count = 1;

    rhi::TextureFormat depth_fmt = rhi::TextureFormat::D32_SFLOAT;

    rhi::GraphicsPipelineDesc desc{};
    desc.layout            = m_shadow_pipeline_layout;
    desc.stages            = std::span{stages, 2};
    desc.vertex_input      = vi;
    desc.rasterizer        = rs;
    desc.depth_stencil     = ds;
    desc.multisample       = ms;
    desc.render.depth_format = depth_fmt;
    // no color attachments

    m_shadow_pipeline = m_rhi->create_graphics_pipeline(desc);
    m_rhi->destroy_shader_module(vert_h);
    m_rhi->destroy_shader_module(frag_h);
    if (!m_shadow_pipeline.is_valid()) return false;

    // Terrain shadow pipeline: same as above but with TerrainVertex layout
    auto terrain_vert_h = load_shader(*m_rhi, "engine/shaders/terrain_shadow.vert.spv");
    auto terrain_frag_h = load_shader(*m_rhi, "engine/shaders/shadow.frag.spv");
    if (terrain_vert_h.is_valid()) {
        rhi::ShaderStageDesc tstages[2]{};
        tstages[0].stage = rhi::ShaderStage::Vertex;   tstages[0].module = terrain_vert_h;
        tstages[1].stage = rhi::ShaderStage::Fragment; tstages[1].module = terrain_frag_h;

        // Shadow pass only reads vertex position. Listing fewer attrs
        // than the binding stride covers is fine — the shader never
        // touches the remaining bytes. Keeps validation clean by
        // matching the shader's location-0-only declaration.
        rhi::VertexBindingDesc tbinding{ 0, sizeof(TerrainVertex), false };
        rhi::VertexAttributeDesc tattr{ 0, 0, offsetof(TerrainVertex, position), rhi::TextureFormat::R32G32B32_SFLOAT };
        rhi::VertexInputDesc tvi{};
        tvi.bindings   = std::span{&tbinding, 1};
        tvi.attributes = std::span{&tattr, 1};

        // Reuse shadow pipeline layout (same push constant)
        m_terrain_shadow_pipeline_layout = m_shadow_pipeline_layout;

        rhi::GraphicsPipelineDesc tdesc{};
        tdesc.layout            = m_terrain_shadow_pipeline_layout;
        tdesc.stages            = std::span{tstages, 2};
        tdesc.vertex_input      = tvi;
        tdesc.rasterizer        = rs;
        tdesc.depth_stencil     = ds;
        tdesc.multisample       = ms;
        tdesc.render.depth_format = depth_fmt;

        m_terrain_shadow_pipeline = m_rhi->create_graphics_pipeline(tdesc);
    }
    m_rhi->destroy_shader_module(terrain_vert_h);
    m_rhi->destroy_shader_module(terrain_frag_h);

    log::info(TAG, "Shadow pipelines created (depth-only)");
    return true;
}

bool Renderer::create_shadow_resources() {
    if (!create_shadow_map(*m_rhi, m_shadow_map)) return false;
    if (!create_shadow_buffer(*m_rhi, m_shadow_ubo)) return false;

    // Create environment UBO (persistently mapped)
    {
        rhi::BufferDesc d{};
        d.size   = sizeof(EnvironmentUBO);
        d.usage  = rhi::BufferUsage::Uniform;
        d.memory = rhi::MemoryUsage::HostSequential;
        m_env_ubo_buffer = m_rhi->create_buffer(d);
        if (!m_env_ubo_buffer.is_valid()) {
            log::error(TAG, "Failed to create environment UBO");
            return false;
        }
        EnvironmentUBO defaults{};
        std::memcpy(m_rhi->mapped_ptr(m_env_ubo_buffer), &defaults, sizeof(defaults));
    }

    // Create default 1x1 cubemap (gray, used when no skybox loaded)
    {
        u8 gray[4] = {128, 128, 140, 255};
        const u8* faces[6] = {gray, gray, gray, gray, gray, gray};
        m_default_cubemap = upload_texture_cubemap(*m_rhi, faces, 1, 1);
        if (!m_default_cubemap.texture.is_valid()) {
            log::error(TAG, "Failed to create default cubemap");
            return false;
        }
    }

    m_shadow_desc_set = allocate_shadow_descriptor();
    return m_shadow_desc_set.is_valid();
}

rhi::DescriptorSetHandle Renderer::allocate_shadow_descriptor() {
    if (m_shadow_desc_set.is_valid()) m_rhi->free_descriptor_set(m_shadow_desc_set);
    rhi::DescriptorSetHandle set = m_rhi->allocate_descriptor_set(m_shadow_desc_layout);
    if (!set.is_valid()) return {};

    const GpuTexture& cube_tex = (m_has_skybox && m_skybox_cubemap.texture.is_valid()) ? m_skybox_cubemap : m_default_cubemap;

    rhi::WriteDescriptor ws[4]{};
    ws[0].binding = 0;
    ws[0].type    = rhi::DescriptorType::UniformBuffer;
    ws[0].buffer  = m_shadow_ubo.buffer;
    ws[0].buffer_range = sizeof(ShadowUBO);

    ws[1].binding      = 1;
    ws[1].type         = rhi::DescriptorType::CombinedImageSampler;
    ws[1].texture      = m_shadow_map.depth_image;
    ws[1].sampler      = m_shadow_map.sampler;
    ws[1].image_layout = rhi::WriteImageLayout::DepthStencilReadOnly;

    ws[2].binding = 2;
    ws[2].type    = rhi::DescriptorType::UniformBuffer;
    ws[2].buffer  = m_env_ubo_buffer;
    ws[2].buffer_range = sizeof(EnvironmentUBO);

    ws[3].binding = 3;
    ws[3].type    = rhi::DescriptorType::CombinedImageSampler;
    ws[3].texture = cube_tex.texture;
    ws[3].sampler = cube_tex.sampler;

    m_rhi->update_descriptor_set(set, std::span{ws, 4});
    log::info(TAG, "Shadow descriptor set allocated");
    return set;
}

rhi::DescriptorSetHandle Renderer::allocate_bone_descriptor(rhi::BufferHandle bone_buffer, usize size) {
    rhi::DescriptorSetHandle set = m_rhi->allocate_descriptor_set(m_bone_desc_layout);
    if (!set.is_valid()) return {};

    rhi::WriteDescriptor w{};
    w.binding      = 0;
    w.type         = rhi::DescriptorType::StorageBuffer;
    w.buffer       = bone_buffer;
    w.buffer_range = size;
    m_rhi->update_descriptor_set(set, std::span{&w, 1});
    return set;
}

// ── Shadow depth pass ─────────────────────────────────────────────────────

void Renderer::draw_shadow_pass(rhi::CommandList& cmd, simulation::World& world, f32 alpha) {
    if (!m_shadow_pipeline.is_valid()) return;

    glm::vec3 light_dir = m_sun_direction;
    // World is centered on (0, 0). Shadow frustum is anchored at map center;
    // radius is generous enough to wrap the biggest current map (test_map's
    // 8192×8192 has ±4096 extent). TODO: derive scene_center/radius from the
    // actual terrain bounds when terrain is set.
    glm::vec3 scene_center{0.0f, 0.0f, 160.0f};
    f32 scene_radius = 5120.0f;
    glm::mat4 light_vp = compute_light_vp(light_dir, scene_center, scene_radius);

    ShadowUBO ubo{light_vp};
    std::memcpy(m_rhi->mapped_ptr(m_shadow_ubo.buffer), &ubo, sizeof(ubo));

    rhi::ImageBarrier to_depth{};
    to_depth.image      = m_shadow_map.depth_image;
    to_depth.src_stage  = rhi::PipelineStage::FragmentShader;
    to_depth.src_access = rhi::AccessFlag::ShaderRead;
    to_depth.dst_stage  = rhi::PipelineStage::EarlyFragmentTests | rhi::PipelineStage::LateFragmentTests;
    to_depth.dst_access = rhi::AccessFlag::DepthStencilAttachmentWrite;
    to_depth.old_layout = rhi::ImageLayout::Undefined;
    to_depth.new_layout = rhi::ImageLayout::DepthAttachmentOptimal;
    to_depth.aspect     = rhi::ImageAspect::Depth;
    cmd.image_barrier(to_depth);

    rhi::DepthAttachment depth_att{};
    depth_att.image  = m_shadow_map.depth_image;
    depth_att.layout = rhi::ImageLayout::DepthAttachmentOptimal;
    depth_att.load   = rhi::LoadOp::Clear;
    depth_att.store  = rhi::StoreOp::Store;
    depth_att.clear  = { 1.0f, 0 };

    rhi::RenderingDesc rdesc{};
    rdesc.depth       = &depth_att;
    rdesc.area_width  = m_shadow_map.size;
    rdesc.area_height = m_shadow_map.size;
    cmd.begin_rendering(rdesc);

    f32 size_f = static_cast<f32>(m_shadow_map.size);
    cmd.bind_pipeline(m_shadow_pipeline);
    cmd.set_viewport(0, 0, size_f, size_f);
    cmd.set_scissor(0, 0, m_shadow_map.size, m_shadow_map.size);

    // Terrain shadow (uses terrain shadow pipeline with TerrainVertex stride)
    if (m_terrain_shadow_pipeline.is_valid() && m_terrain.gpu_mesh.vertex_buffer.is_valid()) {
        cmd.bind_pipeline(m_terrain_shadow_pipeline);
        cmd.set_viewport(0, 0, size_f, size_f);
        cmd.set_scissor(0, 0, m_shadow_map.size, m_shadow_map.size);

        glm::mat4 light_mvp = light_vp;
        cmd.push_constants(m_terrain_shadow_pipeline_layout, rhi::ShaderStage::Vertex,
                           0, sizeof(glm::mat4), &light_mvp);
        cmd.bind_vertex_buffer(0, m_terrain.gpu_mesh.vertex_buffer);
        cmd.bind_index_buffer(m_terrain.gpu_mesh.index_buffer, 0, rhi::IndexType::U32);
        cmd.draw_indexed(m_terrain.gpu_mesh.index_count);
    }

    // Entities — skinned units use skinned shadow pipeline, others use regular
    auto& transforms = world.transforms;
    auto& renderables = world.renderables;

    bool has_skinned_shadow = m_skinned_shadow_pipeline.is_valid();

    // Pass A: skinned units
    if (has_skinned_shadow) {
        cmd.bind_pipeline(m_skinned_shadow_pipeline);
        cmd.set_viewport(0, 0, size_f, size_f);
        cmd.set_scissor(0, 0, m_shadow_map.size, m_shadow_map.size);

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
            if (it == m_anim_instances.end() || !it->second.bone_descriptor.is_valid()) continue;

            // Upload bones (animation already evaluated in draw())
            auto& anim = it->second;
            if (void* dst = m_rhi->mapped_ptr(anim.bone_buffer);
                dst && !anim.bone_matrices.empty()) {
                std::memcpy(dst, anim.bone_matrices.data(),
                            anim.bone_matrices.size() * sizeof(glm::mat4));
            }

            cmd.bind_descriptor_set(m_skinned_shadow_pipeline_layout, 0, anim.bone_descriptor);

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
            cmd.push_constants(m_skinned_shadow_pipeline_layout, rhi::ShaderStage::Vertex,
                               0, sizeof(glm::mat4), &light_mvp);
            cmd.bind_vertex_buffer(0, lm->mesh.vertex_buffer);
            cmd.bind_index_buffer(lm->mesh.index_buffer, 0, rhi::IndexType::U32);
            cmd.draw_indexed(lm->mesh.index_count);
        }
    }

    // Pass B: non-skinned entities via indirect draw (Phase 14a)
    // Build instance batches (reused by main pass)
    build_static_draw_batches(world, alpha);

    if (m_shadow_pipeline.is_valid() && !m_draw_groups.empty()) {
        cmd.bind_pipeline(m_shadow_pipeline);
        cmd.set_viewport(0, 0, size_f, size_f);
        cmd.set_scissor(0, 0, m_shadow_map.size, m_shadow_map.size);

        cmd.push_constants(m_shadow_pipeline_layout, rhi::ShaderStage::Vertex,
                           0, sizeof(glm::mat4), &light_vp);

        // Bind mega VB/IB + instance SSBO once
        cmd.bind_vertex_buffer(0, m_mega_vb);
        cmd.bind_index_buffer(m_mega_ib, 0, rhi::IndexType::U32);
        const u32 fi = m_rhi->frame_index();
        cmd.bind_descriptor_set(m_shadow_pipeline_layout, 0, m_instance_desc_set[fi]);

        // Multi-draw indirect for all static mesh shadows
        u32 draw_count = static_cast<u32>(m_draw_groups.size());
        cmd.draw_indexed_indirect(m_indirect_buffer[fi], 0, draw_count,
                                  sizeof(rhi::DrawIndexedIndirectCommand));
    }

    cmd.end_rendering();

    // Transition shadow map for sampling
    rhi::ImageBarrier to_read{};
    to_read.image      = m_shadow_map.depth_image;
    to_read.src_stage  = rhi::PipelineStage::EarlyFragmentTests | rhi::PipelineStage::LateFragmentTests;
    to_read.src_access = rhi::AccessFlag::DepthStencilAttachmentWrite;
    to_read.dst_stage  = rhi::PipelineStage::FragmentShader;
    to_read.dst_access = rhi::AccessFlag::ShaderRead;
    to_read.old_layout = rhi::ImageLayout::DepthAttachmentOptimal;
    to_read.new_layout = rhi::ImageLayout::DepthStencilReadOnlyOptimal;
    to_read.aspect     = rhi::ImageAspect::Depth;
    cmd.image_barrier(to_read);
}

// ── Draw ───────────────────────────────────────────────────────────────────

void Renderer::draw_shadows(rhi::CommandList& cmd, simulation::World& world, f32 alpha) {
    draw_shadow_pass(cmd, world, alpha);
}

// ── Instance batching for static meshes (Phase 14a) ──────────────────────

void Renderer::build_static_draw_batches(const simulation::World& world, f32 alpha) {
    m_draw_groups.clear();
    m_static_instance_count = 0;

    auto& renderables = world.renderables;
    auto& transforms = world.transforms;


    bool has_skinned = m_skinned_mesh_pipeline.is_valid();
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
    // group_map: mesh geometry → index into per-group instance buckets.
    // Buckets are kept separate during traversal because entities iterate
    // in sparse-set dense order (which interleaves meshes when variations
    // alternate). Concatenating buckets at the end gives each draw group
    // a contiguous slice of the instance buffer — required by multi-draw
    // indirect (each command reads instances[firstInstance .. +count]).
    std::unordered_map<GroupKey, u32, GroupKeyHash> group_map;
    std::vector<std::vector<InstanceData>> buckets;
    buckets.reserve(8);

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
        if (!mesh.vertex_buffer.is_valid() || !mesh.index_buffer.is_valid()) continue;

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
        if (is_corpse && !(lm_static && lm_static->diffuse_texture.texture.is_valid())) {
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

        // Find or create draw group keyed by mesh geometry.
        GroupKey key{mesh.first_index, mesh.index_count,
                     static_cast<i32>(mesh.first_vertex)};
        auto git = group_map.find(key);
        u32 gi;
        if (git == group_map.end()) {
            gi = static_cast<u32>(m_draw_groups.size());
            group_map[key] = gi;
            DrawGroup dg{};
            dg.first_index    = mesh.first_index;
            dg.index_count    = mesh.index_count;
            dg.vertex_offset  = static_cast<i32>(mesh.first_vertex);
            dg.first_instance = 0;   // filled in after concatenation
            dg.instance_count = 0;
            m_draw_groups.push_back(dg);
            buckets.emplace_back();
        } else {
            gi = git->second;
        }

        InstanceData inst{};
        inst.model = model;
        inst.material_index = tex_idx;
        inst.alpha = effective_visual_alpha(world, id, renderable);
        if (is_in_fog_memory(world, id)) inst.alpha *= kFoggedMemoryAlpha;
        buckets[gi].push_back(inst);
    }

    // Concatenate buckets into the final instance buffer; record each
    // group's first_instance + instance_count from the contiguous slice.
    std::vector<InstanceData> instances;
    instances.reserve(256);
    for (u32 gi = 0; gi < m_draw_groups.size(); ++gi) {
        auto& dg = m_draw_groups[gi];
        dg.first_instance = static_cast<u32>(instances.size());
        dg.instance_count = static_cast<u32>(buckets[gi].size());
        instances.insert(instances.end(), buckets[gi].begin(), buckets[gi].end());
    }

    m_static_instance_count = static_cast<u32>(instances.size());

    const u32 fi = m_rhi->frame_index();

    if (m_static_instance_count > 0) {
        if (void* dst = m_rhi->mapped_ptr(m_instance_buffer[fi])) {
            u32 upload_count = std::min(m_static_instance_count, MAX_STATIC_INSTANCES);
            std::memcpy(dst, instances.data(), upload_count * sizeof(InstanceData));
        }
    }

    if (void* dst = m_rhi->mapped_ptr(m_indirect_buffer[fi]); dst && !m_draw_groups.empty()) {
        auto* cmds = static_cast<rhi::DrawIndexedIndirectCommand*>(dst);
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

void Renderer::draw(rhi::CommandList& cmd, rhi::Extent2D extent, simulation::World& world, f32 alpha,
                    const std::function<void()>& on_after_terrain) {
    if (extent.width == 0 || extent.height == 0) return;

    // Collect point lights from active glow particles
    {
        auto particles = m_particles.particle_data();
        for (auto& p : particles) {
            if (p.texture_id == ParticleSystem::SHAPE_GLOW && p.life > 0) {
                f32 life_frac = p.life / p.max_life;
                glm::vec3 color{p.start_color.r, p.start_color.g, p.start_color.b};
                add_point_light(p.position, color, p.size * 20.0f, 0.6f * life_frac);
            }
        }
    }

    // Flush point lights to environment UBO and reset for next frame
    if (void* env_mapped = m_rhi->mapped_ptr(m_env_ubo_buffer)) {
        std::memcpy(env_mapped, &m_env_data, sizeof(m_env_data));
        m_env_data.light_count.x = 0;
    }

    f32 vw = static_cast<f32>(extent.width);
    f32 vh = static_cast<f32>(extent.height);
    glm::mat4 vp = m_camera.view_projection();

    // ── Draw skybox (first, at far plane) ───────────────────────────────
    if (m_has_skybox && m_skybox_pipeline.is_valid() && m_skybox_desc_set.is_valid()) {
        cmd.bind_pipeline(m_skybox_pipeline);
        cmd.set_viewport(0, 0, vw, vh);
        cmd.set_scissor(0, 0, extent.width, extent.height);
        cmd.bind_descriptor_set(m_skybox_pipeline_layout, 0, m_skybox_desc_set);

        // Strip translation from view matrix so camera position doesn't affect skybox
        glm::mat4 view_no_translate = glm::mat4(glm::mat3(m_camera.view_matrix()));
        glm::mat4 skybox_vp = m_camera.projection_matrix() * view_no_translate;
        cmd.push_constants(m_skybox_pipeline_layout, rhi::ShaderStage::Vertex,
                           0, sizeof(glm::mat4), &skybox_vp);
        cmd.bind_vertex_buffer(0, m_skybox_mesh.vertex_buffer);
        cmd.bind_index_buffer(m_skybox_mesh.index_buffer, 0, rhi::IndexType::U32);
        cmd.draw_indexed(m_skybox_mesh.index_count);
    }

    // ── Draw terrain with splatmap pipeline ──────────────────────────────
    glm::mat4 terrain_model{1.0f};
    glm::mat4 terrain_mvp = vp * terrain_model;
    glm::vec2 world_size = m_terrain_data
        ? glm::vec2{m_terrain_data->world_width(), m_terrain_data->world_height()}
        : glm::vec2{1.0f};

    if (m_terrain_pipeline.is_valid() && m_terrain.gpu_mesh.vertex_buffer.is_valid() && m_terrain_material.descriptor_set.is_valid()) {
        cmd.bind_pipeline(m_terrain_pipeline);
        cmd.set_viewport(0, 0, vw, vh);
        cmd.set_scissor(0, 0, extent.width, extent.height);

        rhi::DescriptorSetHandle terrain_sets[] = { m_terrain_material.descriptor_set, m_shadow_desc_set };
        cmd.bind_descriptor_sets(m_terrain_pipeline_layout, 0, std::span{terrain_sets, 2});

        struct {
            glm::mat4 mvp;
            glm::mat4 model;
            glm::vec2 world_size;
            f32 fog_enabled;
            f32 time;
        } terrain_push{};
        terrain_push.mvp = terrain_mvp;
        terrain_push.model = terrain_model;
        terrain_push.world_size = world_size;
        terrain_push.fog_enabled = m_fog_enabled ? 1.0f : 0.0f;
        terrain_push.time = m_elapsed_time;
        cmd.push_constants(m_terrain_pipeline_layout,
                           rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment,
                           0, sizeof(terrain_push), &terrain_push);

        cmd.bind_vertex_buffer(0, m_terrain.gpu_mesh.vertex_buffer);
        cmd.bind_index_buffer(m_terrain.gpu_mesh.index_buffer, 0, rhi::IndexType::U32);
        cmd.draw_indexed(m_terrain.gpu_mesh.index_count);
    }

    // ── Draw water surface (transparent overlay on terrain) ─────────────
    if (m_water_pipeline.is_valid() && m_water_params.water_mask != 0 &&
        m_terrain.gpu_mesh.vertex_buffer.is_valid() && m_terrain_material.descriptor_set.is_valid()) {
        cmd.bind_pipeline(m_water_pipeline);
        cmd.set_viewport(0, 0, vw, vh);
        cmd.set_scissor(0, 0, extent.width, extent.height);

        rhi::DescriptorSetHandle terrain_sets[] = { m_terrain_material.descriptor_set, m_shadow_desc_set };
        cmd.bind_descriptor_sets(m_water_pipeline_layout, 0, std::span{terrain_sets, 2});

        struct {
            glm::mat4 mvp;
            glm::mat4 model;
            glm::vec2 world_size;
            f32 fog_enabled;
            f32 time;
            glm::vec4 shallow_color;
            glm::vec4 deep_color;
            u32 water_mask;
            u32 deep_mask;
            // Pad up to vec4 alignment for camera_pos (matches the std140-ish
            // layout the shader expects).
            u32 _pad0;
            u32 _pad1;
            glm::vec4 camera_pos;
        } water_push{};
        water_push.mvp = terrain_mvp;
        water_push.model = terrain_model;
        water_push.world_size = world_size;
        water_push.fog_enabled = m_fog_enabled ? 1.0f : 0.0f;
        water_push.time = m_elapsed_time;
        water_push.shallow_color = glm::vec4(m_water_params.shallow_color, 0.0f);
        water_push.deep_color = glm::vec4(m_water_params.deep_color, 0.0f);
        water_push.water_mask = m_water_params.water_mask;
        water_push.deep_mask = m_water_params.deep_mask;
        water_push.camera_pos = glm::vec4(m_camera.position(), 0.0f);
        cmd.push_constants(m_water_pipeline_layout,
                           rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment,
                           0, sizeof(water_push), &water_push);

        // Same mesh as terrain — water shader discards non-water fragments
        cmd.bind_vertex_buffer(0, m_terrain.gpu_mesh.vertex_buffer);
        cmd.bind_index_buffer(m_terrain.gpu_mesh.index_buffer, 0, rhi::IndexType::U32);
        cmd.draw_indexed(m_terrain.gpu_mesh.index_count);
    }

    // ── Ground decals (selection rings, focus reticles, ...) ────────────
    // Run AFTER terrain + water but BEFORE any unit mesh so meshes
    // composite over them. Without this ordering, alpha-blended units
    // (Wind Walk fade etc.) depth-occlude the ring underneath their
    // silhouette even though you can see through the body itself.
    if (on_after_terrain) on_after_terrain();

    // ── Draw entities ────────────────────────────────────────────────────
    if (!m_mesh_pipeline.is_valid()) return;

    auto& transforms = world.transforms;
    auto& renderables = world.renderables;


    // Determine frame dt for animation (approximate from last frame)
    static auto last_time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    f32 frame_dt = std::chrono::duration<f32>(now - last_time).count();
    frame_dt = std::min(frame_dt, 0.1f);  // clamp to avoid huge jumps
    last_time = now;
    m_elapsed_time += frame_dt;

    bool has_skinned_pipeline = m_skinned_mesh_pipeline.is_valid();

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
            m_rhi->wait_idle();
        }
        for (u32 eid : stale) {
            auto it = m_anim_instances.find(eid);
            if (it != m_anim_instances.end()) {
                m_rhi->destroy_buffer(it->second.bone_buffer);
                m_anim_instances.erase(it);
            }
        }
    }

    // Update animations for all skinned entities. Two visibility
    // filters keep this from melting CPU on big armies:
    //  • Fog-hidden  — the player can't see them, no point animating.
    //  • Off-screen  — same reason; the camera doesn't see them.
    // Both are cheap tests (tile lookup + bounding-sphere vs. frustum)
    // compared to a skeleton evaluation. When a hidden unit becomes
    // visible again, the next frame's eval brings its bones forward —
    // a single-frame "freeze" the player won't notice.
    if (has_skinned_pipeline) {
        auto cull_frustum = m_camera.frustum();
        for (u32 i = 0; i < renderables.count(); ++i) {
            u32 id = renderables.ids()[i];
            const auto& renderable = renderables.data()[i];

            auto* lm = get_or_load_model(renderable.model_path);
            if (!lm || !lm->is_skinned) continue;

            const auto* transform = transforms.get(id);
            if (!transform) continue;
            if (is_fog_hidden(world, id, *transform)) continue;
            f32 cull_radius = lm->mesh.bounding_radius * transform->scale;
            if (!cull_frustum.is_sphere_visible(transform->interp_position(alpha),
                                                cull_radius)) continue;

            auto& anim = get_or_create_anim(id, *lm);

            // Skip birth animation for revealed entities (not newly created)
            if (renderable.skip_birth && anim.current_state == AnimState::Birth) {
                anim.current_state = AnimState::Idle;
                anim.looping = true;
                anim.time = 0;
            }

            // Script-driven animation override (SetUnitAnimation /
            // QueueUnitAnimation). Death always wins — if the unit is
            // dying we drop the queue so the death clip plays cleanly.
            auto* aq = world.anim_queues.get(id);
            const bool is_dying = world.dead_states.has(id);
            if (aq && is_dying) {
                world.anim_queues.remove(id);
                aq = nullptr;
                anim.script_controlled = false;
                anim.script_clip_name.clear();
            }
            if (aq && !aq->clips.empty()) {
                // Resolve when entering script control, OR when the
                // front-of-queue clip differs from the one currently
                // bound (queue was swapped mid-play, e.g. projectile
                // moving from "idle" loop to "death").
                const bool clip_changed = anim.script_controlled &&
                                          aq->clips.front() != anim.script_clip_name;
                if (!anim.script_controlled || clip_changed) {
                    i32 idx = find_clip_by_name(*anim.model, aq->clips.front());
                    if (idx >= 0) {
                        anim.state_to_clip[static_cast<u8>(AnimState::Custom)] = idx;
                        anim.script_looping = (aq->clips.size() == 1) && aq->looping;
                        anim.script_controlled = true;
                        anim.script_clip_name = aq->clips.front();
                        set_anim_state(anim, AnimState::Custom, 0, /*force_restart=*/true);
                    } else {
                        log::warn(TAG, "SetUnitAnimation: clip '{}' not found on model '{}'",
                                  aq->clips.front(), anim.model->name);
                        world.anim_queues.remove(id);
                        aq = nullptr;
                    }
                }
            } else if (anim.script_controlled) {
                // Queue cleared without an explicit transition — let
                // derive_anim_state pick a fresh engine state next
                // (Walk / Idle / etc.) and set_anim_state crossfade
                // back from Custom.
                anim.script_controlled = false;
                anim.script_clip_name.clear();
            }

            if (!anim.script_controlled) {
                auto anim_info = derive_anim_state(world, id, anim);
                set_anim_state(anim, anim_info.state, anim_info.duration, anim_info.force_restart,
                               anim_info.has_attack_info ? &anim_info.attack_info : nullptr);
            }
            update_animation(anim, frame_dt);

            // Advance the queue if the script-driven clip just finished.
            if (anim.script_controlled && anim.finished && !anim.script_looping && aq) {
                aq->clips.pop_front();
                if (aq->clips.empty()) {
                    world.anim_queues.remove(id);
                    // script_controlled flips false on the next frame's
                    // pass, where derive_anim_state picks Idle/Walk and
                    // set_anim_state can crossfade out of Custom.
                } else {
                    i32 next_idx = find_clip_by_name(*anim.model, aq->clips.front());
                    if (next_idx >= 0) {
                        anim.state_to_clip[static_cast<u8>(AnimState::Custom)] = next_idx;
                        anim.script_looping = (aq->clips.size() == 1) && aq->looping;
                        anim.time = 0;
                        anim.finished = false;
                    } else {
                        log::warn(TAG, "QueueUnitAnimation: clip '{}' not found",
                                  aq->clips.front());
                        world.anim_queues.remove(id);
                    }
                }
            }

            evaluate_animation(anim);
        }
    }

    // Pass 1: Draw skinned units with skinned pipeline
    if (has_skinned_pipeline) {
        cmd.bind_pipeline(m_skinned_mesh_pipeline);
        cmd.set_viewport(0, 0, vw, vh);
        cmd.set_scissor(0, 0, extent.width, extent.height);

        auto draw_frustum = m_camera.frustum();
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

            // Frustum cull: same bounding-sphere test the static draw
            // batches do. The skinned path was missing this and was
            // re-binding + re-uploading bones for off-screen units.
            f32 cull_radius = lm->mesh.bounding_radius * transform->scale;
            if (!draw_frustum.is_sphere_visible(transform->interp_position(alpha),
                                                cull_radius)) continue;

            auto it = m_anim_instances.find(id);
            if (it == m_anim_instances.end()) continue;
            auto& anim = it->second;
            if (!anim.bone_descriptor.is_valid()) continue;

            // Upload this entity's bone matrices to its own SSBO
            if (void* dst = m_rhi->mapped_ptr(anim.bone_buffer);
                dst && !anim.bone_matrices.empty()) {
                std::memcpy(dst, anim.bone_matrices.data(),
                            anim.bone_matrices.size() * sizeof(glm::mat4));
            }

            // Use corpse material only for placeholder models (no real texture)
            bool is_corpse = world.dead_states.has(id);
            bool has_own_texture = lm->diffuse_texture.texture.is_valid();
            auto& mat = (is_corpse && !has_own_texture) ? m_corpse_material : lm->material;
            if (mat.descriptor_set.is_valid() && m_shadow_desc_set.is_valid()) {
                rhi::DescriptorSetHandle sets[] = {
                    mat.descriptor_set,
                    m_shadow_desc_set,
                    anim.bone_descriptor,
                };
                cmd.bind_descriptor_sets(m_skinned_mesh_pipeline_layout, 0, std::span{sets, 3});
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
            cmd.push_constants(m_skinned_mesh_pipeline_layout, rhi::ShaderStage::Vertex,
                               0, sizeof(push), &push);
            f32 alpha_eff = effective_visual_alpha(world, id, renderable);
            if (is_in_fog_memory(world, id)) alpha_eff *= kFoggedMemoryAlpha;
            glm::vec4 visual{alpha_eff, 0.0f, 0.0f, 0.0f};
            cmd.push_constants(m_skinned_mesh_pipeline_layout, rhi::ShaderStage::Fragment,
                               sizeof(push), sizeof(visual), &visual);

            cmd.bind_vertex_buffer(0, lm->mesh.vertex_buffer);
            cmd.bind_index_buffer(lm->mesh.index_buffer, 0, rhi::IndexType::U32);
            cmd.draw_indexed(lm->mesh.index_count);
        }
    }

    // Pass 2: Draw non-skinned entities via mega buffer + bindless + indirect draw (Phase 14b)
    if (m_mesh_pipeline.is_valid() && !m_draw_groups.empty()) {
        cmd.bind_pipeline(m_mesh_pipeline);
        cmd.set_viewport(0, 0, vw, vh);
        cmd.set_scissor(0, 0, extent.width, extent.height);

        // Push vp matrix (same for all instances)
        cmd.push_constants(m_mesh_pipeline_layout, rhi::ShaderStage::Vertex,
                           0, sizeof(glm::mat4), &vp);

        // Bind mega vertex/index buffers once (all static meshes share them)
        cmd.bind_vertex_buffer(0, m_mega_vb);
        cmd.bind_index_buffer(m_mega_ib, 0, rhi::IndexType::U32);

        // Bind all descriptor sets once: bindless textures + shadow + instance SSBO
        const u32 fi = m_rhi->frame_index();
        rhi::DescriptorSetHandle sets[] = {
            m_bindless_set,
            m_shadow_desc_set,
            m_instance_desc_set[fi],
        };
        cmd.bind_descriptor_sets(m_mesh_pipeline_layout, 0, std::span{sets, 3});

        // Multi-draw indirect: one command per unique mesh geometry
        u32 draw_count = static_cast<u32>(m_draw_groups.size());
        cmd.draw_indexed_indirect(m_indirect_buffer[fi], 0, draw_count,
                                  sizeof(rhi::DrawIndexedIndirectCommand));
    }

    // ── Pass 3: Particles (alpha-blended, drawn last) ────────────────────
    if (m_particle_pipeline.is_valid()) {
        // Compute camera right/up from view matrix for billboarding
        glm::mat4 view = m_camera.view_matrix();
        glm::vec3 cam_right{view[0][0], view[1][0], view[2][0]};
        glm::vec3 cam_up{view[0][1], view[1][1], view[2][1]};

        // ── Water splash particles for units on shallow water ──────────
        if (m_terrain_data && m_water_params.water_mask != 0) {
            static f32 splash_timer = 0.0f;
            splash_timer += frame_dt;
            if (splash_timer >= 0.15f) {  // emit every ~150ms
                splash_timer = 0.0f;
                auto t_ids = transforms.ids();
                auto t_data = transforms.data();
                for (u32 i = 0; i < transforms.count(); ++i) {
                    u32 id = t_ids[i];
                    auto& tf = t_data[i];
                    // Skip units hidden by fog of war
                    if (is_fog_hidden(world, id, tf)) continue;
                    // Check if unit is moving
                    glm::vec3 delta = tf.position - tf.prev_position;
                    f32 speed_sq = delta.x * delta.x + delta.y * delta.y;
                    if (speed_sq < 1.0f) continue;  // standing still

                    // Check if on shallow water tile
                    auto tile = m_terrain_data->world_to_tile(tf.position.x, tf.position.y);
                    u32 tx = static_cast<u32>(tile.x);
                    u32 ty = static_cast<u32>(tile.y);
                    if (tx >= m_terrain_data->tiles_x || ty >= m_terrain_data->tiles_y) continue;

                    bool on_shallow = true;
                    for (u32 vy = ty; vy <= ty + 1 && on_shallow; ++vy)
                        for (u32 vx = tx; vx <= tx + 1 && on_shallow; ++vx) {
                            u8 layer = m_terrain_data->tile_layer[vy * m_terrain_data->verts_x() + vx];
                            on_shallow = (m_water_params.water_mask & (1u << layer)) != 0
                                      && (m_water_params.deep_mask & (1u << layer)) == 0;
                        }

                    if (on_shallow) {
                        glm::vec3 splash_pos = tf.position + glm::vec3{0, 0, 3.0f};
                        m_particles.burst(splash_pos, 5,
                            glm::vec4{0.7f, 0.78f, 0.85f, 0.3f},  // subtle blue-white
                            60.0f, 0.5f, 10.0f, -100.0f, ParticleSystem::SHAPE_DROPLET);
                    }
                }
            }
        }

        struct EffectCtx { const simulation::World* w; const Renderer* r; };
        EffectCtx ec{&world, this};
        m_effect_manager.update(frame_dt,
            [](simulation::Unit u, std::string_view attach, void* ctx) -> glm::vec3 {
                auto* c = static_cast<EffectCtx*>(ctx);
                auto* t = c->w->transforms.get(u.id);
                if (!t) return {0,0,0};
                glm::vec3 pos = t->position;
                if (!attach.empty()) {
                    pos += c->r->get_attachment_point(u.id, attach) * t->scale;
                }
                return pos;
            }, &ec);

        m_particles.update(frame_dt);
        m_particles.upload(cam_right, cam_up);

        cmd.bind_pipeline(m_particle_pipeline);
        cmd.set_viewport(0, 0, vw, vh);
        cmd.set_scissor(0, 0, extent.width, extent.height);

        glm::mat4 particle_vp = vp;
        cmd.push_constants(m_particle_pipeline_layout, rhi::ShaderStage::Vertex,
                           0, sizeof(glm::mat4), &particle_vp);

        m_particles.draw(cmd);
    }
}

} // namespace uldum::render
