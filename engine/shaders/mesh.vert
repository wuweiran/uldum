#version 450

layout(push_constant) uniform PushConstants {
    mat4 vp;    // view-projection (same for all instances)
} pc;

// Set 2: per-instance model matrices
layout(set = 2, binding = 0) readonly buffer InstanceBuffer {
    mat4 models[];
} instances;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_texcoord;

layout(location = 0) out vec3 frag_world_normal;
layout(location = 1) out vec2 frag_texcoord;
layout(location = 2) out vec3 frag_world_pos;

void main() {
    mat4 model = instances.models[gl_InstanceIndex];
    vec4 world_pos = model * vec4(in_position, 1.0);
    gl_Position = pc.vp * world_pos;
    frag_world_normal = mat3(model) * in_normal;
    frag_texcoord = in_texcoord;
    frag_world_pos = world_pos.xyz;
}
