#version 310 es
precision highp float;
precision highp int;

// Shadow pass for alphaMode=MASK primitives. Passes texcoord +
// per-instance material data so the fragment can alpha-test.

layout(binding = 30, std140) uniform PushConstants {
    mat4 light_vp;
} pc;

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
layout(location = 2) in vec2 in_texcoord;

layout(location = 0)      out vec2  frag_texcoord;
layout(location = 1) flat out uint  frag_material_index;
layout(location = 2)      out vec4  frag_base_color_factor;
layout(location = 3) flat out float frag_alpha_cutoff;

void main() {
    InstanceData inst = instances[uint(gl_InstanceID) + draw_info.base_instance];
    gl_Position = pc.light_vp * inst.model * vec4(in_position, 1.0);
    frag_texcoord = in_texcoord;
    frag_material_index = inst.material_index;
    frag_base_color_factor = inst.base_color_factor;
    frag_alpha_cutoff = inst.alpha_cutoff;
}
