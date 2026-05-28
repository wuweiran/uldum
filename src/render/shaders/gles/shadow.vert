#version 310 es
precision highp float;
precision highp int;

// Bindings — flat scheme: set N * 16 + binding M (push UBO at 30).
//   set 0 binding 0 → 0   InstanceBuffer SSBO
//   push constants  → 30

layout(binding = 30, std140) uniform PushConstants {
    mat4 light_vp;
} pc;

struct InstanceData {
    mat4 model;
    uint material_index;
    uint _pad1, _pad2, _pad3;
};

layout(binding = 0, std430) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;    // unused, matches vertex layout
layout(location = 2) in vec2 in_texcoord;  // unused, matches vertex layout

void main() {
    mat4 model = instances[gl_InstanceID].model;
    gl_Position = pc.light_vp * model * vec4(in_position, 1.0);
}
