#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
    vec2 world_size;
    float fog_enabled;
    float _pad;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_texcoord;
layout(location = 3) in uint in_layer_corners;
layout(location = 4) in uint in_case_info;

layout(location = 0) out vec3 frag_world_normal;
layout(location = 1) out vec2 frag_texcoord;
layout(location = 2) out vec2 frag_tile_uv;
layout(location = 3) out vec3 frag_world_pos;
layout(location = 4) flat out uint frag_layer_corners;
layout(location = 5) flat out uint frag_case_info;
layout(location = 6) out vec2 frag_fog_uv;

void main() {
    vec4 world_pos = pc.model * vec4(in_position, 1.0);
    gl_Position = pc.mvp * vec4(in_position, 1.0);
    frag_world_normal = mat3(pc.model) * in_normal;
    frag_texcoord = in_texcoord;
    frag_tile_uv = in_position.xy / 128.0;
    frag_world_pos = world_pos.xyz;
    frag_layer_corners = in_layer_corners;
    frag_case_info = in_case_info;
    frag_fog_uv = world_pos.xy / pc.world_size;
}
