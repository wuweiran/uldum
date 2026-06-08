#version 310 es
precision highp float;
precision highp int;

// Push constants — full block must match between vert and frag so the
// std140 offsets line up with the C++ writes. visual sits at offset 128
// (after mvp + model = 128 bytes); vertex shader reads mvp/model only,
// fragment reads visual.
layout(binding = 30, std140) uniform PushConstants {
    mat4 mvp;
    mat4 model;
    vec4 visual;
} pc;

// Bone matrices SSBO. On GLES SSBOs live in their own dense binding
// namespace at binding=0 across all pipelines that use one — ES 3.1
// only guarantees 4 SSBO bindings, so the wider flat-formula used for
// UBOs/samplers would overflow on Mali/Adreno. See
// command_list.cpp::apply_descriptor_bindings.
layout(binding = 0, std430) readonly buffer BoneMatrices {
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
