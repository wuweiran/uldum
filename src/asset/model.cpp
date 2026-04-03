#include "asset/model.h"

#include <cgltf.h>

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

static void extract_skeleton(const cgltf_data* data, const cgltf_skin& skin,
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

    // Extract skeleton from first skin (if any)
    NodeToBoneMap node_to_bone;
    bool has_skin = data->skins_count > 0;
    if (has_skin) {
        extract_skeleton(data, data->skins[0], model.skeleton, node_to_bone);
        extract_animations(data, node_to_bone, model.animations);
    }

    // Extract meshes
    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& mesh = data->meshes[mi];

        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive& prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            // Find attribute accessors
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

            // Read indices
            std::vector<u32> indices;
            if (prim.indices) {
                indices.resize(prim.indices->count);
                for (cgltf_size ii = 0; ii < prim.indices->count; ++ii) {
                    indices[ii] = static_cast<u32>(cgltf_accessor_read_index(prim.indices, ii));
                }
            }

            std::string mesh_name = mesh.name ? mesh.name : std::format("mesh_{}", mi);

            // Skinned mesh if we have skeleton + joint/weight attributes
            if (has_skin && joints_acc && weights_acc) {
                SkinnedMeshData smd;
                smd.name = mesh_name;
                smd.indices = std::move(indices);
                smd.vertices.resize(vertex_count);

                for (cgltf_size vi = 0; vi < vertex_count; ++vi) {
                    auto& v = smd.vertices[vi];
                    v.position = read_vec3(pos_acc, vi);
                    if (norm_acc) v.normal   = read_vec3(norm_acc, vi);
                    if (uv_acc)   v.texcoord = read_vec2(uv_acc, vi);

                    // Joint indices (u16 or u8 in glTF → u32)
                    cgltf_uint joint_vals[4]{};
                    cgltf_accessor_read_uint(joints_acc, vi, joint_vals, 4);
                    v.bone_indices = {joint_vals[0], joint_vals[1], joint_vals[2], joint_vals[3]};

                    v.bone_weights = read_vec4(weights_acc, vi);
                }

                model.skinned_meshes.push_back(std::move(smd));
            } else {
                // Non-skinned mesh
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
