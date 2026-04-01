#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_texcoord;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec3 frag_color;

void main() {
    gl_Position = pc.mvp * vec4(in_position, 1.0);
    frag_normal = in_normal;
    // Simple coloring based on normal direction until we have textures
    frag_color = in_normal * 0.5 + 0.5;
}
