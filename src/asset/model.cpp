#include "asset/model.h"

#include <cgltf.h>
#include <stb_image.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <filesystem>
#include <format>
#include <unordered_map>

namespace uldum::asset {

static glm::vec3 read_vec3(const cgltf_accessor* acc, cgltf_size index) {
    glm::vec3 v{0.0f};
    cgltf_accessor_read_float(acc, index, &v.x, 3);
    return v;
}

static glm::vec2 read_vec2(const cgltf_accessor* acc, cgltf_size index) {
    glm::vec2 v{0.0f};
    cgltf_accessor_read_float(acc, index, &v.x, 2);
    return v;
}

static glm::vec4 read_vec4(const cgltf_accessor* acc, cgltf_size index) {
    glm::vec4 v{0.0f};
    cgltf_accessor_read_float(acc, index, &v.x, 4);
    return v;
}

static glm::quat read_quat(const cgltf_accessor* acc, cgltf_size index) {
    float v[4]{0, 0, 0, 1};
    cgltf_accessor_read_float(acc, index, v, 4);
    return glm::quat(v[3], v[0], v[1], v[2]); // glm: w,x,y,z
}

static glm::mat4 read_mat4(const cgltf_accessor* acc, cgltf_size index) {
    glm::mat4 m{1.0f};
    cgltf_accessor_read_float(acc, index, &m[0][0], 16);
    return m;
}

// ── Skeleton extraction ───────────────────────────────────────────────────

// Build a mapping from cgltf_node* to bone index
using NodeToBoneMap = std::unordered_map<const cgltf_node*, u32>;

static void extract_skeleton(const cgltf_skin& skin,
                              Skeleton& skeleton, NodeToBoneMap& node_to_bone) {
    skeleton.bones.resize(skin.joints_count);

    // Map joint nodes to bone indices
    for (cgltf_size i = 0; i < skin.joints_count; ++i) {
        node_to_bone[skin.joints[i]] = static_cast<u32>(i);
    }

    // Read inverse bind matrices
    const cgltf_accessor* ibm = skin.inverse_bind_matrices;

    for (cgltf_size i = 0; i < skin.joints_count; ++i) {
        auto& bone = skeleton.bones[i];
        const cgltf_node* node = skin.joints[i];

        bone.name = node->name ? node->name : std::format("bone_{}", i);
        bone.parent_index = -1;

        // Find parent bone (walk up the node tree until we find a joint)
        const cgltf_node* parent = node->parent;
        while (parent) {
            auto it = node_to_bone.find(parent);
            if (it != node_to_bone.end()) {
                bone.parent_index = static_cast<i32>(it->second);
                break;
            }
            parent = parent->parent;
        }

        if (ibm) {
            bone.inverse_bind_matrix = read_mat4(ibm, i);
        }

        // Extract rest pose from node's TRS
        if (node->has_translation) {
            bone.rest_translation = {node->translation[0], node->translation[1], node->translation[2]};
        }
        if (node->has_rotation) {
            // cgltf stores quaternion as x,y,z,w; glm::quat is w,x,y,z
            bone.rest_rotation = glm::quat(node->rotation[3], node->rotation[0],
                                            node->rotation[1], node->rotation[2]);
        }
        if (node->has_scale) {
            bone.rest_scale = {node->scale[0], node->scale[1], node->scale[2]};
        }
    }
}

// ── Animation extraction ──────────────────────────────────────────────────

static void extract_animations(const cgltf_data* data, const NodeToBoneMap& node_to_bone,
                                std::vector<AnimationClip>& clips) {
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation& anim = data->animations[ai];

        AnimationClip clip;
        clip.name = anim.name ? anim.name : std::format("anim_{}", ai);
        clip.duration = 0;

        for (cgltf_size ci = 0; ci < anim.channels_count; ++ci) {
            const cgltf_animation_channel& ch = anim.channels[ci];
            if (!ch.target_node) continue;

            auto it = node_to_bone.find(ch.target_node);
            if (it == node_to_bone.end()) continue;

            const cgltf_animation_sampler& sampler = *ch.sampler;
            const cgltf_accessor* input  = sampler.input;   // timestamps
            const cgltf_accessor* output = sampler.output;  // values

            // Find or create channel for this bone
            AnimationChannel* target_channel = nullptr;
            for (auto& existing : clip.channels) {
                if (existing.bone_index == it->second) {
                    target_channel = &existing;
                    break;
                }
            }
            if (!target_channel) {
                clip.channels.push_back({});
                target_channel = &clip.channels.back();
                target_channel->bone_index = it->second;
            }

            // Read timestamps (shared across TRS — only read once)
            if (target_channel->timestamps.empty()) {
                target_channel->timestamps.resize(input->count);
                for (cgltf_size k = 0; k < input->count; ++k) {
                    cgltf_accessor_read_float(input, k, &target_channel->timestamps[k], 1);
                }
            }

            // Track max duration
            if (input->count > 0) {
                float last_time = 0;
                cgltf_accessor_read_float(input, input->count - 1, &last_time, 1);
                if (last_time > clip.duration) clip.duration = last_time;
            }

            // Read keyframe values
            switch (ch.target_path) {
            case cgltf_animation_path_type_translation:
                target_channel->translations.resize(output->count);
                for (cgltf_size k = 0; k < output->count; ++k) {
                    target_channel->translations[k] = read_vec3(output, k);
                }
                break;
            case cgltf_animation_path_type_rotation:
                target_channel->rotations.resize(output->count);
                for (cgltf_size k = 0; k < output->count; ++k) {
                    target_channel->rotations[k] = read_quat(output, k);
                }
                break;
            case cgltf_animation_path_type_scale:
                target_channel->scales.resize(output->count);
                for (cgltf_size k = 0; k < output->count; ++k) {
                    target_channel->scales[k] = read_vec3(output, k);
                }
                break;
            default: break;
            }
        }

        clips.push_back(std::move(clip));
    }
}

