#version 450

// Terrain shadow depth pass: same as shadow.vert but matches TerrainVertex layout (48 bytes).

layout(push_constant) uniform PushConstants {
    mat4 light_mvp;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;          // unused
layout(location = 2) in vec2 in_texcoord;        // unused
layout(location = 3) in vec4 in_splat_weights;   // unused

void main() {
    gl_Position = pc.light_mvp * vec4(in_position, 1.0);
}
