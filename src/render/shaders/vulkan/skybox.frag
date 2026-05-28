#version 450

layout(set = 0, binding = 0) uniform samplerCube skybox_tex;

layout(location = 0) in vec3 frag_dir;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(skybox_tex, frag_dir);
}
