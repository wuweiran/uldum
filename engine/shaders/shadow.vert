#version 450

// Shadow depth pass: render from light's perspective.
// Push constants hold light_vp * model for the current object.

layout(push_constant) uniform PushConstants {
    mat4 light_mvp;
} pc;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;    // unused but must match vertex layout
layout(location = 2) in vec2 in_texcoord;  // unused but must match vertex layout

void main() {
    gl_Position = pc.light_mvp * vec4(in_position, 1.0);
}
