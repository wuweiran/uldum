#version 310 es
precision highp float;

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;

layout(binding = 30, std140) uniform PC {
    mat4 mvp;
} pc;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_uv;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 0.0, 1.0);
    out_color   = in_color;
    out_uv      = in_uv;
}
