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
layout(location = 3) out vec3 frag_world_pos;

void main() {
    vec4 world_pos = pc.model * vec4(in_position, 1.0);
    gl_Position = pc.mvp * vec4(in_position, 1.0);
    frag_world_normal = mat3(pc.model) * in_normal;
    frag_texcoord = in_texcoord;
    frag_tile_uv = in_position.xy * 0.5;
    frag_world_pos = world_pos.xyz;
}