// ── Texture extraction ───────────────────────────────────────────────────

static void extract_textures(const cgltf_data* data, std::string_view model_path,
                              std::vector<TextureData>& textures) {
    namespace fs = std::filesystem;
    fs::path base_dir = fs::path(model_path).parent_path();

    for (cgltf_size i = 0; i < data->images_count; ++i) {
        const cgltf_image& img = data->images[i];

        int w = 0, h = 0, channels = 0;
        u8* pixels = nullptr;

        if (img.buffer_view) {
            // Embedded image (typical for .glb)
            const u8* buf = static_cast<const u8*>(img.buffer_view->buffer->data)
                            + img.buffer_view->offset;
            int len = static_cast<int>(img.buffer_view->size);
            pixels = stbi_load_from_memory(buf, len, &w, &h, &channels, 4);
        } else if (img.uri) {
            // External image file relative to model directory
            fs::path img_path = base_dir / img.uri;
            std::string img_str = img_path.string();
            pixels = stbi_load(img_str.c_str(), &w, &h, &channels, 4);
        }

        if (pixels) {
            TextureData tex;
            tex.width    = static_cast<u32>(w);
            tex.height   = static_cast<u32>(h);
            tex.channels = 4;
            tex.pixels.assign(pixels, pixels + w * h * 4);
            stbi_image_free(pixels);
            textures.push_back(std::move(tex));
        }
    }
}

// ── Model loading ─────────────────────────────────────────────────────────

