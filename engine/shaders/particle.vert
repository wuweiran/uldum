#version 450

// Particle vertex: CPU generates billboard quads in world space.
// Push constants hold view-projection matrix.

layout(push_constant) uniform PushConstants {
    mat4 vp;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) in uint in_texture_id;

layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec2 frag_texcoord;
layout(location = 2) flat out uint frag_texture_id;

void main() {
    gl_Position = pc.vp * vec4(in_position, 1.0);
    frag_color = in_color;
    frag_texcoord = in_texcoord;
    frag_texture_id = in_texture_id;
}
