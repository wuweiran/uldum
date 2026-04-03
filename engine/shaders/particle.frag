#version 450

layout(location = 0) in vec4 frag_color;
layout(location = 1) in vec2 frag_texcoord;

layout(location = 0) out vec4 out_color;

void main() {
    // Soft circle falloff — no texture needed
    vec2 center = frag_texcoord - vec2(0.5);
    float dist = length(center) * 2.0;  // 0 at center, 1 at edge
    float alpha = frag_color.a * smoothstep(1.0, 0.6, dist);

    if (alpha < 0.01) discard;

    out_color = vec4(frag_color.rgb, alpha);
}
