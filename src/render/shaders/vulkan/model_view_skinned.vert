#version 450

// Model-viewer skinned mesh (64B vertex + bone SSBO). Same push constants as
// the static viewer vert; bones at set 1 (set 0 stays the diffuse texture, so
// the shared fragment shader samples set 0 for both pipelines).

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
} pc;

layout(set = 1, binding = 0) readonly buffer BoneMatrices {
    mat4 bones[];
};

layout(location = 0) in vec3  in_position;
layout(location = 1) in vec3  in_normal;
layout(location = 2) in vec2  in_texcoord;
layout(location = 3) in uvec4 in_bone_indices;
layout(location = 4) in vec4  in_bone_weights;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec2 frag_uv;

void main() {
    mat4 skin = in_bone_weights.x * bones[in_bone_indices.x]
              + in_bone_weights.y * bones[in_bone_indices.y]
              + in_bone_weights.z * bones[in_bone_indices.z]
              + in_bone_weights.w * bones[in_bone_indices.w];

    vec4 pos = skin * vec4(in_position, 1.0);
    gl_Position = pc.mvp * pos;
    frag_normal = mat3(pc.model) * (mat3(skin) * in_normal);
    frag_uv     = in_texcoord;
}
