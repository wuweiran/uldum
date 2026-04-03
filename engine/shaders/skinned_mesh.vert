#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
} pc;

// Bone matrices SSBO (set 2)
layout(set = 2, binding = 0) readonly buffer BoneMatrices {
    mat4 bones[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) in uvec4 in_bone_indices;
layout(location = 4) in vec4 in_bone_weights;

layout(location = 0) out vec3 frag_world_normal;
layout(location = 1) out vec2 frag_texcoord;
layout(location = 2) out vec3 frag_world_pos;

void main() {
    // GPU skinning: blend 4 bone influences
    mat4 skin_matrix = in_bone_weights.x * bones[in_bone_indices.x]
                     + in_bone_weights.y * bones[in_bone_indices.y]
                     + in_bone_weights.z * bones[in_bone_indices.z]
                     + in_bone_weights.w * bones[in_bone_indices.w];

    vec4 skinned_pos = skin_matrix * vec4(in_position, 1.0);
    vec3 skinned_normal = mat3(skin_matrix) * in_normal;

    vec4 world_pos = pc.model * skinned_pos;
    gl_Position = pc.mvp * skinned_pos;
    frag_world_normal = mat3(pc.model) * skinned_normal;
    frag_texcoord = in_texcoord;
    frag_world_pos = world_pos.xyz;
}
