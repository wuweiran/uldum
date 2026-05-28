#version 450

// HUD text fragment shader.
// Atlas is single-channel R8 alpha coverage from FreeType's normal
// renderer. The vertex color is premultiplied-alpha, so we multiply all
// four components by coverage and let the premul blend equation
// (ONE + ONE_MINUS_SRC_ALPHA) produce correct partial coverage.

layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) out vec4 out_color;

void main() {
    float coverage = texture(u_tex, in_uv).r;
    out_color = in_color * coverage;
}
