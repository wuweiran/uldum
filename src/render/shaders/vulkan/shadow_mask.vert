#version 450

// Shadow pass for alphaMode=MASK primitives. Same vertex math as
// shadow.vert, but additionally passes the texcoord + per-instance
// material data so the fragment shader can run the alpha test.

layout(push_constant) uniform PushConstants {
    mat4 light_vp;
} pc;

// Must mirror Renderer::InstanceData (renderer.h).
struct InstanceData {
    mat4  model;
    vec4  base_color_factor;
    uint  material_index;
    float alpha;
    float alpha_cutoff;
    uint  _pad;
};

layout(set = 0, binding = 0) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;    // unused
layout(location = 2) in vec2 in_texcoord;

layout(location = 0)      out vec2  frag_texcoord;
layout(location = 1) flat out uint  frag_material_index;
layout(location = 2)      out vec4  frag_base_color_factor;
layout(location = 3) flat out float frag_alpha_cutoff;

void main() {
    InstanceData inst = instances[gl_InstanceIndex];
    gl_Position = pc.light_vp * inst.model * vec4(in_position, 1.0);
    frag_texcoord = in_texcoord;
    frag_material_index = inst.material_index;
    frag_base_color_factor = inst.base_color_factor;
    frag_alpha_cutoff = inst.alpha_cutoff;
}
