#version 310 es
precision highp float;

// set 0 binding 0 → flat 0
layout(binding = 0) uniform samplerCube skybox_tex;

layout(location = 0) in vec3 frag_dir;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(skybox_tex, frag_dir);
}
