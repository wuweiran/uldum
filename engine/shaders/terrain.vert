#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_texcoord;

layout(location = 0) out vec3 frag_world_normal;
layout(location = 1) out vec2 frag_texcoord;
layout(location = 2) out vec2 frag_tile_uv;

void main() {
    gl_Position = pc.mvp * vec4(in_position, 1.0);
    frag_world_normal = mat3(pc.model) * in_normal;

    // Splatmap UV: spans entire terrain (0-1)
    frag_texcoord = in_texcoord;

    // Tile UV: repeating texture coordinates based on world position.
    // Each tile gets one full texture repeat (tile_size = 2.0 world units).
    frag_tile_uv = in_position.xy * 0.5;
}
