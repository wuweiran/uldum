#version 450

layout(push_constant) uniform PushConstants {
    mat4 vp;    // view-projection (same for all instances)
} pc;

// Per-instance data (Phase 14b: model matrix + material index)
struct InstanceData {
    mat4 model;
    uint material_index;
    uint _pad1, _pad2, _pad3;
};

layout(set = 2, binding = 0) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_texcoord;

layout(location = 0) out vec3 frag_world_normal;
layout(location = 1) out vec2 frag_texcoord;
layout(location = 2) out vec3 frag_world_pos;
layout(location = 3) flat out uint frag_material_index;

void main() {
    InstanceData inst = instances[gl_InstanceIndex];
    vec4 world_pos = inst.model * vec4(in_position, 1.0);
    gl_Position = pc.vp * world_pos;
    frag_world_normal = mat3(inst.model) * in_normal;
    frag_texcoord = in_texcoord;
    frag_world_pos = world_pos.xyz;
    frag_material_index = inst.material_index;
}