std::expected<ModelData, std::string> load_model(std::string_view path) {
    std::string path_str(path);

    cgltf_options options{};
    cgltf_data* data = nullptr;

    cgltf_result result = cgltf_parse_file(&options, path_str.c_str(), &data);
    if (result != cgltf_result_success) {
        return std::unexpected(std::format("Failed to parse glTF '{}': error {}", path, static_cast<int>(result)));
    }

    result = cgltf_load_buffers(&options, data, path_str.c_str());
    if (result != cgltf_result_success) {
        cgltf_free(data);
        return std::unexpected(std::format("Failed to load glTF buffers '{}': error {}", path, static_cast<int>(result)));
    }

    ModelData model;
    model.name = path_str;

    // Extract textures from glTF images
    extract_textures(data, path, model.textures);

    // Extract skeleton from first skin (if any)
    NodeToBoneMap node_to_bone;
    bool has_skin = data->skins_count > 0;
    if (has_skin) {
        extract_skeleton(data->skins[0], model.skeleton, node_to_bone);
        extract_animations(data, node_to_bone, model.animations);
    }

    // Extract meshes by walking the node tree (not iterating meshes directly).
    // This detects bone-parented meshes (e.g., weapons) and converts them to
    // skinned meshes with 100% weight on the parent bone.

    // Helper: get a node's local transform as a mat4
    auto node_local_transform = [](const cgltf_node* node) -> glm::mat4 {
        cgltf_float mat[16];
        cgltf_node_transform_local(node, mat);
        return glm::make_mat4(mat);
    };

    // Helper: find which bone a node is parented to.
    // Returns bone index and the node's local transform (relative to bone parent).
    struct BoneParentInfo { i32 bone_index = -1; glm::mat4 transform{1.0f}; };
    auto find_parent_bone = [&](const cgltf_node* node) -> BoneParentInfo {
        const cgltf_node* cur = node->parent;
        while (cur) {
            auto it = node_to_bone.find(cur);
            if (it != node_to_bone.end()) {
                // Just use the weapon node's local transform — it encodes the
                // offset from the bone. The skinning shader handles the rest.
                return {static_cast<i32>(it->second), node_local_transform(node)};
            }
            cur = cur->parent;
        }
        return {};
    };

    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;
        const cgltf_mesh& mesh = *node.mesh;

        // Determine if this node is bone-parented (unskinned mesh attached to skeleton)
        auto bone_info = has_skin ? find_parent_bone(&node) : BoneParentInfo{};

        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* pos_acc    = nullptr;
            const cgltf_accessor* norm_acc   = nullptr;
            const cgltf_accessor* uv_acc     = nullptr;
            const cgltf_accessor* joints_acc = nullptr;
            const cgltf_accessor* weights_acc = nullptr;

            for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
                const auto& attr = prim.attributes[ai];
                if (attr.type == cgltf_attribute_type_position)  pos_acc     = attr.data;
                if (attr.type == cgltf_attribute_type_normal)    norm_acc    = attr.data;
                if (attr.type == cgltf_attribute_type_texcoord)  uv_acc      = attr.data;
                if (attr.type == cgltf_attribute_type_joints)    joints_acc  = attr.data;
                if (attr.type == cgltf_attribute_type_weights)   weights_acc = attr.data;
            }

            if (!pos_acc) continue;

            cgltf_size vertex_count = pos_acc->count;

            std::vector<u32> indices;
            if (prim.indices) {
                indices.resize(prim.indices->count);
                for (cgltf_size ii = 0; ii < prim.indices->count; ++ii) {
                    indices[ii] = static_cast<u32>(cgltf_accessor_read_index(prim.indices, ii));
                }
            }

            std::string mesh_name = mesh.name ? mesh.name : std::format("mesh_{}", ni);

            if (has_skin && joints_acc && weights_acc) {
                // Properly skinned mesh
                SkinnedMeshData smd;
                smd.name = mesh_name;
                smd.indices = std::move(indices);
                smd.vertices.resize(vertex_count);

                for (cgltf_size vi = 0; vi < vertex_count; ++vi) {
                    auto& v = smd.vertices[vi];
                    v.position = read_vec3(pos_acc, vi);
                    if (norm_acc) v.normal   = read_vec3(norm_acc, vi);
                    if (uv_acc)   v.texcoord = read_vec2(uv_acc, vi);

                    cgltf_uint joint_vals[4]{};
                    cgltf_accessor_read_uint(joints_acc, vi, joint_vals, 4);
                    v.bone_indices = {joint_vals[0], joint_vals[1], joint_vals[2], joint_vals[3]};
                    v.bone_weights = read_vec4(weights_acc, vi);
                }

                model.skinned_meshes.push_back(std::move(smd));
            } else if (bone_info.bone_index >= 0) {
                // Unskinned mesh parented to a bone — convert to skinned.
                // The skinning shader computes: bone_global * inverse_bind * vertex.
                // Regular skinned vertices are in bind-pose (model) space.
                // The weapon's vertices are in the node's local space. To place them
                // in bind-pose space: bind_matrix * node_local * vertex
                // where bind_matrix = inverse(inverse_bind_matrix).
                glm::mat4 bind_matrix = glm::inverse(model.skeleton.bones[bone_info.bone_index].inverse_bind_matrix);
                glm::mat4 to_bind_space = bind_matrix * bone_info.transform;
                glm::mat3 to_bind_normal = glm::transpose(glm::inverse(glm::mat3(to_bind_space)));

                SkinnedMeshData smd;
                smd.name = mesh_name;
                smd.indices = std::move(indices);
                smd.vertices.resize(vertex_count);

                for (cgltf_size vi = 0; vi < vertex_count; ++vi) {
                    auto& v = smd.vertices[vi];
                    glm::vec3 pos = read_vec3(pos_acc, vi);
                    v.position = glm::vec3(to_bind_space * glm::vec4(pos, 1.0f));
                    if (norm_acc) {
                        glm::vec3 n = read_vec3(norm_acc, vi);
                        v.normal = glm::normalize(to_bind_normal * n);
                    }
                    if (uv_acc) v.texcoord = read_vec2(uv_acc, vi);

                    v.bone_indices = {static_cast<u32>(bone_info.bone_index), 0, 0, 0};
                    v.bone_weights = {1.0f, 0.0f, 0.0f, 0.0f};
                }

                model.skinned_meshes.push_back(std::move(smd));
            } else {
                // Non-skinned, not bone-parented
                MeshData md;
                md.name = mesh_name;
                md.indices = std::move(indices);
                md.vertices.resize(vertex_count);

                for (cgltf_size vi = 0; vi < vertex_count; ++vi) {
                    auto& v = md.vertices[vi];
                    v.position = read_vec3(pos_acc, vi);
                    if (norm_acc) v.normal   = read_vec3(norm_acc, vi);
                    if (uv_acc)   v.texcoord = read_vec2(uv_acc, vi);
                }

                model.meshes.push_back(std::move(md));
            }
        }
    }

    cgltf_free(data);
    return model;
}

} // namespace uldum::asset
