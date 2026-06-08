#version 310 es
precision highp float;
precision highp int;

layout(binding = 30, std140) uniform PushConstants {
    mat4 vp;
} pc;

// Per-instance data — must mirror Renderer::InstanceData (renderer.h).
struct InstanceData {
    mat4  model;                // 64
    vec4  base_color_factor;    // 16
    uint  material_index;       //  4
    float alpha;                //  4
    float alpha_cutoff;         //  4
    uint  _pad;                 //  4
};

// SSBO uses a dense binding namespace on GLES (binding=0 across all
// pipelines that have an SSBO). The CPU side packs SSBO bindings into
// the descriptor's `binding` value directly (no set*N offset) because
// the GLES SSBO binding pool is tiny — only 4 slots guaranteed by
// ES 3.1 spec — so the flat formula used for UBOs/samplers would
// overflow it. See command_list.cpp::apply_descriptor_bindings.
layout(binding = 0, std430) readonly buffer InstanceBuffer {
    InstanceData instances[];
};

// GLES baseInstance emulation. GLES 3.1's gl_InstanceID resets to 0
// for each indirect draw command — it doesn't include the indirect
// command's `firstInstance`. The Rhi writes this UBO between draws
// (see gles_rhi.cpp::set_base_instance) and the shader adds it to
// gl_InstanceID to recover the per-instance index. Vulkan code path
// doesn't bind this slot (gl_InstanceIndex covers it natively).
layout(binding = 31, std140) uniform DrawInfo {
    uint base_instance;
    uint _pad0, _pad1, _pad2;
} draw_info;

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
    InstanceData inst = instances[uint(gl_InstanceID) + draw_info.base_instance];
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
