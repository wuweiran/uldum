#version 450

layout(push_constant) uniform PushConstants {
    mat4 vp;    // view-projection (same for all instances)
} pc;

// Per-instance data. Must mirror Renderer::InstanceData (renderer.h).
struct InstanceData {
    mat4  model;                // 64
    vec4  base_color_factor;    // 16
    uint  material_index;       //  4
    float alpha;                //  4
    float alpha_cutoff;         //  4
    uint  _pad;                 //  4
};

layout(set = 2, binding = 0) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_texcoord;

layout(location = 0) out vec3  frag_world_normal;
layout(location = 1) out vec2  frag_texcoord;
layout(location = 2) out vec3  frag_world_pos;
layout(location = 3) flat out uint  frag_material_index;
layout(location = 4)      out float frag_alpha;
layout(location = 5)      out vec4  frag_base_color_factor;
layout(location = 6) flat out float frag_alpha_cutoff;

void main() {
    InstanceData inst = instances[gl_InstanceIndex];
    vec4 world_pos = inst.model * vec4(in_position, 1.0);
    gl_Position = pc.vp * world_pos;
    frag_world_normal = mat3(inst.model) * in_normal;
    frag_texcoord = in_texcoord;
    frag_world_pos = world_pos.xyz;
    frag_material_index = inst.material_index;
    frag_alpha = inst.alpha;
    frag_base_color_factor = inst.base_color_factor;
    frag_alpha_cutoff = inst.alpha_cutoff;
}
