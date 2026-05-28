#version 310 es
precision highp float;

layout(binding = 30, std140) uniform PC {
    mat4 mvp;
    vec4 tint;
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    out_uv      = in_uv;
    out_color   = pc.tint * in_color;
}
