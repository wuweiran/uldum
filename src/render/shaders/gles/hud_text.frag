#version 310 es
precision highp float;

layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;

// set 0 binding 0 → flat 0
layout(binding = 0) uniform sampler2D u_tex;

layout(location = 0) out vec4 out_color;

void main() {
    float coverage = texture(u_tex, in_uv).r;
    out_color = in_color * coverage;
}
