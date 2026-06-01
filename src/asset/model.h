#pragma once

#include "core/types.h"
#include "asset/texture.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace uldum::asset {

// Non-skinned vertex (32 bytes)
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 texcoord{0.0f};
};

// Skinned vertex with bone influences (64 bytes)
struct SkinnedVertex {
    glm::vec3  position{0.0f};
    glm::vec3  normal{0.0f, 1.0f, 0.0f};
    glm::vec2  texcoord{0.0f};
    glm::uvec4 bone_indices{0};
    glm::vec4  bone_weights{0.0f};
};

// glTF alphaMode — drives the renderer's pipeline + shader path per primitive.
//   Opaque: alpha channel ignored, no discard, no blend
//   Mask:   `if (sample.a < alpha_cutoff) discard;` — foliage, decals, cut-outs
//   Blend:  fully transparent (depth-sorted) — not yet supported by the engine;
//           primitives flagged Blend currently render as Opaque with a warning.
enum class AlphaMode : u8 { Opaque, Mask, Blend };

// Per-primitive material data extracted from a glTF material. The renderer
// uses these to drive per-primitive draw state (texture binding, color
// multiplier, alpha-test pipeline variant, cull mode). A primitive with
// `material = -1` falls back to the engine's default: white texture,
// (1,1,1,1) base color, Opaque, single-sided.
struct MaterialDef {
    glm::vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
    i32       base_color_texture = -1;   // index into ModelData.textures; -1 = no texture
    f32       alpha_cutoff       = 0.5f; // MASK threshold
    AlphaMode alpha_mode         = AlphaMode::Opaque;
    bool      double_sided       = false;
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<u32>    indices;
    std::string         name;
    i32                 material = -1;   // index into ModelData.materials; -1 = default
};

struct SkinnedMeshData {
    std::vector<SkinnedVertex> vertices;
    std::vector<u32>           indices;
    std::string                name;
    i32                        material = -1;
};

// ── Skeleton ──────────────────────────────────────────────────────────────

struct Bone {
    std::string name;
    i32         parent_index = -1;          // -1 = root
    glm::mat4   inverse_bind_matrix{1.0f};
    // Rest pose (default TRS from glTF node)
    glm::vec3   rest_translation{0.0f};
    glm::quat   rest_rotation{1, 0, 0, 0};  // w,x,y,z
    glm::vec3   rest_scale{1.0f};
};

struct Skeleton {
    std::vector<Bone> bones;
    bool empty() const { return bones.empty(); }
};

// ── Animation ─────────────────────────────────────────────────────────────

struct AnimationChannel {
    u32 bone_index = 0;
    std::vector<f32>       timestamps;
    std::vector<glm::vec3> translations;
    std::vector<glm::quat> rotations;
    std::vector<glm::vec3> scales;
};

struct AnimationClip {
    std::string name;
    f32 duration = 0.0f;
    std::vector<AnimationChannel> channels;
};

// ── Model ─────────────────────────────────────────────────────────────────

struct ModelData {
    std::vector<MeshData>        meshes;
    std::vector<SkinnedMeshData> skinned_meshes;
    Skeleton                     skeleton;
    std::vector<AnimationClip>   animations;
    std::string                  name;
    std::vector<TextureData>     textures;   // extracted diffuse images (one per cgltf_image)
    std::vector<MaterialDef>     materials;  // one per cgltf_material; MeshData.material indexes here

    bool has_skeleton() const { return !skeleton.empty(); }
};

std::expected<ModelData, std::string> load_model(std::string_view path);
std::expected<ModelData, std::string> load_model_from_memory(const u8* data, u32 size, std::string_view source_path = "");

} // namespace uldum::asset
