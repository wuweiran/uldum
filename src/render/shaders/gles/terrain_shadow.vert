#version 310 es
precision highp float;
precision highp int;

layout(binding = 30, std140) uniform PushConstants {
    mat4 light_mvp;
} pc;

layout(location = 0) in vec3 in_position;

void main() {
    gl_Position = pc.light_mvp * vec4(in_position, 1.0);
}
