#version 450

// Model-viewer static mesh (32B vertex). Isolated from the scene renderer:
// camera VP + model come from push constants, no instance SSBO.

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_texcoord;

layout(location = 0) out vec3 frag_normal;
layout(location = 1) out vec2 frag_uv;

void main() {
    gl_Position = pc.mvp * vec4(in_position, 1.0);
    frag_normal = mat3(pc.model) * in_normal;
    frag_uv     = in_texcoord;
}
