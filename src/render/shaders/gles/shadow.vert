#version 310 es
precision highp float;
precision highp int;

// Bindings — flat scheme: set N * 16 + binding M (push UBO at 30).
//   set 0 binding 0 → 0   InstanceBuffer SSBO
//   push constants  → 30

layout(binding = 30, std140) uniform PushConstants {
    mat4 light_vp;
} pc;

// Must mirror Renderer::InstanceData (renderer.h). Shadow pass only
// reads the model matrix, but the stride has to match so gl_InstanceID
// indexes the right element.
struct InstanceData {
    mat4  model;
    vec4  base_color_factor;
    uint  material_index;
    float alpha;
    float alpha_cutoff;
    uint  _pad;
};

layout(binding = 0, std430) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

// GLES baseInstance emulation — see mesh.vert's comment.
layout(binding = 31, std140) uniform DrawInfo {
    uint base_instance;
    uint _pad0, _pad1, _pad2;
} draw_info;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;    // unused
layout(location = 2) in vec2 in_texcoord;  // unused

void main() {
    mat4 model = instances[uint(gl_InstanceID) + draw_info.base_instance].model;
    gl_Position = pc.light_vp * model * vec4(in_position, 1.0);
}
