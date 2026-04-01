#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_texcoord;

layout(location = 0) out vec3 frag_world_normal;
layout(location = 1) out vec3 frag_color;

void main() {
    gl_Position = pc.mvp * vec4(in_position, 1.0);

    // Transform normal to world space (using upper-left 3x3 of model matrix).
    // For uniform scale this is correct; non-uniform scale would need inverse transpose.
    frag_world_normal = mat3(pc.model) * in_normal;

    // Base color from texcoord (R=texcoord.x, G=texcoord.y, B derived).
    // Placeholder boxes use warm tones; terrain UVs (0-1) give green-brown tones.
    frag_color = vec3(in_texcoord.x, in_texcoord.y, 0.2);
}
