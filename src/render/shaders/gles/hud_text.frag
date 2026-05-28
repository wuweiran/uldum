#version 310 es
precision highp float;

layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;

// set 0 binding 0 → flat 0
layout(binding = 0) uniform sampler2D u_tex;

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
