#version 450

// HUD text (MSDF) fragment shader.
// Samples a multi-channel signed-distance-field atlas and resolves the
// per-fragment signed distance by taking the median of the 3 color
// channels. Anti-aliasing uses fwidth() to get the per-pixel rate of
// change of the signed distance, which is exactly the width of one
// fragment in distance-space.
//
// The vertex color is premultiplied-alpha. The shader multiplies all
// four components by coverage so the premul blend equation
// (ONE + ONE_MINUS_SRC_ALPHA) produces correct partial coverage.

layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) out vec4 out_color;

float median3(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

void main() {
    vec3 msdf = texture(u_tex, in_uv).rgb;
    float sd = median3(msdf.r, msdf.g, msdf.b) - 0.5;
    float px = fwidth(sd);
    float coverage = smoothstep(-px, px, sd);
    out_color = in_color * coverage;
}
