#version 450

// Shadow depth pass with instancing.
// Push constant holds light_vp (same for all instances).
// Instance buffer holds per-entity InstanceData (model matrix + material index).

layout(push_constant) uniform PushConstants {
    mat4 light_vp;
} pc;

struct InstanceData {
    mat4 model;
    uint material_index;
    uint _pad1, _pad2, _pad3;
};

layout(set = 0, binding = 0) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;    // unused but must match vertex layout
layout(location = 2) in vec2 in_texcoord;  // unused but must match vertex layout

void main() {
    mat4 model = instances[gl_InstanceIndex].model;
    gl_Position = pc.light_vp * model * vec4(in_position, 1.0);
}
