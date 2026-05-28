#version 450

layout(push_constant) uniform PushConstants {
    mat4 light_mvp;
} pc;

// Bone matrices SSBO (set 0 for shadow pipeline — no material set needed)
layout(set = 0, binding = 0) readonly buffer BoneMatrices {
    mat4 bones[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) in uvec4 in_bone_indices;
layout(location = 4) in vec4 in_bone_weights;

void main() {
    mat4 skin_matrix = in_bone_weights.x * bones[in_bone_indices.x]
                     + in_bone_weights.y * bones[in_bone_indices.y]
                     + in_bone_weights.z * bones[in_bone_indices.z]
                     + in_bone_weights.w * bones[in_bone_indices.w];

    vec4 skinned_pos = skin_matrix * vec4(in_position, 1.0);
    gl_Position = pc.light_mvp * skinned_pos;
}
